# 把 pldata 从 flash 分区搬到 SD 卡文件（可行性研究）

> 2026-11-15。**纯代码/文档研究，未烧录、未改任何源码**（用户要求"不动设备"）。
> 结论先说：**可行，且收益比预期大**；真正的代价不是内存或性能，而是
> "固件第一次开始写用户的音乐卡"这一类新风险。建议分三阶段落地（见 §10）。

---

## 0. TL;DR

| 问题 | 结论 |
| --- | --- |
| 能不能搬？ | **能**。`pldata` 的唯一使用者是 `playlist_store`，对外只有 8 个函数（`erase_all`/`get_size` 目前还没人调），改一个文件即可换掉后端 |
| 数据多大？ | 数据本体 **618,622 B**（5000+ 首）/ **620,134 B**（9042 首、173 目录）；卡上文件 = 4KB 头 + 本体 ≈ **624 KB**。分区 4MB 完全是浪费 |
| PSRAM 够吗？ | 够。稳态空闲 **16.09 MB**（`MEM: heap=`），常驻 624KB ≈ 3.9% |
| 读的代价 | 开机多一次 SD 读 620KB ≈ **30~150 ms**，相对开机已有的 1276ms 指纹探针可忽略 |
| 写的代价 | 扫描末尾把 620KB 写 SD ≈ **0.15~0.5 s**（对比现在擦写 flash **6.14 s**） |
| 最大收益 | ① 扫描写盘 6.1s → <0.5s；② **彻底消灭"扫描期间关外部缓存"这条杀手机的路径**（见 `NPF_FONT.md` §7.1）；③ 腾出 **4MB flash**（release 播放器只剩 354KB 余量）；④ 顺带解开"pldata 必须钉在分区表最末尾"这条布局枷锁 |
| 最大风险 | 固件**第一次向用户的音乐卡写数据**。断电/拔卡可能损坏 FAT/exFAT（丢簇、目录项损坏），最坏要 `chkdsk`。音乐文件本身不会被碰（只写自己那一个文件 + tmp + rename） |
| 推荐形态 | **文件内容与现在的 pldata 分区镜像逐字节相同**（4KB 头 + PLST 数据），开机整块读进 PSRAM，运行时仍是指针访问。代码里现有的 `playlist_init_from_buffer()` 就是这条路径 |
| 建议节奏 | P1 换后端（**保留 pldata 分区做一次性迁移/回退**）→ P2 去掉 LVGL 冻结、扫描栈搬 PSRAM → P3 删分区 + 重排布局 + 刷字体 |

---

## 1. 现状（这些东西到底在哪、谁在用）

### 1.1 分区与数据

```
partitions.csv：
  pldata  data 0x40  0xC00000  4M        ← 锚定在 flash 最末尾
```

文件布局（`playlist_store.h`）：

```
offset 0x0000  │ playlist_store 头部（占满 1 个 4KB 扇区）  │ commit 标志 + 元信息
offset 0x1000  │ PLST 数据本体（playlist_format.h 定义的固定布局） │ 最长 4MB-4KB
```

PLST 本体（`playlist_format.h`）是**固定布局的稀疏块**：

| 偏移 | 内容 | 实际用量（9042 首 / 173 目录） |
| --- | --- | --- |
| 0x00 | 头部 0x30 字节（magic `PLST`） | 48 B |
| 0x30 | 目录条目区（12B/个） | 173 × 12 = 2,076 B |
| 0x10000 | 歌曲索引区（8B/个） | 9042 × 8 = 72,336 B |
| —— | ↑ 0x30~0x40000 之间**未使用的填充** | 187,684 B（≈183 KB） |
| 0x40000 | 字符串池（null-terminated 连排） | 357,990 B |
| | **数据本体 data_len（= 写进分区的字节数）** | **620,134 B** |

也就是说：**真正有意义的数据只有约 620KB，其中 ~183KB 是没被写过的空档**
（内容其实是堆残留，见 §4.3 末条），而给它保留了 4MB 的 flash 分区。
代码里的实际上限也不是 4MB，而是 `PLS_STR_MAX_OFFSET = 0x2cc000`
（2.93MB，等于构建缓冲大小）。

> 精确口径：`data_len` = 620,134 B（数据本体）；pldata 分区镜像 = 4KB 头扇区 + data_len
> ≈ **624 KB**。搬到 SD 后文件也是这个尺寸（见 §4.3）。

### 1.2 访问方式（为什么现在是指针）

```
main.c:248         pldata_init()                       ← 开机装配阶段
  └─ 只做 esp_partition_find_first("pldata")，不读数据

boot_flow_run() → boot_enter_normal() → player_controller_init()
  └─ playlist_init()                 playlist.c:38
       ├─ playlist_store_is_valid()  ← pldata_read(0, hdr, ~40B)：读头部
       ├─ playlist_store_map_to_memory() → pldata_mmap() → esp_partition_mmap(整 4MB)
       └─ playlist_parse(base + 0x1000)  ← 之后全部走裸指针
```

