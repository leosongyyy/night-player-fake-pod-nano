# PLAYLIST_SCAN_SUMMARY — NightPlayer 播放列表扫描系统

> 本文档汇总 FakePodNanoNightPlayer 中**播放列表（PLST）扫描与存储系统**的设计、实现、调试与验证结果。
> 这是 NightPlayer 的核心设计之一：用户通过**目录 + `.pl` 播放列表文本文件**组织全部音乐，无歌手/流派/专辑等元数据习惯，纯目录和列表。
>
> ⚠ **2026-11-16 存储介质变更**：数据不再存 flash 的 `pldata` 分区，改存 **SD 卡文件
> `/sdcard/.ecov/pldata.bin`**（开机整块读进 PSRAM，扫描末尾 tmp+rename 原子写回；
> 实测写 620KB 只要 **95ms**，原来 flash 擦写 6.14s）。**PLST 格式与本文的扫描/字符串池
> 设计完全没变**，只换了后端；下文凡"写入 flash 分区/mmap"的说法请对照
> `PLDATA_ON_SDCARD.md` §14 阅读。

---

## 1. 目标与背景

- 设备：**ESP32-S31**（FakePodNano Pro V1，16MB Flash + 16MB Octal PSRAM），SD 卡（64GB，FAT32/exFAT）存放约 5000+ 首音频文件（MP3 / FLAC / OGG / WAV / m4a / aac）。
- 需求：开机扫描 SD 卡，把**目录结构**与**用户自建的 `.pl` 播放列表**持久化到独立 Flash 分区，运行期通过**内存映射只读**访问（省 RAM）。
- 数据源：`musics/` 下的音频文件 + 根目录/`playlists/` 下的 `.pl` 歌单文件。

### 设计取舍
- **纯内存映射读取**：播放列表数据留在 Flash，mmap 后直接指针访问，不占堆 RAM。
- **字符串池去重**：路径前缀共享，极大压缩空间（5697 首歌曲的路径字符串仅约 155KB）。
- **中断安全持久化**：通过 `playlist_store`（先擦头部→写数据→最后写头部 commit）实现。

---

## 2. 数据格式（PLST）

一份固定布局的二进制块，整体存入 `pldata` 分区的 `playlist_store` 数据区。

```
地址空间（约 4MB，偏移相对播放列表数据起点）：
  0x00       头部（0x30 字节，PLST magic）
  0x30       目录区（到 0x10000，约 65488 字节 ≈ 5457 个目录/歌单条目）
  0x10000    歌曲索引区（到 0x40000，约 192KB ≈ 24576 首）
  0x40000    字符串常量池（到 0x3f0000，约 3.7MB，null-terminated 串依次排列）
```

### 头部（0x30 字节，packed）
| 偏移 | 字段 | 说明 |
|---|---|---|
| 0x00 | magic[4] | `"PLST"` |
| 0x04 | dir_offset | 目录区偏移（固定 0x30） |
| 0x08 | dir_count (u16) | 实际目录/歌单数 |
| 0x0A | song_offset | 歌曲区偏移（固定 0x10000） |
| 0x0E | song_capacity | 歌曲区容量 |
| 0x12 | str_offset | 字符串池偏移（固定 0x40000） |
| 0x16 | str_len | 字符串池已用长度 |
| 0x1A~0x2F | reserved | 保留 |

### 目录条目（12 字节）
```
name_str(4) + song_start(4) + song_count(4)
```
- `name_str`：目录名 / 歌单名（含 .pl）在字符串池的偏移（相对池起点）。
- `song_start`：该目录第一首歌在歌曲数组的**下标**。
- `song_count`：该目录歌曲数。

### 歌曲条目（8 字节）
```
name_str(4) + path_str(4)
```
- `name_str`：文件名（含扩展名）在池偏移。
- `path_str`：父目录路径（不含 `/sdcard` 前缀）在池偏移。空路径用空串索引 0。

### 字符串池
- 首字节 `0x00` = 空字符串（索引 0）。
- 所有 `*_str` 字段都是**相对池起点**的偏移。
- 播放时拼接：`/sdcard/` + path + `/` + name。

---

## 3. 代码模块

