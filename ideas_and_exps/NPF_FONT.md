# NPF_FONT —— NPF1 外置位图全字库：LVGL 9.4 可行性、格式、生成与刷写

> 适用分支：`master`（ESP32-S31 + LVGL **9.4.0** + ESP-IDF **6.1**）。
> `legacy_s3`（ESP32-S3 + LVGL 8.4 + IDF 5.5.X）用的是**同一份镜像、同一套生成工具**，
> 差异只在运行时对 `lv_font_fmt_txt_dsc_t` 的字段填充（见 §2.3）。
>
> 结论先行：**LVGL 9.4 可以直接吃"构建期预烘焙"的字体镜像**——`fmt_txt` 的
> `glyph_dsc` 在 `LV_FONT_FMT_TXT_LARGE=1` 下仍是 **16B 定长、无位域**，与 8.4
> 逐字段相同；于是 `master` 不再编译任何全字库、也不再依赖 FreeType：24px 全字库
> （Droid Sans Fallback，**34,461 个码位**）预烘焙成 NPF1 镜像放进 `font` 分区，
> 运行时一次 `esp_partition_mmap` + 填指针即可，**零解析**。
> 实测（2026-11-14，debug）：应用固件 **1.38MB**、字体镜像 **4.44MiB** 独立分区、
> 常驻 RAM ≈3.7KB、PSRAM 不再有 FreeType 字形缓存，用户数据里的生僻字/日文/韩文
> 不再显示为方框。

相关文档：`BINFONT_RESEARCH.md`（调研：官方 bin loader 为什么不能直接用、体积账）、
`FONT_FLASH.md`（刷字体操作）、`tools/font/README.md`（生成工具）。

---

## 1. 要解决的问题

2026-10 项目把系统字体从"内嵌位图字库"迁到 `font` 分区里的 **TTF + FreeType**
（`spiffs/system.ttf`）。TTF 的优点是字形全、任意字号，代价也很实在：

| 项目 | TTF + FreeType | 后果 |
|---|---|---|
| 依赖 | `espressif/freetype`（~2MB 代码） | app 变大，构建配置多一整套 FreeType 接线 |
| 栈 | 光栅化器在栈上放渲染池 | LVGL / 主任务 / deep_worker 的栈被迫加到 16~20KB |
| 内存 | 1024 条字形位图缓存（≈0.5MB）落在 PSRAM | 多一份常驻内存；关缓存期间取不到字形 |
| 观感 | 8bit AA，细腻 | 但位图字体时代滚动帧率反而更高（现场光栅化是热点） |

**目标**：字体全（覆盖整份 TTF 的 cmap）、app 不再背 FreeType、PSRAM 不占缓存、
字形观感与 S3 分支（位图字体）一致。

---

## 2. LVGL 9.4 能不能用这个镜像（关键结论）

能。9.4 的 `fmt_txt` 运行期只依赖三样东西，全部与"字体数据放在哪"无关：

| 依赖 | 9.4 的定义（`managed_components/lvgl__lvgl/src/font/lv_font_fmt_txt.h`） | 结论 |
|---|---|---|
| 字形描述 `lv_font_fmt_txt_glyph_dsc_t` | `LV_FONT_FMT_TXT_LARGE=1` 时是 `u32 bitmap_index; u32 adv_w; u16 box_w; u16 box_h; i16 ofs_x; i16 ofs_y;` = **16B 定长、无位域、无 padding** | 与 8.4 逐字段相同，可以离线按小端直接写进镜像（`font_npf.c` 里有 `_Static_assert` 锁死） |
| 字符映射 `lv_font_fmt_txt_cmap_t` | 含 2 个指针（`unicode_list` / `glyph_id_ofs_list`），必须在 RAM；`range_length`/`list_length` 仍是 u16 | 运行时只拷贝 `cmap 数 × sizeof(cmap)`（183 个 ≈ 4.4KB），指针指向 mmap 区 |
| 字形位图 | `lv_font_get_bitmap_fmt_txt()` 在 `LV_FONT_FMT_TXT_PLAIN` 下把 bpp 位图**展开成 A8** 写进调用方给的 `draw_buf` | 镜像里的位图按"逐字形字节对齐、`stride=0`、无压缩"存放即可；开销与内嵌位图字库完全相同 |