下游（目录页 / 歌曲页 / 正在播放 / 虚拟列表 / 随机播放）**全部通过
`playlist_xxx()` 访问器读这几个指针**，没有任何模块直接持有 mmap 地址去算偏移
（`VIRTUAL_LIST_REBIND_STRATEGY.md` 已明确"不复制数据，只算指针偏移"）。
这一点是本次改造最有利的结构：**只要 `pldata_mmap()` 返回的指针换成 PSRAM 地址，
上层一行都不用改。**

### 1.3 写入方式

`playlist_store_save_playlist()`（`playlist_store.c:72`）的顺序是刻意的：

1. 擦头部扇区（**立即失效**，断电安全）
2. 按 len 向上取整擦数据区
3. 写数据本体
4. 最后写头部（commit）

写入耗时（`pldata.h:81` 的 2026-10-21 实测，同一份 **618,622 B** 数据）：

| 方案 | 写入阶段总耗时 | 说明 |
| --- | --- | --- |
| 直接 `esp_partition_write`（现状） | **6.14 s** | 擦除 ~2.3s + 编程 ~2.8s + 32B 栈弹跳开销 ~1s |
| 4KB 内部 RAM 分块 + `esp_partition_write` | 5.17 s | 只省窗口固定开销，已回滚 |

擦除/编程时间是 flash 物理特性，**减少字节数之外没有优化空间**。

### 1.4 写 flash 带来的连带代价（这才是重点）

`esp_partition_erase/write` 会**关闭外部内存缓存并停住另一颗核**，于是整个工程被迫
围绕它建立了一整套约束：

| 位置 | 被这条约束绑架的写法 |
| --- | --- |
| `boot_flow.c:54-60` | 扫描任务**必须 32KB 内部 RAM 栈**（实测常退回 16KB，开机内部 RAM 就此见底） |
| `boot_flow.c:807-847` | 写入阶段必须**抢 LVGL 锁冻结 UI**，否则渲染读 mmap 字体 → LVGL 任务永久卡死（`NPF_FONT.md` §7.1 的真机事故） |
| `settings_menu.c:752-821` | 重扫先把 stop/unmap/invalidate 委托给 `deep_worker`（内部栈） |
| `PSRAM_TASK_STACK.md`、`FLASH_WRITE_VS_PSRAM.md` | 一整套"PSRAM 栈任务不许进关缓存段"的纪律，`pldata_write` 是其中一条主因 |

搬到 SD 之后，**上述四条对 pldata 全部消失**（flash/NVS 的其他路径仍然保留该纪律）。

---

## 2. 为什么当年放 flash（历史前提已经反过来了）

原设计理由（`pldata.h` 头注释、`FontSystem.md`）：

> 读取通过 mmap 直接把分区映射进地址空间，**数据仍留在 Flash，不占堆 RAM**。

S3 时代 8MB PSRAM，字体（当年还是 TTF 整份塞 PSRAM/内嵌）把 PSRAM 吃得很紧，
所以"620KB 数据不占 RAM"是真金白银的收益。

S31 之后的现实：

* 16MB Octal PSRAM / 200MHz，稳态空闲 **16,093,491 B**（`MEM: heap=` 采样）；
* 字体已改成 **font 分区里的 NPF1 裸镜像 + mmap**（`font_npf.c`），不再占 PSRAM；
* 内部 RAM 才是稀缺资源（蓝牙约 50KB、LVGL 对象堆、任务栈），PSRAM 是富余资源。

**结论：当年换 RAM 的那笔账，现在反过来算了——用 620KB PSRAM 换掉"每次扫描 6 秒
flash 擦写 + 关缓存事故域"是明显的赚。**

---

## 3. 收益清单（带数字）

### 3.1 扫描写盘从 ~6 s 降到 <0.5 s

620KB 顺序写 SD（4-bit @40MHz，实际 5~10MB/s）+ FatFs 开销 ≈ **0.15~0.5 s**。
量级上快 15~40 倍。这直接影响用户感知：曲库一变就要重扫，现在重扫要盯着
"正在写入扫描数据..."冻结 6 秒。

### 3.2 消灭"扫描期间关缓存"整条事故域

`NPF_FONT.md` §7.1 记录的事故：扫描写入阶段 LVGL 还在渲染、取 mmap 字体字形时
总线卡死，task_wdt 5 秒后报 `CPU 0: LVGL`，表现为**整屏定格只能断电**。
SD 写入**不关外部缓存**，这条路径从根上不存在了，连带可以：

* 删掉 `boot_scan_progress_cb` 里"抢 LVGL 锁 + `lv_refr_now` 冻结"那段（`boot_flow.c:818-846`）；
* **扫描任务栈搬到 PSRAM**（`task_create_psram_stack`）→ 开机最紧张的时刻
  内部 RAM **+16~32KB**（现在是 32KB 申请失败退回 16KB，见 `SAKURAIO_FORK_EVAL.md` §9.2）；