| 文件 | 作用 |
|---|---|
| `core/storage/playlist_format.h` | PLST 格式定义（头部/目录/歌曲/字符串池地址常量） |
| `core/storage/playlist_builder.h/.c` | 扫描构建器（目录 + .pl 解析，构建到内存，可选写回） |
| `core/storage/playlist.h/.c` | 读取/解析 API（mmap 或内存 buffer） |
| `core/storage/playlist_store.h/.c` | 泛化持久化层（中断安全整体保存，见另文档） |
| `core/hardware/pldata.h/.c` | 4MB 数据分区底层读写 + mmap |
| `main/ui/boot_flow.c` | 开机流程 / 扫描界面：启动后台扫描任务、刷新进度、按结果切界面 |

### 关键接口
- `playlist_build_from_sd(stats)`：扫描 + 写回分区。
- `playlist_build_in_memory(stats, &buf, &size)`：仅扫描到内存（测试用，不写回）。
- `playlist_builder_probe_signature(&sig, &files)`：**开机快速指纹探针**，只遍历目录与文件名、不读音频内容、不写 flash，用于判断缓存是否仍匹配当前曲库。
- `playlist_store_get_meta(&meta)`：读取缓存头部的 SD 卡 ID / 曲库指纹 / 文件数。
- `playlist_init()` / `playlist_init_from_buffer(buf)`：解析（mmap / 内存）。
- `playlist_dir_count() / song_count() / dir_at() / song_at() / get_string() / build_path()`。

---

## 4. 扫描构建流程

**核心顺序（关键设计）**：每个目录递归处理时，**先 `.pl` 播放列表 → 再音乐文件 → 最后按名称进子目录**。从根目录开始即如此，天然形成目标顺序：

> **根目录 `收藏.pl` → 一堆音乐文件夹 → `playlists/` 下的一堆播放列表**

### 目录遍历
- 用**显式栈**（PSRAM 分配）替代递归，避免深目录栈溢出。
- `collect_dir()` 优先用 `readdir` 的 `d_type` 判断条目类型，只有 `DT_UNKNOWN` 才回退 `stat()`（详见 4.1），收集三类：
  - **音频文件**（`is_audio_file`：`.mp3` / `.flac` / `.ogg` / `.wav`，`CONFIG_AUDIO_ENABLE_AAC=y` 时再加 `.m4a` / `.aac`）
  - **`.pl` 播放列表**（`is_pl_file`）
  - **子目录**
- 目录/文件/歌单各自按**名称前 4 字节 + 大小写不敏感**排序（`name_key` + `strcasecmp`）。

### 音乐目录
- 每个含音频文件的目录生成一个目录条目，歌曲条目 `name`=文件名、`path`=父目录路径（共享）。

### `.pl` 播放列表解析（`scan_pl_file`）
- `.pl` 每行是一个相对 SD 根目录的路径，可能含空行/空白行。
- 逐行读取，`strip_line` 去 `\r\n` 和首尾空白，跳过空行。
- **按"文件名 + 路径前缀"拆分**存储：`name`=最后一个 `/` 后的部分，`path`=其前缀。
  - 方便列表显示文件名；同路径连续出现的歌共享 `path` 字符串。
- 每个 `.pl` 生成一个**播放列表目录条目**，`name` = 歌单文件名（含 `.pl`）。

---

## 4.1 开机自动发现（2026-11 反哺）

> 本节是 2026-11 从 hello_world 测试固件反哺回本项目的功能：**不再要求用户点「开始扫描」**，开机自动判断"缓存是否仍属于当前这张卡、曲库有没有增删/改名/换文件"。

### 缓存头部（`playlist_store` v2 → v3）

`playlist_store_header_t` 在原 magic/version/data_len/crc32 之后新增：

| 字段 | 说明 |
|---|---|
| `card_id` (u64) | SD 卡 CID 派生身份（mfg/oem/name/revision/serial/date 的 FNV-1a 64） |
| `signature` (u64) | 曲库指纹（见下） |
| `file_count` (u32) | 物理音频文件数（不含 `.pl` 内引用） |
| `reserved` (u32) | 0 |

**版本从 v1 升到 v2（自动发现），v3（多格式），v4（加 WAV），v5（加 m4a/aac）**：旧缓存因为格式范围不同，首次启动会判为无效并自动重扫一次，之后稳定。PLST 二进制布局与数据区偏移（0x1000）始终未变，无需迁移数据。

### 曲库指纹