### 2.1 与 8.4 的差异（写运行时代码时必须知道）

1. **`lv_font_fmt_txt_dsc_t` 多了 `uint8_t stride` 字段**（每行位图的字节对齐，0 = 不 padding）。
   NPF 的位图是紧排的，`font_npf.c` 必须显式 `s_dsc.stride = 0`，否则 9.4 会按
   `LV_ROUND_UP` 算行宽、把位图读歪。**这是 8.4 → 9.4 唯一需要新增的赋值**。
2. **`lv_font_t` 多了 `release_glyph` / `kerning` / `static_bitmap` / `user_data`**：
   `memset(&s_font, 0, …)` 后分别是 NULL / `LV_FONT_KERNING_NORMAL`(0) / 0 / NULL，
   正好都是我们要的行为（`lv_font_glyph_release_draw_data()` 会先判 `release_glyph` 非空）。
3. **`sizeof(lv_font_fmt_txt_cmap_t)` 不再是 20B**（9.4 的 enum 对齐可能让它变 24B），
   但 NPF 镜像里的 cmap **记录**仍是固定 20B、运行时逐字段读进结构体 —— 两者无关，
   所以 `font_npf.c` 不再对 `sizeof(cmap)` 做断言，只锁镜像格式用到的字段偏移。
4. **取位图不再直接返回原指针**：9.4 的 `lv_font_get_bitmap_fmt_txt()` 在 PLAIN 下
   逐像素展开为 A8（8.4 也是这条路径；"零拷贝"指的是**镜像本身不进 RAM**）。
   9.4 的绘制层会按字形缓存这份 A8 数据（`g_dsc->entry`），重复绘制同一字不重复展开。
5. `get_glyph_dsc_id()` 的区间判断 8.4 是 `>`（off-by-one）、9.4 改成 `>=`：只影响
   "每个区间末尾之后那一个本来就不存在的码位"，**对真实码位没有差别**，镜像通用。

### 2.2 与官方 `lv_binfont_create()` 的关系

9.4 自带的 `lv_binfont_create()` 读的是 lv_font_conv 的原始 **bin**（带 head 表、
   cmap 间接寻址、glyph_dsc 是**位域**格式），需要逐字形解析、重排，运行期开销与
   RAM 占用都不合适（调研见 `BINFONT_RESEARCH.md`）。NPF1 是我们自己的**预烘焙**
   格式：所有偏移、cmap 列表、16B dsc 都在构建期算好，运行时只填指针。

### 2.3 两条分支怎么共用同一份镜像

| | `master`（9.4） | `legacy_s3`（8.4） |
|---|---|---|
| 主要差异 | `dsc.stride = 0`；`lv_font_t` 的新字段 memset 为 0 | 无 stride 字段 |
| 镜像 | **同一份 `spiffs/system.npf`** | 同左 |
| 生成工具 | `tools/font/*.py` 相同 | 同左 |

---

## 3. NPF1 v1 格式（`tools/font/bin_font_prebake.py` 产出，`main/core/fonts/font_npf.c` 消费）

全部小端。单 face（当前只有 24px），头里为多字号预留了 `face_count`。

```
+0x00  Header（64B）
       +0  "NPF1"
       +4  version      u16  (=1)
       +6  face_count   u16  (=1)
       +8  flags        u16
       +10 max_face     u16  (最大字号 px，仅供信息)
       +12 crc32        u32  (覆盖 [64, total_size)，当前运行时不校验)
       +16 total_size   u32  (整文件字节数 → 运行时据此决定 mmap 长度)
       +20 保留 44B
+0x40  Face 记录（48B）
       +0  size_px u16 | +2 bpp u8 | +3 cmap_num u8 | +4 flags u16 | +6 kern_class u8
       +8  glyph_count u32 | +12 dsc_off | +16 cmap_off | +20 lists_off
       +24 bitmap_off | +28 bitmap_size | +32 kern_off
       +36 line_height i16 | +38 base_line i16 | +40 subpx u8
       +41 underline_position i8 | +42 underline_thickness u8 | +44 kern_scale u16
+0x70  glyph_dsc[]  16B/字形，与 LARGE 布局逐字节一致（gid0 是空字形）
...    cmap[]       20B/子表：
         +0 range_start u32 | +4 range_length u16 | +6 glyph_id_start u16
         +8 unicode_list_off u32 (-1=无) | +12 glyph_id_ofs_off u32 (-1=无)
         +16 list_length u16 | +18 type u8
...    lists        u16 unicode 列表 / u8 或 u16 偏移表（按 2 字节对齐）
...    （kern 段，可选，当前**不生成**）
...    bitmap       按 gid 顺序、逐字形字节对齐、stride=0、无压缩（4 字节对齐起点）
```