* `settings_menu` 重扫链条里那条"必须委托 deep_worker 才能 unmap"的约束消失。

### 3.3 腾出 4MB flash —— 正好解 release 体积的燃眉之急

当前实测镜像（`build/`）：

| 构建 | 大小 | 槽 | 余量 |
| --- | --- | --- | --- |
| debug | 2,621,248 B | factory 3,145,728 B | 524 KB |
| release | **2,791,024 B** | factory 3,145,728 B | **354 KB** |
| sync | 1,666,912 B | ota_0 2,621,440 B | 954 KB |

release 只剩 **354,704 B（≈346 KiB）** 余量，而 `SAKURAIO_FORK_EVAL.md` §5 已经预警
"合并多格式 + BT 后 3MB factory 大概率吃紧"。删掉 pldata 就等于凭空多 4MB，
布局可以一次松到位（推荐方案见 §9）。

### 3.4 解开分区表的布局枷锁

`partitions.csv` 的四条原则里，**三条（①③④）都只为 pldata 服务**：
"pldata 必须留最末尾"、"pldata/font 按 64KB 对齐才能整段 mmap"、"动 pldata 偏移
就丢播放列表"。数据搬到 SD 之后这些约束全部作废，font/app 的边界可以自由重排，
**代价只剩"重刷一次字体 + 分区表"**（本来换字体也要重刷）。

### 3.5 健壮性/体验的附带改善

* **播放列表跟着卡走**：换卡不再出现"flash 里的缓存属于旧卡"这种状态，
  `card_id` 校验退化成保险，而不再是主线逻辑；
* **运行中拔卡不会让列表失效**（数据已在 PSRAM，比 mmap 更稳）；
* **写失败不再毁数据**：tmp 文件 + rename，旧缓存完整保留；
* 播放列表变成卡上一个普通文件 → 可备份、可在 PC 上看、可手工替换（符合本项目
  "数据不设限"的风格）；
* `is_valid()` 可以顺便做**整块 CRC32 校验**（现在注释里明说"只做快速判断"，
  因为 NOR flash 基本不坏；SD 卡会坏，值得校验一次，620KB CRC ≈ 2ms）。

---

## 4. 代码影响面（精确到函数）

### 4.1 结论：后端换掉，上层零改动

`pldata` 对外 8 个函数（`pldata.h`），除 `pldata.c` 自身外，**只有 `playlist_store.c`
调用**（`pldata_get_size` / `pldata_erase_all` 目前没有调用者）。
所以最小改法是"重写 `pldata.c` 内部实现，保持函数签名"，其余全部不动。

| 现有 API | 语义（flash） | 新语义（SD 文件 + PSRAM） |
| --- | --- | --- |
| `pldata_init()` | 找分区句柄 | **打开文件 → 整块读进 PSRAM**（失败=无效） |
| `pldata_get_size()` | 分区大小 4MB | 缓冲容量（文件中数据长度）——**当前无调用者** |
| `pldata_erase_range()` | `esp_partition_erase_range` | 缓冲内 `memset 0xFF`（保持上层逻辑不变） |
| `pldata_write()` | `esp_partition_write` | 缓冲内 `memcpy`，置 dirty |
| `pldata_read()` | flash 读 / 从映射 memcpy | 缓冲内 `memcpy` |
| `pldata_mmap()` | `esp_partition_mmap` | **返回 PSRAM 基址**（数据已就位） |
| `pldata_munmap()` | `esp_partition_munmap`（关缓存！） | 空操作 |
| （新增）`pldata_commit()` | — | 缓冲 → tmp 文件 → `f_sync` → `f_close` → rename |

### 4.2 逐文件改动

| 文件 | 改动 | 量级 |
| --- | --- | --- |
| `core/hardware/pldata.c` | **整体重写**为"SD 文件 + PSRAM 缓冲"后端 | ~220 行（原 177 行） |
| `core/hardware/pldata.h` | 注释重写（"省 RAM"→"开机载入 PSRAM"）；`PLDATA_PART_SIZE` 仅保留给上限判断 | ~40 行注释 |
| `core/storage/playlist_store.c` | ① `save_playlist` 末尾加 `pldata_commit()`；② `invalidate()` 改为删除文件（保留"立刻失效"语义） | +6 行 |
| `main/main.c` | `pldata_init()` 从 `sd_card_init()` **之前** 移到之后（`main.c:247-260`） | 移动 3 行 |
| ⚠ 放置细节 | 新的 `pldata_init()` 会**读 620KB 卡**，必须留在 `lvgl_port_lock()` 之前（`main.c:266`）——否则这 30~150ms 卡在 LVGL 锁里会冻住 UI 任务 | — |
| `ui/boot_flow.c` | ① **删掉无卡界面的"清除扫描数据"按钮**（无卡时没有文件可删，见 §6.4，约 −30 行）；② 可选：删写入阶段冻结 LVGL（约 −25 行）；③ 可选：扫描栈换 PSRAM | 中 |
| `ui/settings_menu.c`、`core/utils/deep_worker.c` | 可选：`munmap` 空操作后，重扫链里"必须委托 deep_worker"的注释/兜底可简化 | 注释为主 |
| `partitions.csv`、`tools/make_full_firmware.py` | **仅 P3 阶段**：删 pldata 段、改布局、脚本不再填 0xC00000 | 见 §9 |
| `AGENTS.md` / `FontSystem.md` / `NPF_FONT.md` / `FLASH_WRITE_VS_PSRAM.md` / `PSRAM_TASK_STACK.md` / `PLAYLIST_SCAN_SUMMARY.md` / `BOOT_FLOW_SUMMARY.md` / `FULL_FIRMWARE.md` / `CHANGELOG.md` | 同步"pldata 不再关缓存"的口径 | 见 §11 |