- FNV-1a 64 位；对每个音频文件与 `.pl` 文件各算一个哈希，再**相加**合并（顺序无关，`readdir` 顺序变化不会造成误判）。
- 音频文件只哈希**相对路径**：播放列表只存路径/文件名，音频内容变化（例如重新编码但路径不变）不需要重建列表。
- `.pl` 额外哈希 `size + mtime`：歌单内容变化会影响歌曲集合，必须触发重建。
- 指纹探针与正式构建共用 `sig_add_file()`，保证两边算法一致。
- 遍历规则（隐藏项跳过、单目录条目上限 4096、最大深度 512、目录排序）也强制一致，否则会出现"每次开机都重扫"。

### 开机决策

`boot_flow_run()` 在 SD 挂载之后：

1. 缓存无效 / 元信息缺失 → 自动扫描；
2. `card_id` 与当前卡不同 → 清空旧卡的播放位置（`config_store_reset_position()`）→ 自动扫描；
3. 快速指纹探针与缓存记录不一致（`signature` 或 `file_count` 变化）→ 自动扫描；
4. 全部一致 → 直接进入常规界面，不扫描。

扫描失败（读卡/写分区失败或扫描任务创建失败）停在「扫描失败」页，不再带着空列表进入主界面；无音乐仍停在「未发现音乐」。

### `d_type` 优化（全量扫描与探针共用）

原实现每个目录条目都 `stat()` 判断类型。FatFs 的 `stat` 每次都要从头扫描目录，在扁平大目录（上千首）里整轮是 O(n²)。现在：

- 优先使用 `readdir` 的 `d_type`（ESP-IDF FatFs VFS 按 `FILINFO.fattrib` 填 `DT_DIR`/`DT_REG`）；
- 只有 `DT_UNKNOWN` 才回退 `stat()`；
- 音频文件不再逐文件 `stat`（指纹只哈希路径）；`.pl` 需要 size/mtime，单独 `stat()`。

实测（790 首、1 个目录）：**指纹探针约 161ms；全量扫描约 2.7s（含写 300KB 播放列表）**。

### 扫描任务栈

原代码调用 `xTaskCreatePinnedToCore(..., BOOT_SCAN_STACK_BYTES / 4u, ...)`，而 IDF 的栈单位是**字节**，32KB 常量实际只分配 8KB。现在改为真正的 32KB，失败回退 16KB；任务创建失败会删除进度定时器并显示「扫描失败」页。

---

## 5. 构建缓冲与写回

- 构建缓冲在 **PSRAM**（约 3.7MB）中组装，扫描完成后：
  - 写回模式：通过 `playlist_store_save_playlist(data, len, &meta)` 整体持久化，同时写入 `card_id / signature / file_count`（中断安全）。
  - 内存模式：直接返回 buffer 供内存解析（测试用，避免反复擦写 flash）。

---

## 6. 读取 API（内存映射，省 RAM）

- `playlist_init()`：校验分区有效 → `esp_partition_mmap` 整个分区 → 解析各分区指针。
- 目录/歌曲/字符串均为**只读指针**访问，数据留在 Flash，不占堆 RAM。
- `playlist_build_path()`：拼接 `/sdcard` + path + `/` + name 得到完整播放路径。
- `playlist_dir_is_pl(name)`：按结尾 `.pl`（忽略大小写）判断该目录条目是不是歌单，
  与 `dir_list.c` 的行图标判定、扫描器 `is_pl_file` 语义一致。
- `playlist_song_count_excluding_pl()`：**不含歌单目录**的歌曲总数（文件夹里的真实曲目数）。
  ⚠ 目录区里**文件夹目录与歌单目录是交错的**（每个目录先写本目录的 `.pl` 条目、再写音频
  文件条目），所以不能像 `playlist_song_count()` 那样只取最后一个目录的终点，必须逐目录
  累加并跳过 `.pl`。用途：设置菜单「重新扫描」行右侧显示当前曲库规模。
  实测同一张卡：`song_count()=8965`（含歌单引用）→ `song_count_excluding_pl()=5781`。

---

## 7. 关键踩坑与解决方案