运行时（`font_npf_init()`）只做这些事：

1. `esp_partition_find_first(DATA, 0x41, "font")` + 读 64B 头校验 magic/version/total_size；
2. `esp_partition_mmap(part, 0, total_size, ESP_PARTITION_MMAP_DATA, ...)`
   —— 只映射镜像实际长度（4.65MB → 71 个 64KB MMU 页），不是整个 6MB 分区；
3. 校验 face 里各段偏移都落在 `total_size` 内；
4. `heap_caps_malloc(cmap_num × sizeof(cmap))`（优先内部 RAM，失败退默认堆），
   把 183 条 cmap 记录里的偏移换成 `base + off` 指针；
5. 填 `lv_font_fmt_txt_dsc_t`（bpp=2、PLAIN、kern=NULL、**stride=0**）与
   `lv_font_t`（两个回调指向 LVGL 自带的 `lv_font_get_glyph_dsc_fmt_txt` /
   `lv_font_get_bitmap_fmt_txt`，fallback = `lv_font_montserrat_24`，
   用于 `LV_SYMBOL_*` 这类 PUA 图标）。

**常驻 RAM**：`lv_font_t` + `dsc` + cmap 数组 ≈ **3.7KB**（实测 04b-system_font 阶段
内部 RAM 仍有 ~213KB 空闲）。

### 3.1 位图格式与开销

`lv_font_conv` 默认压缩（`compression_id=1`）；本项目 `CONFIG_LV_USE_FONT_COMPRESSED=n`，
所以生成时必须 `--no-compress`。镜像里是 2bpp 原始位图，运行时逐字形展开为 A8
（每帧每个字形一次，之后由 LVGL 的字形缓存命中）；与内嵌位图字库的开销完全相同。

### 3.2 和 kern 的关系

预烘焙默认**丢弃** kern 段（`--keep-kern` 可保留，但运行时不解析）：中文界面几乎不用
kern；ASCII 的少量字距差在 24px 下肉眼不可见，换来的是不用在运行时构造
`lv_font_fmt_txt_kern_pair_t`（含 2 个指针），少一份耦合。

---

## 4. 生成流程（可复现）

前置：`lv_font_conv` 1.5.3（Node，本地安装在 `tempdir/fontconv`，见 `tools/font/README.md`），
源字体是 **Droid Sans Fallback**（AOSP，Apache-2.0）。仓库里只存 NPF 镜像、不再存 TTF，
源 TTF 从"删除 TTF 之前"的提交里取：

```powershell
# 0) 取源字体（3.55MB）：git log --diff-filter=D --oneline -- spiffs/system.ttf
git show <ttf_commit>:spiffs/system.ttf > tempdir\system.ttf

# 1) 从 TTF 的 cmap 生成区间清单（149 段）
python tools/font/font_meta.py tempdir\system.ttf --outdir tempdir

# 2) 栅格化成 lv_font_conv bin（24px / bpp2 / 不压缩）
$ranges = Get-Content tempdir\font_ranges.txt
$argv = @('--font','tempdir\system.ttf','--size','24','--bpp','2','--no-compress',
          '--format','bin','-o','tempdir\all24_bpp2.bin')
foreach ($r in $ranges) { $argv += '--range'; $argv += $r }
& tempdir\fontconv\node_modules\.bin\lv_font_conv.cmd @argv

# 3) 预烘焙成 NPF1（逐字形与源 bin 比对，约 4 秒）
python tools/font/bin_font_prebake.py tempdir\all24_bpp2.bin spiffs\system.npf

# 4) 按 LVGL 的查找逻辑复算一遍 + 与 TTF cmap 交叉核对（强烈建议）
python tools/font/npf_verify.py spiffs\system.npf --ttf tempdir\system.ttf
```