### 4.3 关键实现草图

```c
/* pldata.c —— SD 文件 + PSRAM 缓冲后端（草图，非最终代码） */
#define PLDATA_FILE_DIR   "/sdcard/.np"
#define PLDATA_FILE_PATH  "/sdcard/.np/pldata.bin"
#define PLDATA_TMP_PATH   "/sdcard/.np/pldata.tmp"
/* 镜像上限：4KB 头 + 数据区上限（与 playlist_store 的判据一致，实际远小于它） */
#define PLDATA_MAX_IMAGE  (PLAYLIST_STORE_DATA_OFFSET + PLAYLIST_STORE_MAX_DATA)

static uint8_t *s_buf = NULL;   /* 整个镜像（4KB 头 + PLST），PSRAM，常驻 */
static size_t   s_len = 0;      /* 有效长度（= 文件大小） */
/* pldata_write / pldata_erase_range 只改这块缓冲（memcpy / memset 0xFF），
   内部先 ensure_capacity(offset+len) 按需 realloc；不再碰 flash */

esp_err_t pldata_init(void)
{
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;   /* boot_flow 会走"无卡"界面 */

    FILE *f = fopen(PLDATA_FILE_PATH, "rb");
    if (!f) { ESP_LOGW(TAG, "no cache file, will scan"); return ESP_ERR_NOT_FOUND; }

    /* 1) 量长度 2) 一次性堆分配 3) 整块 fread */
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < (long)PLAYLIST_STORE_DATA_OFFSET || sz > (long)PLDATA_MAX_IMAGE) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    s_buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* ~620KB */
    if (!s_buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(s_buf, 1, sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(s_buf); s_buf = NULL; return ESP_ERR_INVALID_SIZE; }

    s_len = sz;
    ESP_LOGI(TAG, "pldata loaded from SD: %u B", (unsigned)sz);
    return ESP_OK;
}

/* 写盘（只在扫描末尾调一次；可在任意任务上下文，PSRAM 栈也安全） */
esp_err_t pldata_commit(void)
{
    FILE *f = fopen(PLDATA_TMP_PATH, "wb");
    if (!f) return ESP_FAIL;
    size_t put = fwrite(s_buf, 1, s_len, f);      /* 对齐时走 PSRAM→SD 直接 DMA（见 §5） */
    if (put == s_len) { fflush(f); fsync(fileno(f)); } /* FatFs f_sync：确保数据落卡 */
    fclose(f);
    if (put != s_len) { remove(PLDATA_TMP_PATH); return ESP_FAIL; }   /* 旧文件不受影响 */

    remove(PLDATA_FILE_PATH);                      /* 断电最坏 = 没有缓存 → 下次重扫 */
    if (rename(PLDATA_TMP_PATH, PLDATA_FILE_PATH) != 0) return ESP_FAIL;   /* 罕见：旧缓存已删 */
    return ESP_OK;
}
```

**实现细节备注（都是查过 IDF 的，别踩）：**

* `fsync()` / `rename()` / `remove()` 在 VFS-FAT 都有实现（`vfs_fat.c:177/151/150`），
  不需要绕过 VFS 直接调 FatFs；
* 本工程 `CONFIG_FATFS_SECTOR_4096=y`（**FatFs 扇区是 4KB 不是 512B**）→
  文件最小占用 4KB、写粒度 4KB，`ff.c:4196` 的"大块直写"每次至少 4KB；
* `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` + `PER_FILE_CACHE=y` → 每打开一个文件多占
  4KB PSRAM 缓存（`max_files=5` 时上限 20KB），可忽略；
* `CONFIG_FATFS_TIMEOUT_MS=10000`（默认）→ 写 620KB 期间别的任务抢 FS 锁最多等
  几百 ms，不会超时；
* 目录用 `mkdir` 创建一次（`/sdcard/.np`），失败就退回直接放根目录——不值得为它引入错误分支。
* **缓冲必须按需增长**（最容易漏的一条）：无缓存时 `s_buf == NULL`，而一次扫描结束时的
  `erase_range(0x1000, 620KB)` + `write(0x1000, 620KB)` 会要求 624KB 容量 →
  `pldata_erase_range()` / `pldata_write()` 里先 `ensure_capacity(offset + size)`
  （`heap_caps_realloc`，失败返回 `ESP_ERR_NO_MEM`，上层已有失败路径）；
  写完可选 `realloc` 缩到实际长度（省 2.3MB）。