| 问题 | 根因 | 解决 |
|---|---|---|
| **路径拼接错误**（`playlists/musics` 等） | 显式栈弹出 `node` 后，push 第一个 `child` 写回同一槽位，覆盖了 `node->path` | push 前先把父路径拷贝到局部变量 |
| **`esp_partition_mmap` 崩溃死循环** | mmap 需冻结 flash cache，**PSRAM 任务栈**访问会触发断言 | 构建任务改用**内部 RAM 栈** |
| **任务栈溢出**（加入 .pl 解析后） | `scan_pl_file` 多个 512 字节局部数组，16KB 栈不够 | 任务栈增至 32KB |
| monitor 中文/日文乱码或崩溃 | Python 终端按 GBK 解码 UTF-8 | 用户通过修改了idfc的源码修复 |
| 卡目录异常（`System Volume Information` 等残留） | chkdsk 残留 / 目录结构问题 | 用户在电脑上修复卡 |
| 隐藏目录混入 | 未过滤点开头 | `collect_dir` 跳过 `name[0]=='.'` |
| **扁平大目录扫描慢** | 每个条目 `stat()`，FatFs 每次都从头扫目录，O(n²) | 优先用 `d_type`，仅 `DT_UNKNOWN` 回退 `stat()`；音频不再逐文件 stat（2026-11） |
| **扫描任务栈实际只有 8KB** | `xTaskCreatePinnedToCore` 的栈单位是字节，调用处多除了 4 | 去掉 `/4u`，32KB 失败回退 16KB（2026-11） |

---

## 8. 验证结果（真机 COM12）

### 阶段一：纯音乐目录扫描
```
scan done: dirs=152 songs=5697 str_len=220556 total=482700 truncated=0
```
- 152 目录 / 5697 首歌，字符串池仅 215KB（路径共享生效），总数据 471KB，未超分区。

### 阶段二：加入 `.pl` 播放列表解析
```
scan dir='' files=0 pls=1 subdirs=3      ← 根目录：收藏.pl
playlist '收藏.pl': 116 songs
...
scan dir='playlists' files=0 pls=15 subdirs=0   ← playlists：15 个 .pl
playlist 'Eastshade歌单.pl': 11 songs
playlist 'G1.原神...': 611 songs
playlist 'G9.原神...': 1031 songs
...
scan done: dirs=168 songs=8828 str_len=349169 total=611313 truncated=0
[RB] dir[0] '收藏.pl' songs[0..116)      ← 根目录播放列表排最前 ✓
[RB] dir[1] 'bgm' songs[116..1201)      ← 然后音乐文件夹 ✓
```
- **168 目录 / 8828 首歌**（含播放列表引用），总数据 597KB，`truncated=0`。
- **顺序完全符合设计**：根目录 收藏.pl → 音乐文件夹 → playlists 下播放列表。
- **文件名/路径拆分正确**：回读只打印文件名（如 `Serra - ...mp3`），确认 `name`/`path` 分离。
- 扫描耗时约 **2.2 秒**（含全部播放列表解析）。

---

## 9. 当前状态与后续

### 已完成
- [x] PLST 格式定义
- [x] 目录 + `.pl` 播放列表扫描构建器（顺序正确、大小写不敏感排序；音频支持 MP3 / FLAC / OGG）
- [x] **多格式扫描（2026-11）**：`is_audio_file` 扩展为 `.mp3/.flac/.ogg/.wav`，pldata 版本升到 v4 触发旧缓存重扫
- [x] **AAC 扫描（2026-11）**：`is_audio_file` 再加 `.m4a/.aac`（受 `CONFIG_AUDIO_ENABLE_AAC` 控制），pldata 版本升到 **v5**，旧缓存自动重扫一次；探针与构建器共用该函数
- [x] 读取/解析 API（mmap 省 RAM）
- [x] 内存模式（仅扫描不写回）用于测试
- [x] 真机验证（168 目录 / 8828 首歌，无截断）
- [x] 播放列表 UI：目录/歌单列表 → 歌曲列表 → 播放
- [x] 持久化写回验证（`playlist_build_from_sd` 写回 pldata 后重启读取，开机流程实际启用）
- [x] **开机自动发现（2026-11）**：pldata v2 引入卡 ID / 曲库指纹 / 文件数，v3 随多格式扩展；开机自动判断换卡与曲库变化，无需用户点「开始扫描」
- [x] **`d_type` 扫描优化 + 扫描任务栈修复（2026-11）**

### 待办 / 规划
- [ ] （可选）扫描逻辑做成 Kconfig 开关
- [ ] APE / ALAC / Opus / HE-AAC 仍未支持（本次只做有损 AAC-LC）

---

*文档整理时间：NightPlayer 开发中期。*