`spiffs/system.npf` **入库**（4.44MiB，仓库里另有一份 TTF 的许可文本
`spiffs/DroidSansFallback-LICENSE.txt`）；`tempdir/` 下的中间产物不入库。

改字号/bpp/字体后必须重新走一遍，并确认镜像仍能塞进 6MB 分区
（`npf_verify.py` 会打印总长与位图末端）。

---

## 5. 分区与刷写

```
factory  0x10000  0x5F0000 (5.9375MB)   应用固件（debug 1.38MB，余量充足）
font     0x600000 6MB                   spiffs/system.npf（4.44MiB）
pldata   0xC00000 4MB                   播放列表（**偏移不动**，旧数据不失效）
```

- 只改了 `factory` 与 `font` 的边界，`nvs`(0x9000) 与 `pldata`(0xC00000) 都没动
  → **用户配置与播放列表都保留**；
- 分区表变了 → 首次必须让 esptool 写一次 `partition-table.bin`（`idfc flash` 会写）；
- 字体位置从 0x800000 挪到 0x600000，且内容换成 `.npf` → **必须重刷字体**。

日常刷机：

```powershell
idfc flash                                  # app + 分区表（**不含字体**）
# 首次/换字体时补一条（4.44MiB，约 29 秒）：
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool `
    --chip esp32s31 -p COM3 -b 1500000 write_flash 0x600000 spiffs\system.npf