* **顺手修掉一个历史瑕疵**：`playlist_builder` 的 2.93MB 构建缓冲是 `heap_caps_malloc`
  （未初始化），只用到了头部/目录/歌曲/字符串四段，**中间的填充区（约 183KB）写进文件的是
  堆残留字节**——这正是 `data_len` 是 620KB 而不是 ~437KB 的原因。改成
  `heap_caps_calloc`（或 malloc 后 `memset(0xFF)`）就能让文件内容变成确定性的，
  便于 PC 端比对/校验。不改也不影响功能。

**为什么文件内容与分区镜像逐字节相同**（4KB 头 + PLST 数据，不用新格式）：

1. `playlist_store.c` / `playlist_format.h` 完全不用动，`PLST` 版本号也不用升；
2. 老设备升级可一次性导出（§10 P1），新老固件可以互相读写；
3. 出问题可以直接把 4MB 分区 dump 成这个文件，或用 PC 工具解析对比；
4. 代价只是多 4KB 的头扇区"浪费"——不值得为省 4KB 引入新格式。

---

## 5. 为什么这条路径本来就能通（代码里已有先例）

| 事实 | 出处 | 意义 |
| --- | --- | --- |
| **`playlist_init_from_buffer()` 已存在** | `playlist.c:66` | "从内存 buffer 解析 PLST"是现成路径，`s_from_buffer` 分支就是为它准备的 |
| **S31 的 SDMMC 硬件支持 PSRAM 直连 DMA** | `soc_caps.h:330 SOC_SDMMC_PSRAM_DMA_CAPABLE 1`（注释原文：*can do DMA transfer to/from PSRAM*）→ `sdmmc_cmd.c:491-496` 直接走 DMA，**只有缓冲不满足对齐时才回退**到内部临时 DMA 缓冲（`sdmmc_cmd.c:497-511`，IDF 自动兜底，不需要我们手写 bounce） | 620KB 从 PSRAM 写卡的**正确性由 IDF 保证**，最坏情况只是多一次内部 RAM 拷贝 |
| **FatFs 自己的缓冲早就在 PSRAM** | `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y`（`build/debug/sdkconfig:3263`）→ 卷窗口与每文件缓存都从 PSRAM 分配；`CONFIG_FATFS_PER_FILE_CACHE=y` | 本项目**一直在**用 PSRAM 做 SD DMA 的源/目标，不是新玩法 |
| **FatFs 读缓冲放 PSRAM** | `audio_decoder.c:20-64`（16KB/64KB `setvbuf` + `MALLOC_CAP_SPIRAM`） | 实机长期正常，等于这条路径已经被每天验证 |
| 封面整块读进 PSRAM | `cover_art.c` 读 JPEG blob（最大 131KB，直接 `fread` 到 PSRAM） | 同上，且是"大块直读"，正是我们写 620KB 的对称场景 |
| FatFs 可重入 | `ffconf.h:343 FF_FS_REENTRANT 1` + `vfs_fat.c` 每卷 `_lock` | 多任务同时读写 SD 是安全的（内部串行化） |
| 扫描任务本来就在读 SD | `playlist_builder.c` | 只多一个"写"的动作，不引入新外设/新初始化 |
| 换卡/曲库变化判定已完备 | `boot_flow.c:1009-1051` + 头部 `card_id`/`signature`/`file_count` | 缓存搬到卡上后这套逻辑照用，且更自然 |
| `fsync` / `rename` / `remove` 在 VFS 都有实现 | `vfs_fat.c:177 fsync_p`、`:151 rename_p`、`:150 unlink_p` | tmp + `fsync` + rename 的原子提交方案可以直接写 |

---

## 6. 风险清单（**第 6.1 条是本次唯一真正需要用户拍板的**）

### 6.1 ⚠ 固件第一次开始写用户的音乐卡

现在固件对 SD **纯只读**（全项目 `fopen` 只有 `"r"`/`"rb"`，无 `fwrite`/`remove`），
连 `format_if_mount_failed` 都是 `false`（`sd_card.c:71`）。这是很宝贵的性质：
**无论发生什么，固件的 bug 都不会毁掉用户的音乐库。** 改成可写之后：

* 写坏的最坏后果不是"播放列表丢了"（那只需重扫 2 秒），而是**卡上文件系统损坏**
  → 用户得拿去 PC 上 `chkdsk`/修复，极端情况要重新拷库；
* 触发条件：写到一半断电（PMU 断电、电池耗尽）、写的过程中拔卡、卡本身质量差。

**缓解（必须全部做到）：**