```

`python tools/make_full_firmware.py -t release` 生成的全量固件**已包含**字体段，
新机器一次烧 `0x0` 即可（`--no-font` 可跳过）。

没刷字体 / 刷坏时会怎样：`font_npf_init()` 打印
`font 分区里不是 NPF1 镜像…回退内嵌字库`，UI 用精简字库继续跑（固定文案全在，
用户数据缺字）——不会白屏、不会崩。

---

## 6. 实测数据（2026-11-14，S31 + debug）

| 项目 | TTF + FreeType（旧） | NPF1（新） |
|---|---|---|
| 应用固件（debug） | 含 FreeType 源码与引擎 | **1.38MB** |
| 字体占 Flash | 3.55MiB（font 分区） | 4.44MiB（独立分区） |
| 依赖组件 | `espressif/freetype` | **无** |
| 常驻 RAM | 字形缓存 1024 条（PSRAM） | ≈3.7KB（cmap 数组 + dsc，内部 RAM） |
| 栈预算 | LVGL/主任务/deep_worker 被迫 16~20KB | 位图不再光栅化，可回落 12KB（当前仍保留 20KB） |
| 每帧文字 | 现场光栅化（未命中时） | 逐字形 bpp2→A8 展开（与内嵌位图字库同） |
| 码位覆盖 | TTF 全 cmap | **34,461（同）** |

启动日志（正常挂载）：

```
I (1072) font_npf: NPF1 全字库已挂载: 24px bpp2, 34462 个字形, 183 个 cmap, 镜像 4653106B @0x600000, 行高 32
I (1083) system_font: 系统字体：NPF1 全字库（34462 个字形）
```

---

## 7. 已知取舍与风险

1. **画质**：bpp2（4 档灰度），与内嵌位图字库一致；不是 FreeType 的 8bit AA。
   要更细腻只能 bpp4，24px 全字库会到 ~8MB，6MB 分区放不下。
2. **字形来自 Droid Sans Fallback**：行高 **32**（TTF 24px 是 31），个别固定高度容器
   可能需要 1~2px 的视觉微调；20px 小字体仍用内嵌 `system_smaller_font`（行高 23）。
3. **位图在 Flash（XIP）而非 PSRAM**：读走 L1 cache，一屏中文约 10~20KB 字形数据；
   滚动时可能有个别 cache miss（与 `pldata` mmap 同性质）。
4. **关缓存的窗口**：`pldata` 重写（扫描/重新扫描）会短暂关闭外部缓存，此时 mmap 的
   字体不可访问 —— **写入期间必须让 LVGL 完全停止渲染**。设置菜单的"重新扫描"走
   `lvgl_port_suspend`；开机扫描走"持 LVGL 锁 + `lv_refr_now` 提交最后一帧"
   （`ui/boot_flow.c` 的 `boot_scan_progress_cb`）。**NVS 写入不需要额外处理**：
   写 flash 期间两颗核都被停住，不会与 LVGL 渲染并发（见 `FLASH_WRITE_VS_PSRAM.md`）。
5. **降级路径**：镜像 magic/版本/长度任一不对 → 自动回退内嵌精简字库（见 §5）。
6. **旧字体残留**：`font` 分区里若还是 TTF（TTF 时代刷的），mmap 头 4 字节不是
   `NPF1`，同样走回退分支，不会误解析。
7. **缺字**：Droid Sans Fallback 缺部分 Latin-1 Supplement（如 `ä ö Ä`）与
   `U+2B00-2BFF`（如 `⭐`）；TTF 时代用同一份字体，所以这不是本次迁移引入的
   （详见 `legacy_s3` 分支的 `NPF_FONT.md` §7.2 与 `FontSymbols.md`）。

### 7.1 2026-11-11：开机扫描写入阶段 LVGL 卡死（S3 分支先修，已同步到本分支）

**现象**：扫描完成后不自动进主界面，界面定住再也不动；串口刷
`task_wdt ... CPU 0: LVGL`；扫描数据其实已经写进 flash，断电重启后一切正常。

**根因**：开机扫描的写入阶段（`playlist_store_save_playlist` 擦写 pldata）会关闭外部
flash 缓存；而系统字体是 **mmap 在 font 分区上的 NPF1 全字库**，LVGL 渲染文字时取字形
位图就是读这块 flash。写入期间 LVGL 若还在渲染，这次总线访问就卡在"缓存已关、
flash 忙"的状态里出不来 → LVGL 任务永久卡死。

**修法**（`ui/boot_flow.c` 的 `boot_scan_progress_cb`）：收到 `PLAYLIST_BUILD_PHASE_WRITE`
时 `lvgl_port_lock(-1)` 抢下 LVGL 锁 → 把状态改成"正在写入扫描数据..." →
`lv_refr_now(NULL)` 提交最后一帧；锁一直持到 `boot_scan_task` 写完 flash（含无音乐时的
`playlist_store_invalidate`）才 `lvgl_port_unlock()`（**在置 `s_scan_done` 之前**）。

**通用规则**：任何"映射在 flash 上的数据被渲染/读取"的路径，在 `pldata`/NVS 等 flash
写入期间都必须停止（LVGL 锁 + 提交最后一帧，或 `lvgl_port_suspend`）。以后新增
"写 flash"的功能时，先检查是否会与 LVGL 渲染重叠。

---

## 8. 验证记录（本次落地，2026-11-14）

- 构建：`idfc build`（debug）通过；`font_npf.c` 的 `_Static_assert`（glyph_dsc 16B /
  cmap 偏移）在 LVGL 9.4 上全部成立；
- 烧录：`idfc flash`（会一并写新分区表）+ `esptool write_flash 0x600000 spiffs/system.npf`
  （4,653,108 B 写入、`Hash of data verified.`）；
- 启动日志：`NPF1 全字库已挂载: 24px bpp2, 34462 个字形, 183 个 cmap`、
  `系统字体：NPF1 全字库`，随后 `App started`，无 LVGL 断言 / 看门狗；
- 界面：主界面/列表/正在播放/播放设置/下拉控制中心/设置菜单均正常显示。

---

## 9. 回退到内嵌字库（仅极端情况）

1. 代码上把 `system_font.c` 里的 `font_npf_init()` 跳过（或直接 `git revert` 本次提交），
   `UI_SYSTEM_FONT` 就会用 `system_global_font_lite`；
2. 分区表可改回 `font 4M @0x800000` / `factory 0x7F0000`（**pldata 不动**）；
3. `idfc flash` 重刷分区表与 app（旧 TTF 若还在 0x800000 会落在 font 分区内，不影响启动）。

> 注意：回退到精简字库后用户数据大量缺字——这正是本方案要解决的问题，
> 回退只在"字体镜像刷不进去"这类极端情况下考虑。