1. **只写自己那一个文件**（+ 同级 tmp），绝不碰其它路径，绝不 `format`/`mkfs`；
2. **tmp + rename**：目标文件永远是完整版本，写失败留旧文件；
3. 写完先 `fsync()`（→ FatFs `f_sync`）再 `fclose`，缩短"脏数据在飞"的窗口；
4. **只在扫描末尾写一次**（每次扫描最多 620KB），不做增量/高频写；
5. 写的时候**不播放**（现有流程天然满足：开机扫描时还没开始播；重扫是
   "先停播放 → invalidate → 重启"）；
6. 写失败要能在 UI 上说清楚（现有 `boot_show_scan_failed()` 就在位）。

> 我的判断：风险等级 **低但非零**，和"用 SD 存播放列表"这个产品决策本身绑定。
> 如果用户完全不能接受"固件写卡"，那就没有别的选择——只能留在 flash（见 §12）。

### 6.2 PSRAM 常驻 620KB（按曲库线性增长）

* 9042 首 = 620KB；估算 2 万首 ≈ 1.4MB；代码上限 `PLS_STR_MAX_OFFSET` 2.93MB。
* 相对 16MB 空闲：3%~18%，可接受。但要**同时**看叠加峰值：

| 场景 | PSRAM 占用 |
| --- | --- |
| 稳态播放 OGG + 800×800 封面 | 1MB（OGG arena）+ 1.33MB（封面像素） |
| 开机扫描峰值 | 2.93MB（构建缓冲）+ 0.62MB（旧列表，扫描完成后释放/替换） |
| 扫描 + 播放 OGG（当前不会同时发生） | 4.9MB，仍安全 |

* **失败路径**：`heap_caps_malloc` 拿不到 620KB 时，不能"带空列表进主界面"，
  要落到 `boot_show_scan_failed()` 或专门的"内存不足"界面。现在 mmap 是零堆开销、
  天然免疫 OOM，这是**新引入的失败模式**，必须显式处理。

### 6.3 写盘期间 FatFs 卷锁

`FF_FS_REENTRANT=1`，写 620KB 期间其它任务读 SD 会被阻塞（最多几百 ms，
`CONFIG_FATFS_TIMEOUT_MS` 默认 10s，不会超时）。开机扫描时没有其它读者，安全；
但**如果将来支持"播放中增量保存列表"**，就会顶到音频读卡（I2S DMA 只有 93ms 余量）
→ 明确记为"不要做增量写"的理由。

### 6.4 无卡界面的"清除扫描数据"按钮必须删掉

`boot_flow.c:946` 的 failsafe 按钮调 `playlist_store_invalidate()`，而它出现在
**没有挂载 SD** 的场景。数据搬到卡上之后，无卡时没有任何东西可清。
处理：删按钮 + 改提示文案（那屏还剩"重置歌曲位置"，仍有用）。

### 6.5 首次升级必须重扫一次（可优化）

老设备的播放列表在 flash 分区里，新固件找不到 SD 文件 → 走一次自动扫描（~2s + 写盘）。
可以接受；如果想省掉，P1 阶段保留分区多做一次"一次性导出"（读分区 → 写文件），
大约 15 行代码。

### 6.6 其他小项

| 项 | 说明 |
| --- | --- |
| SD 写保护/卡满 | 旧缓存保留，每次开机重扫 2 秒；不致命，但要在日志里明确 |
| exFAT | 需要 IDF 本体 `FF_FS_EXFAT=1` 的补丁（`IDF6_MIGRATION.md` §6.7）；写入路径同样依赖它，注意别换电脑后忘打补丁 |
| `max_files = 5`（`sd_card.c:72`） | 只要**开机读完就 `fclose`**，不常驻句柄；常驻会把句柄从 5 里长期占掉 1 个（播放/封面/npd 并发时容易撞） |
| 隐藏文件被用户删/被清理软件删 | 无害：下次开机自动重扫 |
| 文件名含 `.` 前缀 | 扫描器 `collect_dir()` 跳过 `.` 开头的条目（`playlist_builder.c:321`）→ `.np/` 不会被扫成目录，也不会出现在 UI |
| 对时固件（sync） | 完全不碰 pldata，无影响；但分区表变了要一起重刷（`DUAL_APP_TIME_SYNC.md`） |

---

## 7. 性能估算

| 阶段 | 现在 | 改后 | 差 |
| --- | --- | --- | --- |
| 开机读列表 | `esp_partition_mmap` 4MB ≈ 0ms（映射，不读数据） | 读 620KB ≈ **30~150ms** | +0.03~0.15s（对比开机已有 1276ms 指纹探针，可忽略） |
| 扫描写盘 | 5.17~6.14s | **0.15~0.5s** | **−5.7~6s** |
| 扫描期间 UI | 冻结 6 秒（画面定格） | 不冻结（只有 FS 锁） | 体验明显改善 |
| 列表遍历（滚动/切歌） | flash mmap + 缓存 | PSRAM（200MHz octal）+ 缓存 | 基本持平，略偏快 |
| 扫描任务栈 | 32KB **内部** RAM（常退回 16KB） | 可放 PSRAM | +16~32KB 内部 RAM |
| 关缓存风险 | 每次扫描都有（`NPF_FONT.md` §7.1 事故） | **没有** | — |

> 唯一变慢的一项是开机那一次 620KB 读，量级远小于开机流程里已有的指纹探针
> （5833 个文件 1276ms，`SAKURAIO_FORK_EVAL.md` §4.1）。

---

## 8. 已验证事实 / 待真机确认

| 事实 | 依据 | 状态 |
| --- | --- | --- |
| PLST 是自包含定长块，解析只读指针 | `playlist.c:21-36` | ✅ 读码确认 |
| 除 `playlist_store` 外无 `pldata` 使用者 | 全仓 grep（32 处，均在 `pldata.c`/`playlist_store.c`/注释） | ✅ 读码确认 |
| `pldata_get_size()` 无调用者 | 全仓 grep | ✅ 读码确认 |
| 内存 buffer 解析路径已存在 | `playlist_init_from_buffer()` | ✅ 读码确认 |
| PSRAM 缓冲可做 SD DMA 目标 | `audio_decoder.c` 16KB/64KB `setvbuf`（PSRAM）实机长期正常 | ✅ 间接实测 |
| `f_write` 大块直写会把用户缓冲交给 `disk_write` | `ff.c:4196-4201`（`wbuff` = 用户指针） | ✅ 读码确认 |
| FatFs 线程安全 | `FF_FS_REENTRANT=1` + VFS `_lock` | ✅ 读码确认 |
| **写**方向（PSRAM → SD DMA）的可行性 | `SOC_SDMMC_PSRAM_DMA_CAPABLE=1` + `sdmmc_cmd.c:491-496` 直接 DMA、`:497-511` 未对齐自动兜底 | ✅ **硬件+驱动保证**（不是靠猜） |
| 620KB 实际写卡耗时 | 4-bit@40MHz 理论 + FatFs 开销（`CONFIG_FATFS_SECTOR_4096=y`，4KB 粒度） | ⚠ 待真机实测（估 0.15~0.5s） |
| `f_rename` 的断电语义（FAT/exFAT 两套） | FatFs 实现 | ⚠ 待实测（写入瞬间拔电） |
| 扫描任务搬 PSRAM 栈后能否跑完 9042 首 | `task_create_psram_stack` 可用 | ⚠ 待实测（可选优化） |

---

## 9. P3：分区表怎么用这 4MB（推荐方案）

现状（16MB）：

```
0x010000  factory   3MB          → 0x310000
0x310000  ota_0     2.5MB        → 0x590000
0x590000  otadata   8KB          → 0x592000
          （空隙 896KB）
0x600000  font      6MB          → 0xC00000
0xC00000  pldata    4MB          → 0x1000000   ← 删掉它
```

推荐（font 移到尾部，把腾出的空间全给 app）：

```
0x010000  factory   6MB     (0x600000)   → 0x610000   release 余量 354KB → ≈3.5MB
0x610000  ota_0     3.93MB  (0x3EE000)   → 0x9FE000   对时固件余量 954KB → ≈2.45MB
0x9FE000  otadata   8KB     (0x2000)     → 0xA00000
0xA00000  font      6MB     (0x600000)   → 0x1000000   正好顶到 16MB 末尾
```

对齐核对：`0x010000 / 0x610000 / 0xA00000` 都是 64KB 整数倍 ✅；`0x9FE000` 是 4KB 整数倍 ✅。

一次性成本：**重刷分区表 + 重刷字体到 0xA00000**（字体本来也是手动单独刷，
`FONT_FLASH.md` 的流程不变，只改地址）。之后 layout 就自由了——
因为**再没有一张"播放列表必须留在 flash 尾部"的钉子了**。

保守替代方案（**不用刷字体**）：font 保持 `@0x600000`，只把 ota_0 从 2.5MB 缩到 2MB
（对时固件实测 1,666,912 B，仍余 ~400KB）：

```
0x010000  factory   3.5MB (0x380000) → 0x390000
0x390000  ota_0     2MB   (0x200000) → 0x590000
0x590000  otadata   8KB              → 0x592000
0x600000  font      6MB（不动）
```

factory 3MB → 3.5MB（release 余量 354KB → ≈850KB），代价只有"重刷分区表 + 对时固件
要按新地址重烧一次"。想再大就得动 font 了。

---

## 10. 分阶段落地计划

### P1 — 换后端（最小可用，可回退）

1. 重写 `pldata.c`：开机 `fopen` + 整块读 → PSRAM；`mmap` 返回 PSRAM；
   `commit()` 走 tmp+f_sync+rename。
2. `main.c`：把 `pldata_init()` 移到 `sd_card_init()` 之后。
3. `playlist_store.c`：`save_playlist` 末尾 `pldata_commit()`；`invalidate()` 改删文件。
4. `boot_flow.c`：删无卡界面的"清除扫描数据"按钮。
5. **保留 `pldata` 分区**：只在"新固件首次开机、SD 上没有文件、而分区里有有效数据"时
   做一次性导出 → 之后再也不写 flash。好处有二：老用户不用重扫；出问题还能退回旧固件。
6. `playlist_store_is_valid()` 顺带校验整块 CRC32（SD 会坏，值得）。

**验收（真机）：** ① 空卡首扫 → 生成文件 → 重启命中缓存直接进主界面；
② 加/删一首歌 → 自动重扫；③ 断电测试（写入瞬间拔电）→ 下次开机重扫而不是崩溃；
④ 换卡；⑤ 无卡界面；⑥ 曲库 9042 首实测写盘耗时；⑦ `MEM: heap=` 前后对比。

### P2 — 拆掉为 flash 写而生的脚手架

* 删 `boot_scan_progress_cb` 的"抢 LVGL 锁冻结"逻辑（`boot_flow.c:818-846`）；
* 扫描任务栈换 `task_create_psram_stack()`（`boot_flow.c:329-338` 的 32KB/16KB 内部栈那段）；
* `settings_menu.c` 重扫链、`deep_worker.c` 注释里"必须内部栈"的 pldata 部分可以简化；
* 更新 `AGENTS.md` 硬规则第 4 条：`pldata_write/erase` 从"关缓存清单"里划掉。

### P3 — 删分区 + 重排布局

* 删 `partitions.csv` 的 `pldata` 行、按 §9 重排；
* `tools/make_full_firmware.py` 去掉 pldata 段（0xC00000 不再需要 0xFF）；
* 重刷分区表 + 字体；文档全面同步（§11）。

---

## 11. 文档/工具同步清单（漏了就等着下一个人踩）

| 文件 | 要改的地方 |
| --- | --- |
| `AGENTS.md` | 分区表段（pldata 删除/新布局）；"关缓存"硬规则第 4 条；`pldata` 在模块清单里的描述；任务栈里 `boot_scan` 的"必须内部 RAM"。 |
| `partitions.csv` | 头注释里的四条原则（pldata 相关三条作废）+ 第五次重排记录。 |
| `FontSystem.md` / `NPF_FONT.md` | §"pldata 必须待在最末尾"、§7.1 事故结论（"任何映射在 flash 的数据在 pldata 写入期间不可访问"→ 该路径已不存在）。 |
| `PSRAM_TASK_STACK.md` / `FLASH_WRITE_VS_PSRAM.md` | `pldata_write` 相关条目；`boot_scan` 栈位置。 |
| `PLAYLIST_SCAN_SUMMARY.md` / `BOOT_FLOW_SUMMARY.md` | 存储介质描述、写盘阶段、界面文案。 |
| `FULL_FIRMWARE.md` / `README.md` / `CHANGELOG.md` | "整片烧 0x0 会清 pldata/播放列表"的说法失效（列表在 SD 上）；新增"卡上 `.np/pldata.bin` 可删除以强制重扫"的说明。 |
| `DUAL_APP_TIME_SYNC.md` | 分区表第二节的布局表。 |
| `CMakeLists.txt` | 白名单里 `esp_partition` 注释"font 分区 mmap、pldata" → 只剩 font（组件仍要留，字体还在用它）。 |

---

## 12. 考虑过但否掉的方案

| 方案 | 为什么否掉 |
| --- | --- |
| **留在 flash，改用 LittleFS/SPIFFS**（顺带也能腾出 4MB 分区概念） | 写 flash 仍然要擦除+编程，6s 量级不变；关缓存窗口不变；只是换了个容器。收益基本为零 |
| **放进 font 分区的空余（4.44MiB 用了 6MB 的一部分）** | 同上，仍是 flash 写；而且会破坏"font 分区是裸镜像、单文件"的简单模型 |
| **把 pldata 分区从 4MB 缩到 1MB** | 省 3MB，但保留全部 flash 写代价。可以作为"只想省空间、不想承担写卡风险"的折中，但不解决 6 秒问题 |
| **SD 文件 + 按需读（不开机载入）** | 播放列表访问是**高频随机指针读**（滚动列表每帧读几十条），按需 `fseek+fread` 会把 UI 帧率打崩 |
| **保留 mmap 语义、把 SD 文件整块映射** | FAT 文件无法 mmap（不是物理连续、无扇区→页表映射），只有 SPIFFS/LittleFS 之类才能考虑，而它们又在 flash 上 |
| **双写（flash + SD 各一份）** | 6 秒的代价照付，只多了复杂度 |

---

## 13. 一句话建议

**做，但按 P1→P2→P3 走，第一步只换 `pldata.c` 的后端并保留旧分区做迁移/回退。**
收益（扫描 6s→0.4s、消灭关缓存事故域、+4MB flash、解开布局枷锁）远大于代价
（620KB PSRAM 常驻 + 一个新的 OOM 失败模式 + "固件会写卡"这一类新风险）；
唯一需要用户明确点头的是 §6.1——**从此固件的 bug 有可能损坏 SD 卡上的文件系统**。
