# 字体系统（TTF / FreeType 迁移记录）

> ⚠ **本文是历史记录，已被 NPF1 位图方案取代（2026-11-14）**：`master` 现在与
> `legacy_s3` 一样，系统字体是 **font 分区里的 NPF1 预烘焙位图全字库（`spiffs/system.npf`，
> mmap 直读）**，不再使用 TTF / FreeType，`font_ttf.c`、`ftoption_lvgl.h`、
> `espressif/freetype` 依赖与相关 CMake 接线都已删除。现行方案见
> [NPF_FONT.md](NPF_FONT.md)；本文保留下来是因为 TTF 时代的**分区重排、栈预算、
> 关缓存陷阱**等结论仍有参考价值（改回位图后栈已可回落，见 NPF_FONT.md §6）。

> 2026-10 把系统字体从「内嵌位图字库」迁到 **font 分区里的 `system.ttf` + FreeType 按需光栅化**。
> （迁完先用的是 MiSans，实测滚动帧率明显更低，最终**固定为 Droid Sans Fallback**，见 §8.1。）
> 相关代码（**均已删除**）：`main/core/fonts/font_ttf.h/.c`、`main/core/fonts/ftoption_lvgl.h`、
> `main/core/fonts/system_font.h`、`main/main.c`、`main/CMakeLists.txt`、`partitions.csv`。
> 定位过程参考了另一个项目（OtoClock，`tempdir/oto-clock-master`，它把 TTF 放 SD 卡）的
> `ideas_and_exps/FontSystem.md`，但**承载方式、文件系统与栈预算完全不同**。

---

## 一、结论速览

| 项目 | 方案 |
| --- | --- |
| 字体文件 | `spiffs/system.ttf` = **Droid Sans Fallback**（AOSP，Apache-2.0），50951 字形，3,725,920 B（3.55 MiB）。**2026-10 起作为唯一字体固定使用**（此前用过 MiSans Normal，因实测滚动帧率明显更低而弃用，见 §8.1） |
| 存放位置 | **flash `font` 分区 @0x800000，4 MiB，裸二进制、无文件系统**（`partitions.csv`；2026-10-25 由 8 MiB 缩小 —— 字体只用 3.55 MiB，省下的 4 MiB 全还给 `factory`） |
| 引擎 | FreeType 2.14.3（`espressif/freetype` 组件，IDF 组件管理器拉取） |
| 读取方式 | `esp_partition_mmap` 整个分区 + 自写 lv_fs 驱动按偏移 `memcpy` |
| 字号 | 复用同一份 TTF：24px（正文/列表/标题）、20px（状态栏/时长/两行文字图标） |
| 回退 | TTF 不可用（font 分区空/损坏、或 `font` 维度选了 `binfont`）→ 自动用**内嵌位图字体**：`CONFIG_UI_SYSTEM_FONT_FULL=y` 时是完整版 `system_global_font`（24px，含 CJK 全集）+ `system_smaller_font`（20px），否则是精简版 `system_global_font_lite`。界面不会没字 |
| 构建维度 | **`font`**（`ttffont` 默认 / `binfont`，2026-10-25 加入配置矩阵）：`ttffont` = 本文档这一套（font 分区 + FreeType）；`binfont` = 完整版位图字库整份编进固件、关掉 TTF 与整个 FreeType。见 `BUILD_MATRIX.md` §1.8 |
| 固件体积 | debug：**ttffont 1.85 MiB**（`0x1d9060`，factory 余 77%）/ **binfont 3.84 MiB**（`0x3d8040`，余 52%）。⚠ binfont 的字库 `.rodata` 会被 XIP 拷进 PSRAM，可用 PSRAM 由 6.40MB 降到 4.35MB——这是**内嵌字库方案的固有特性**（TTF 迁移前的老方案亦然，属已知行为）。⚠ 百分比随 `factory` 大小而变：2026-10-25 把 factory 由 3.9375MiB 扩到 **7.9375MiB** 后才有这个余量（§2） |
| 开机耗时 | 字体探测 + mmap + 建两个字号 ≈ **53ms**，但**延迟到"进常规界面"时才发生**（2026-10-25 起，见 §9 第 8 条）：`font_ttf_init()` 在 `boot_enter_normal()` 里调用，boot 早期不再加载 |
| 烧录 | `idfc flash` **只烧 bootloader + 分区表 + app（约 19 秒）**，**有意不含字体**；字体要单独用手边的 esptool 手动刷进 `font` 分区（约 30~40 秒 / 3.55MiB）—— 命令、校验与换字体流程见 **`FONT_FLASH.md`** |

---

## 二、分区表改动（`partitions.csv`）

```
nvs      @ 0x9000   (24KB)
phy_init @ 0xF000   (4KB)
factory  @ 0x10000  (0x7F0000 = 7.9375MB)   ← 3.9375M
font     @ 0x800000 (4MB, subtype 0x41)     ← 8M，裸二进制
pldata   @ 0xC00000 (4MB, subtype 0x40)     ← 原 0x400000，现锚定在最尾部
                          合计正好 16MiB
```

**当前布局 = 2026-10-25 定稿（两次重排的结果）**，三条原则：

1. **`pldata` 必须待在分区表最末尾**。它是"整体重写 + 头部 commit"的只读 mmap 数据，
   偏移一变旧播放列表就作废、首次开机必须重扫。锚定尾部后，以后调整 `font` / `factory`
   只动它俩之间那一条线，`pldata` 再也不动。
2. `font` 与 `pldata` 的偏移都按 **64KB（`SPI_FLASH_MMU_PAGE_SIZE`）对齐**，否则无法整体内存映射。
3. `factory` 起始固定 0x10000（app 分区对齐要求），它和 `font` 平分中间那块空间、此消彼长。

- **为什么必须改**：16MiB flash 尾部原本只剩 5.94MiB 空闲，装不下 7.82MiB 的 MiSans 字体，只能从 `factory` 割。
- ⚠ **副作用**：`pldata` 偏移变了 → **旧播放列表数据作废，首次开机要走一次"重新扫描"**；
  `nvs` 偏移没动（0x9000），所以音量/亮度/上次播放位置等配置**不丢**。
- **2026-10-25 第二次重排（缩 font / 扩 factory / pldata 移尾）**：字体换成 Droid Sans Fallback 后
  只占 3.55MiB，8MiB 的分区纯属浪费，于是 `font` 8M→4M（余 457KB）、`factory` 3.9375M→7.9375M，
  并把 `pldata` 从中间挪到最尾部（0x400000→0xC00000）。要点：
  - **`font` 起始偏移仍是 0x800000 → 字体数据原样有效、不用重烧**（4MiB 装 3.55MiB 够）；
  - `pldata` 偏移这次又变了 → **旧播放列表再作废一次、首次开机再重扫一次**（这是最后一次）；
  - `nvs` 依旧未动，用户配置不丢；
  - 旧 pldata 区 0x400000~0x800000 现归 `factory`，但烧录只写 app 镜像长度内的数据
    （debug 1.78MiB / release 2.19MiB），擦不到那里。
- ⚠ 换更大的字体前先看文件是否仍在 **4MiB** 以内；超了就调大 `font` 并等量缩小 `factory`，
  **不必再动 `pldata`**（这正是把 `pldata` 移尾的目的）。

### 2.1 为什么是"裸分区"，而不是 SPIFFS / LittleFS

用 IDF 自带的 `spiffsgen.py`（精确复刻 IDF 的参数 `--page-size=256 --obj-name-len=32 --meta-len=4 --use-magic --use-magic-len`）
二分实测（当时放的是 7.82MiB 的 MiSans）：**8.5625 MiB 直接 `SpiffsFullError`，8.625 MiB 才通过** ——
也就是说 SPIFFS 装它至少要 **8.625 MiB**，额外吃掉 0.4~0.8 MiB，而本板最缺的就是 flash 空间。
裸分区按文件大小对齐即可，当时 **8 MiB 就够**（余 187,464 B）；现在换成 3.55MiB 的 Droid 后
**4 MiB 就够**（余 468,384 B）。并且：

- 不必每轮构建生成 8.75MB 镜像；
- 没有挂载扫描时间、没有常驻 FS 开销；
- 读路径可以整个 mmap（见第四节），比经 FS 层快。

代价：分区里只能放这一个文件，读写要自己写 lv_fs 驱动（约 100 行，已在 `font_ttf.c`）。

---

## 三、FreeType 接入：四个必须记住的坑

`main/CMakeLists.txt` 里的注释有完整版，这里给结论：

### 3.1 `CONFIG_LV_USE_FREETYPE=y` 只编译源码，**不加任何依赖**

LVGL 自己的 `env_support/cmake/esp.cmake` 在 `LV_USE_FREETYPE=y` 时只把 `src/libs/freetype/*.c` 加进编译，
`lv_freetype_private.h` 里的 `#include "ft2build.h"` 找不到头文件。
必须由**我们自己**把 `espressif__freetype` 链接给 **lvgl 组件**（不是链接给 main）：

```cmake
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC idf::espressif__freetype)
```

> 参考项目用的是 `espressif/esp_lvgl_adapter`（`CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE`）在做这件事；
> 本项目是自己的 `lvgl_port`，没有适配器，所以自己做。

### 3.2 必须给 lvgl 补 `FT2_BUILD_LIBRARY`

LVGL 的 `src/libs/freetype/lv_ftsystem.c` 用了 FreeType 的**内部**接口（`FT_THROW` / `FT_ERR_PREFIX`），
而 `freetype/fterrors.h` 末尾是：

```c
#ifndef FT2_BUILD_LIBRARY
#undef FT_ERR_PREFIX          /* ← 于是 FT_THROW 展开成 FT_ERR_PREFIXxxx 这种鬼东西 */
#endif
```

上游 freetype 目标只把它设成 **PRIVATE**，外部使用者看不到。报错长这样：

```
error: 'FT_ERR_PREFIXCannot_Open_Resource' undeclared (did you mean 'FT_Err_Cannot_Open_Resource'?)
```

修法：`target_compile_definitions(${lvgl_lib} PRIVATE FT2_BUILD_LIBRARY)`。

### 3.3 ⚠⚠ 必须让 **LVGL 的 `lv_ftsystem.c` 赢得链接**

FreeType 自带的 `src/base/ftsystem.c`（stdio `fopen/fseek` + newlib `malloc`）就在 `libfreetype.a` 里，
链接器扫这个库时会顺手把它拉进来，后果是：

- **文件访问不走 `lv_fs`**：LVGL 明明用 `lv_fs` 打开了 `F:/system.ttf`，FreeType 底下却拿路径去 `fopen()`
  → `FT_New_Face error(0x1)`（Cannot_Open_Resource），界面静默回退位图字体；
- **内存分配不走 `lv_malloc`**：FreeType 的内部结构（本字体 cmap 22KB、loca 119KB）会落到 newlib malloc
  = **内部 RAM**，本板开机后内部 RAM 只剩几十 KB —— 等于 `CONFIG_LV_FREETYPE_USE_LVGL_PORT=y` 完全失效。

实测证据：`FakePodNanoNightPlayer.map` 里 `FT_Stream_Open ... libfreetype.a(ftsystem.c.obj)`，
而 `lv_ftsystem.c.obj` 根本没进链接。

修法（沿用本项目 `lv_malloc_core` 的同款手法）：链接开始前把这三个符号标为未定义，
扫到 `liblvgl.a` 时就先拉入 `lv_ftsystem.o`，之后 `ftsystem.o` 因符号已满足不会被拉入（否则 multiple definition）：

```cmake
target_link_options(${lvgl_lib} INTERFACE
    "-Wl,-u,FT_New_Memory" "-Wl,-u,FT_Done_Memory" "-Wl,-u,FT_Stream_Open")
```

实测链接顺序是 `libmain.a → liblvgl__lvgl.a → libfreetype.a`，所以这个顺序可靠。

### 3.4 ⚠⚠ 光栅化器把 **16KB 渲染池放在栈上** → 必须压小（本次崩溃的根因）

FreeType 的平滑光栅化器：

```c
/* smooth/ftgrays.c */
#if FT_RENDER_POOL_SIZE > 2048
#define FT_MAX_GRAY_POOL  ( FT_RENDER_POOL_SIZE / sizeof ( TCell ) )
#else
#define FT_MAX_GRAY_POOL  ( 2048 / sizeof ( TCell ) )     /* 2048 是下限 */
#endif
...
    TCell  buffer[FT_MAX_GRAY_POOL];      /* ← gray_raster_render 的局部变量 = 栈！ */
```

`FT_RENDER_POOL_SIZE` 默认 **16384**，即**单次 `FT_Render_Glyph` 就要 16KB 栈**。
本板内部 RAM 紧张（开机完成只剩 ~30KB），不可能给每个"可能渲染字形"的任务都加 16KB。
所以用 FreeType 官方的定制口子（`ftheader.h` 里 `#ifndef FT_CONFIG_OPTIONS_H`）
把池子压到 **4KB**：`main/core/fonts/ftoption_lvgl.h` 先原样 `#include <freetype/config/ftoption.h>`
再 `#undef/#define FT_RENDER_POOL_SIZE 4096L`，CMake 里以
`FT_CONFIG_OPTIONS_H=<绝对路径>` 注入给 **freetype 目标**。

池子不够时 `ftgrays.c` 会自动把字形切成水平"带"多次光栅化（banding），**结果位图不变**、只是复杂字形稍慢；
而且每个字形只光栅化一次，之后走 LVGL 的字形缓存。

---

## 四、字体读取：自写 lv_fs 驱动 + mmap

裸分区没有"文件长度"这种元数据，而 FreeType 的流长度是**通过 `lv_fs_seek(END)` + `lv_fs_tell` 得到**的
（`lv_ftsystem.c` 就这么干），所以 `font_ttf.c` 在初始化时：

1. 校验 sfnt 头（`0x00010000` / `true` / `OTTO` / `ttcf`；`0xFFFFFFFF` 说明分区是空的 → 提示先烧字体）；
2. 遍历表目录，取 `max(offset+length)` 按 4 字节对齐，**算出字体文件真实长度**（当前字体 3,725,920 B）；
   顺便打印 `maxp` 里的字形数（50951）作为"换没换字体"的指纹；
3. 注册 lv_fs 驱动（盘符 `F`）：`open/close/read/seek/tell` 全部只读，路径固定 `/system.ttf`
   （LVGL 传给 `open_cb` 的是**去掉盘符后**的路径，见 `lv_fs.c: lv_fs_resolve_path`）；
   ⚠ `lv_fs_drv_register()` 只保存**指针**（`lv_fs.c: *new_drv = drv_p`），所以驱动结构体必须 static；
4. **`esp_partition_mmap` 整个 8MB 分区**，之后 `read_cb` 直接 `memcpy`。

### 4.1 为什么一定要 mmap（而不是 `esp_partition_read`）

`esp_flash_read()` 在「目标缓冲区不在 DRAM」+「分区**未映射**」时会退化成重路径：
**关外部内存缓存 + 申请 DRAM 临时缓冲中转 + 停住另一核**（见 `esp_flash_api.c` 的
`direct_read` / `temp_buffer` 分支）。而 FreeType 的缓冲都是 `lv_malloc`（PSRAM）→ 每次读都走这条重路径。
渲染一个汉字要读 3~6 次 → 每次界面首刷都要关/开缓存几百次、反复停另一核。

mmap 之后读命中 flash 缓存，不关缓存、不停核、不申请临时缓冲，顺带快很多。
mmap 失败时自动退回 `esp_partition_read`（功能可用但慢，会打 W 级日志）。

### 4.2 回退链

TTF 不可用时（`font` 分区为空/字体损坏/`CONFIG_UI_FONT_TTF=n`），`font_ttf_get()` 一律返回**内嵌位图字体**：

| 请求字号 | 回退字体 | 说明 |
| --- | --- | --- |
| 24px（`UI_FONT_PX_MAIN`） | `system_global_font_lite` | 始终编译进固件（约 100KB） |
| 20px（`UI_FONT_PX_SMALL`） | `system_smaller_font` | 20px 点阵 |

所以 UI 代码永远可以无脑用 `UI_SYSTEM_FONT` / `UI_SMALL_FONT`。
⚠ 这两个宏现在是**运行时函数调用**（不是常量指针）：不要用在静态初始化里。
⚠ `UI_SYSTEM_FONT_FULL`（内嵌完整字库）在 `CONFIG_UI_FONT_TTF=y` 时被 Kconfig 的 `depends on !UI_FONT_TTF` 屏蔽，
release/releasecandidate 里那两行 `CONFIG_UI_SYSTEM_FONT_FULL=y` 已注释掉 —— 完整字库 2.4MB 不再进固件。

---

## 五、崩溃定位实录：主任务栈溢出写坏 .bss

这一段是本次迁移最有价值的记录 —— **现象极其误导，最后靠硬件写监视点才抓到真凶**。

### 5.1 现象

TTF 生效后一进主界面就 panic，且**每次开机必然复现**：

```
I (1739) boot_flow: enter normal UI
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
PC: spinlock_acquire  ← 由 esp_pm_lock_acquire / esp_pm_impl_idle_hook / leave_idle 调用
A2: 0x000000d4        ← 不存在的"锁句柄"
```

不同次崩溃还会落在 Core1 空闲任务、句柄值变成 `0x19`，并伴随
`E task_wdt: esp_task_wdt_reset: task not found`。也就是说 **`.bss` 里 `esp_pm` 的静态量被写坏了**，
而受害者 `s_rtos_lock_handle` 在 map 里位于 `0x3fcbe514`，`task_wdt` 的静态量（`p_twdt_obj`/`init_context`）就在它**后面 132 字节**。

### 5.2 二分（每一步都是一次真实构建 + 烧录 + 观察）

| 试验 | 结果 | 结论 |
| --- | --- | --- |
| `CONFIG_UI_FONT_TTF=n`（回到位图字体） | 不崩 | 与 FreeType 运行时有关 |
| 建字体但**不渲染**（`s_ready=false`） | 不崩 | 不是字体创建/分区读的问题 |
| 读路径改成 mmap + memcpy | 照样崩 | 与 flash 读路径无关（**因此换 LittleFS 也救不了**） |
| 只预创建 24px 一个字号 | 照样崩 | 与"两个字号共用一个 FT_Face"无关 |

### 5.3 硬件写监视点抓凶

`esp_cpu_set_watchpoint(0/1, 0x3fcbe514 / 0x3fcbe518, 4, ESP_CPU_WATCHPOINT_STORE)`
（两个核都要装，地址取自 map；Xtensa 调试异常向量把命中当 panic 处理，
`xt_debugexception` 在 IDF 里是**弱符号、无实现**，所以命中会打印**写入者**的 PC 与回溯）：

```
Debug exception reason: Watchpoint 1 triggered
--- 0x420d0ef4: gray_set_cell at .../freetype/src/smooth/ftgrays.c:621
Backtrace: gray_set_cell → gray_render_line → gray_line_to → FT_Outline_Decompose
           → gray_convert_glyph_inner → gray_raster_render → ft_smooth_render
           → FT_Render_Glyph → freetype_image_create_cb → lv_cache_acquire_or_create
           → LVGL 绘制 → lv_refr_now → app_main
```

### 5.4 真因

看这次 panic 的栈指针：

- 最外层 `main_task` 帧在 **0x3fcc3180**，最深层 `gray_set_cell` 在 **0x3fcbe040** → 这条链用了 **~20.5KB 栈**；
- 其中 `gray_convert_glyph_inner(0x3fcbe150) → gray_raster_render(0x3fcc2280)`
  **单个帧就占 ~16.5KB** —— 正是 `FT_RENDER_POOL_SIZE`（16384）那块栈上渲染池；
- 而主任务栈当时只有 **8KB**（`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`）。

**即：`app_main` 里"持锁渲染首帧"（`boot_splash_show` + `lv_refr_now`）串联到 FreeType 光栅化，
栈溢出 12.6KB 跌进 `.bss`，把 `esp_pm`/`task_wdt` 的静态量一起写坏；随后 PM 的中断/空闲钩子拿到垃圾句柄就崩。**
位图字体时代这条链浅得多，所以从没暴露。

> 顺带说明：`task_wdt: task not found` 不是独立故障，而是同一片内存被写坏的连带现象。

### 5.5 修复（四处，缺一不可）

| 位置 | 改动 | 依据 |
| --- | --- | --- |
| `core/fonts/ftoption_lvgl.h` + CMake | 渲染池 16KB → **4KB** | 省下 ~12KB 栈，是所有渲染任务的公共红利 |
| `sdkconfig.defaults` | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 8192 → **16384** | 实测 `MAIN-STACK-HWM: 7100 B free of 16384 B`（用 ~9.3KB）；该栈只在 app_main 期间占用，返回后释放 |
| `core/ui/lvgl_port.c` | LVGL 任务栈 12KB → **20KB** | 4KB 池子下实测 12KB 只剩 **172 字节**；改后 HWM 余 ~8.3KB |
| `core/utils/deep_worker.c` | 8KB → **12KB** | 它要跑"息屏唤醒 → `lv_refr_now` 全屏重刷"，同一条深链（早期创建，内部 RAM 尚有 200KB，12KB 能分配成功） |

⚠ 以后凡是**可能同步渲染 LVGL（尤其会光栅化字形）**的任务，栈都要按 ≥12KB 预算：
目前是 main / LVGL / deep_worker 三个。调 `FT_RENDER_POOL_SIZE` 时这几个任务的余量要一起看。

### 5.6 手法备忘（值得复用）

- **`esp_cpu_set_watchpoint` + `ESP_CPU_WATCHPOINT_STORE`** 是排"谁写坏了某个 .bss 变量"的利器：
  地址从 `build/<cell>/FakePodNanoNightPlayer.map` 查（改代码后要重新核对），两个核都要装；
  Xtensa 调试异常会走 `_xt_debugexception` → `PANIC_RSN_DEBUGEXCEPTION` → 串口打印写入者回溯。
  ⚠ 诊断代码本身不要新增静态变量，否则地址会漂。
- **二分时优先关"运行时用法"而不是关整个组件**：本次"建字体但不渲染"那一步（临时 `s_ready=false`）
  一步就把范围缩到"光栅化"。

---

## 六、刷入与更新流程

> ⚠ **2026-10-25 改版**：字体不再由构建系统烧录。原来那条 `font-flash` ninja 目标
> （`idfc idfpy font-flash`）已从 `main/CMakeLists.txt` **整段移除**，改为"分区表只保留位置 +
> 用 esptool 手动刷一遍"。**完整刷入文档（何时必须刷、怎么校验、出错表现、换字体流程、
> 历史坑）见 [`FONT_FLASH.md`](FONT_FLASH.md)**；本节只留开发视角的要点。

```powershell
idfc build                       # 增量构建
idfc flash                       # 烧 bootloader + 分区表 + app（约 20 秒，**不含字体**）
esptool.exe --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf   # 字体（约 30~40 秒；首次装机、换字体、font 偏移变了才需要）
```

- `idfc flash` **故意不含字体**（3.55MiB，每轮多烧 30~40 秒不值）。字体在**固件之外**，
  用装机时手边本来就会有的 **esptool** 直接写 `font` 分区（偏移见 `partitions.csv`）——
  构建系统里因此不再需要任何自定义目标，也顺带回避了 `idf.py <自定义目标>` 的端口问题。
- ⚠ 换**分区表**后要确认 `font` 分区的**起始偏移**变没变：
  - 偏移变了（例如从无到有）→ 必须重刷一次字体，否则 font 分区是空的：
    固件仍能启动，但会打印 `font 分区里不是 TTF/OTF（头 4 字节 = 0xFFFFFFFF）` 并回退内嵌位图字体；
  - 偏移没变、只是分区变小了（2026-10-25 这次 8MiB→4MiB，起点仍 0x800000）→ 字体数据原样保留，
    **不需要重烧**，4MiB 仍装得下 3.55MiB 的字体。
- 换字体：**改名覆盖 `spiffs/system.ttf`**（见 §6.1），然后 `idfc build` +
  `esptool.exe --chip esp32s3 -p COMx write_flash 0x800000 spiffs/system.ttf`。
  启动日志里的"N 张表，M 个字形"就是当前生效字体的指纹。
- **2026-10-25 实测**（esptool 4.12 + USB-Serial/JTAG）：手动刷 3.55MiB 用时 **33.4 秒**
  （893 kbit/s），结尾 `Hash of data verified.`；读回 3.55MiB 校验 MD5 与源文件**完全一致**
  （读取很慢，324 秒 / 92 kbit/s，且首次读还偶发过一次 `Corrupt data`，重试即通过）。
  删除 `font-flash` 目标后 `idfc build` 一切正常，只是多打一行提示
  （`-- 提示：font 分区（0x800000）的内容需手动刷入 …`）。

### 6.1 单一字体文件约定（换字体 = 改名覆盖）

**`spiffs/system.ttf` 是唯一字体文件，运行期固定按 `F:/system.ttf` 打开**（`core/fonts/font_ttf.c`
的 lv_fs 驱动；font 分区是裸二进制、只放一个文件），所以在仓库里它的**名字与路径也是写死的**
（刷进分区的内容就是这个文件本身）。
所以**换字体不需要改任何代码/Kconfig**，三步：

1. 把新字体文件**改名成 `system.ttf` 覆盖** `spiffs/system.ttf`（想留旧的就自己先备份到别处）；
2. `idfc build`（一般不用重编，只是确认配置没过期）；
3. **手动重刷字体分区**：`esptool.exe --chip esp32s3 -p COMx write_flash 0x800000 spiffs/system.ttf`
   （约 30~40 秒 / 3.55MiB；命令细节与校验见 `FONT_FLASH.md`）。

启动日志的 "N 张表，M 个字形" 就是校验指纹，用于确认生效的是哪份。

当前字体的关键参数（换字体时对照，尤其**行高**会影响布局）：

| 项 | 值 |
| --- | --- |
| 名称/来源 | Droid Sans Fallback（Android 时代 CJK 回退字体，AOSP，2011） |
| 许可 | Apache-2.0，`Copyright (C) 2012 The Android Open Source Project`；文本入库在 `spiffs/DroidSansFallback-LICENSE.txt`（⚠ 只是仓库文件，**不打进固件**、不占分区，刷入设备时也不需要它） |
| 体积 / 字形数 | 3,725,920 B（3.55 MiB）/ 50951 |
| upem | 256 |
| 行高 | 24px → **31**，20px → **26**（基准线 7 / 6） |

⚠ 行高变化会影响布局：`settings_page.c` 里那个两行文字图标用的是 `text_line_space=-6`（按 20px 行高
26 调成 2×26-6=46，放得进 48px 图标区）。**换字体后要复核这个值和列表行高**（`virtual_list` 用
`lv_font_get_line_height` 算行高）。
⚠ 观感取舍：Droid 的拉丁字母/数字比现代 UI 字体朴素（它是为低端设备省空间设计的），换来的是
**每字形轮廓数据只有 MiSans 的约 1/4**、滚动帧率明显更高（见 §8.1）。

---

## 七、实测数据（2026-10-25，debug/cs43131，Droid + 4MiB font 分区）

```
I (700) font_ttf: 字体信息：TrueType(glyf)，18 张表，50951 个字形
I (708) font_ttf: font 分区：offset=0x800000 size=4194304 B；字体文件实际长度=3725920 B（3.55 MiB）
I (719) font_ttf: font 分区已 mmap 到 0x3c800000（4194304 B），字形读取走 memcpy
I (740) font_ttf: TTF 字体就绪：F:/system.ttf 24px（行高 31，基准线 7）
I (741) font_ttf: TTF 字体就绪：F:/system.ttf 20px（行高 26，基准线 6）
I (746) font_ttf: 系统字体已切换到 TTF（FreeType，字形按需从 font 分区读取）
I (1020) pldata: pldata partition: offset=0xc00000 size=4194304 (4096 KB)
W (1197) boot_flow: playlist data invalid, prompt to scan   ← 分区重排的预期副作用，重扫一次即恢复
I (22609) MEM: stk: deep_worker=11504 ... LVGL=15660 sleep_timer=3420 ...
```

| 指标 | 实测 |
| --- | --- |
| 字体初始化 | 693ms → 746ms（**53ms**：探测 + mmap + 建两个字号） |
| 进主界面 | `App started` @1268ms。⚠ 这次不可与迁移前的 ~1753ms 直接比：分区重排让播放列表数据作废，开机停在"提示扫描"界面，少建了列表页 |
| LVGL 任务栈 | HWM 15660 B free / 20480 B |
| deep_worker 栈 | HWM 11504 B free / 12288 B |
| 主任务栈 | HWM 7100 B free / 16384 B（迁移时测，本次未打印） |
| 内部 RAM | `MEM: iram=69983`（约 68KB 空闲）；MEMTRACE 开机完成时 int 51995 / dma 44279 |
| PSRAM | `heap=6437395`（约 6.14MB）**全程 +0 不动**，无泄漏 |
| 空闲 CPU | Core0 **3%**（静止界面） |
| 固件 | debug `0x1c7d10` = **1.78 MiB**；factory 分区 `0x7f0000` 中 **余 78%**（`0x6282f0`），扩分区前是余 55% |

### 7.1 行高变化对布局的影响

| 用途 | 迁移前（点阵） | 现在（Droid TTF） | 处理 |
| --- | --- | --- | --- |
| 系统正文 24px | 精简版 26 / 完整版 33 | **31** | 列表行高随之变化（`virtual_list` 用 `lv_font_get_line_height` 算行高），属预期 |
| 小字体 20px | 24（MiSans-Bold 生成） | **26**（Droid 常规字重） | `settings_page.c` 的两行文字图标用 `text_line_space=-6` 收紧（2×26-6=46，放进 48px 图标区）；时间/图标文字由粗变常规 |

> 对照：MiSans 时代这两项分别是 **32 / 27**（当时 `text_line_space` 是 2×27-6=48）。换字体后务必复核。

---

## 八、性能与后续可调项

**结论**：字形是**按需光栅化**的，某个汉字第一次出现要现场做 `FT_Load_Glyph` + `FT_Render_Glyph`，
之后走缓存。因此：

- 静态界面（正在播放页、状态栏）空闲时 Core0 只有 4~5%；
- **ClickWheel 在长列表上快速滚动**时会成批产生新字形 → 实测连续滚 55 秒期间 Core0 全程 97~99%，
  帧率掉到 10fps 量级（位图字体时代同一手势约 20fps）。ClickWheel 的机制本身看不出卡顿，但这是真实开销。

### 8.1 换字体是最大的单项收益（2026-10 实测）

固定改用 **DroidSansFallback 后，同一手势的滚动帧率明显高于 MiSans**。最可能的原因是
**单字形的轮廓数据量只有约 1/4**：

| | `glyf` 表 | 字形数 | 平均每字形轮廓 | 文件 |
| --- | --- | --- | --- | --- |
| MiSans | 7280 KB | 29758 | ~245 B | 7.82 MiB |
| Droid | 3224 KB | 50951 | **~63 B** | 3.55 MiB |

轮廓点越少 → 光栅化（含分带）越便宜；同时整个字形数据小一半，从 mmap 的 flash 读进来时缓存
局部性也更好。Droid 是 2011 年为低端 Android 设计的回退字体，字形刻意画得简单，正好适合这种
"每帧现算"的场景（代价是拉丁字母/数字观感朴素）。

### 8.2 缓存与其它可调项（按性价比排序）

0. **换字体文件**（见 §8.1）：改名覆盖 `spiffs/system.ttf` 再手动重刷字体分区
   （`esptool.exe --chip esp32s3 -p COMx write_flash 0x800000 spiffs/system.ttf`），
   零代码改动、收益最大。
1. **`CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT`**（当前 **1024**；它同时管两个 LRU：度量缓存 = 本值×2、
   **位图缓存 = 本值**）：**只吃 PSRAM**，实测填满约 0.61MB，之后完全稳定（无泄漏）。
   ⚠ 实测结论：**对"整列表从顶滚到底"帮助有限** —— 8504 首 × 每行约 15 个汉字，不重复字形远超 1024 条，
   LRU 必然颠簸（CPU 仍 97~99%）；它的收益场景是**局部来回滚动**（同一屏附近反复搓，第二遍起基本全命中）。
   要覆盖整列表得几千条（2MB+ PSRAM）仍不够，所以不必再往上堆。
2. **`FT_RENDER_POOL_SIZE`**（当前 4096，见 `ftoption_lvgl.h`）：调大减少分带次数、光栅化更快，
   但**每个渲染任务 +N 字节栈**（LVGL/deep_worker/main 三个都要重新核对余量）。
3. **列表页减少每帧重新排版的文本量**：`virtual_list` 的行绑定 + 跑马灯在快速滚动时是最贵的路径。
   位图时代 20fps 的预算里已有一半花在这条路上（换字体解决的是另一半），要再往上压就得动它。

---

## 九、已知遗留 / 注意事项

1. **20px 原来是 MiSans-Bold 点阵，现在的 TTF 只有 Droid Sans Fallback 的常规字重**：
   时长、"回放/增益"等文字由粗体变常规。想要粗体就得换一份带 Bold 的字体（`font` 分区
   4MiB 里现在还剩 457KB，第二份字体放不下，只能把系统字体整个换掉）。
   LVGL 的 `LV_FREETYPE_FONT_STYLE_BOLD` 在 **bitmap 渲染模式下不生效**（合成加粗只支持 outline 模式）。
2. **`power_service` 的 deep_worker 兜底路径**：`deep_worker_call()` 失败时会在 `power_svc`（3KB 栈）
   里就地执行亮屏/息屏深链 —— TTF 之后这条兜底会爆栈。它只在"deep_worker 起不来/卡死"时才走
   （此时系统本已异常），但如果要彻底稳妥，应改成"只记录、不再就地渲染"。
3. **字体文件大小受 `font` 分区 4MiB 限制**（2026-10-25 起；换更大的字体要调大 `font`
   并等量缩小 `factory`，**不必再动 `pldata`** —— 它已锚定在分区表最末尾）。
4. `spiffs/` 目录名沿用参考资料的习惯，但里面**不是 SPIFFS 镜像**，就是裸 TTF 文件本身。
5. 调试版开了 `CONFIG_LV_USE_LOG=y` + `CONFIG_LV_LOG_PRINTF=y`（字体排障用；LVGL 默认打印回调是空的，
   只开前者日志会被静默丢弃）。⚠ LVGL 日志级别数字是反的（TRACE=0 < … < ERROR=3 < USER=4），
   调到 `USER` 反而会把 ERROR 过滤掉。
6. 应用侧**不要**调 `lv_freetype_init()`：LVGL 9.4 的 `lv_init()` 已经初始化过（`lv_init.c` 里带
   `LV_FREETYPE_CACHE_FT_GLYPH_CNT`），重复调用会 `freetype already initialized` 并返回
   `LV_RESULT_INVALID`（引擎其实是好的）——参考项目踩过这个坑。
7. **扫描的"写入阶段"会让 LVGL 停摆、文字变方框 → 已修复**（2026-10-25 发现并解决）：
   写 pldata 走 `esp_partition_erase/write` 会**关闭外部内存缓存**，此时 PSRAM 不可访问，而
   LVGL 对象堆（`core/ui/lvgl_mem.c`）与 FreeType 的字形位图缓存都在 PSRAM → 若那一刻 LVGL
   正在渲染，这一帧就停摆、新字形取不到（屏幕出现方框、进度界面冻住）。属"关缓存陷阱"又一例。
   **修法**（`ui/boot_flow.c`）：扫描进度回调在 `PLAYLIST_BUILD_PHASE_WRITE` 阶段
   `lvgl_port_lock(-1)` 把 LVGL 锁抢过来 → 主动把文案改成"正在写入扫描数据..."并
   `lv_refr_now()` 提交最后一帧 → 整个写入期间 LVGL 冻结不渲染 → `boot_scan_task` 在写完
   flash（含无音乐时的 `playlist_store_invalidate`）后 `lvgl_port_unlock()`；随后进度定时器
   才拿得到锁、切到主界面/无音乐页。也就是"宁可界面定格，也不要跑到不可恢复的状态"。
   ⚠ 两条硬前提：① 扫描任务栈必须在**内部 RAM**（关缓存期间 PSRAM 栈不可访问，见
   `BOOT_SCAN_STACK_BYTES` 注释）；② **解锁必须早于置 `s_scan_done`**（定时器要靠这把锁
   才能跑起来）。⚠ 写入期间若触发息屏/深链，会等这把锁直到 deep_worker 的 5s 超时
   （只打告警、不影响正确性；扫描时本也不该息屏）。
   ⚠ 内嵌回退位图字库不受此问题影响（在 `.rodata`、渲染不需要运行时分配）——顺带发现并
   补齐了回退字库的字形覆盖，见 `FontSymbols.md`。
8. **TTF 改为延迟加载**（2026-10-25，用户要求"只有正常启动流程才加载"）：
   `font_ttf_init()` 不再由 `main.c` 在开机早期调用，而是**只在 `boot_enter_normal()`**
   （确实要进常规界面时）调用一次；低电 / 无卡 / 待扫描这几个非正常启动分支全程使用
   内嵌位图字体。代价：进常规界面时多花约 53ms（字体加载从"开机封面背后并行"变成
   "锁内串行"）——用户明确表示此时开机速度不再是约束。收益：boot 阶段完全不加载 TTF，
   不占 PSRAM 字形缓存、不依赖 FreeType，扫描写 flash（关外部内存缓存）期间又少一处
   "字形取不到 → 方框"的风险。
   ⚠ 由此 `font_ttf_init()` 的契约也变了：**要求调用方已持 LVGL 锁**（原先内部自己
   加锁）。因为 `boot_enter_normal()` 运行在 `boot_flow_run()` 的锁内，而 `lvgl_port_lock`
   用的是**普通 mutex、不可重入**，内部再加锁会直接死锁。
9. **第 7、8 两条机制的完整闭环实测**（2026-10-25，debug/cs43131，用户手动操作 + 串口日志）：
   路径 = 设置菜单「重新扫描」→ 失效分区并 `esp_restart` → 停在"需要扫描音乐" → 点「开始扫描」
   → 扫描 172 个目录 / 8603 首 → 写入分区 → 进播放界面。关键证据：

   ```
   I (1131) app_main: Run boot flow
   W (1135) boot_flow: playlist data invalid, prompt to scan   ← 之后**没有任何 font_ttf 日志**
                                                                 （= 内嵌位图字体，延迟加载生效）
   I (1627) boot_flow: scan task begin
   I (3733) playlist_builder: scan done: dirs=172 songs=8603 str_len=336661 total=598805
   I (3746) boot_flow: write phase: LVGL locked, UI frozen until write finishes   ← 抢锁冻结
   I (3749) playlist_builder: saving playlist to partition (len=598805) ...
   I (9288) playlist_store: playlist saved: len=598805 crc=0xae87dbd2
   I (9290) boot_flow: write phase: LVGL unlocked, UI resumes                     ← 释放
   I (9590) font_ttf: 字体信息：TrueType(glyf)，18 张表，50951 个字形              ← 写完才加载 TTF
   I (9626) font_ttf: 系统字体已切换到 TTF（FreeType，字形按需从 font 分区读取）
   I (10205) boot_flow: enter normal UI
   ```

   - 写入耗时 **5.5 秒**（3746→9290），界面全程定格在"正在写入扫描数据..."（用户确认），
     **没有再出现方框**——正是设计意图：宁可定格，也不在 PSRAM 不可访问的窗口里渲染；
   - PSRAM 由写入期的 3.33MB 恢复到解锁后的 5.67MB（`heap=3333711` → `5732095`），无泄漏；
   - `MEM: stk: … LVGL=10852 …`：20KB 的 LVGL 任务栈在渲染完主界面后仍余 10.8KB，健康。
   ⚠ **顺带复现了一次老问题**：扫描早期出现 `E sdmmc_cmd: allocate_dma_buf: not enough mem`
   + `diskio_sdmmc: sdmmc_read_blocks failed (0x101)`（仅 1 次，重试后成功，最终 8603 首与
   写入前一致）。这是内部 RAM/DMA 余量偏低导致的瞬时失败（见 AGENTS.md 与
   `IRAM_DMA_AUDIT.md`），与本次字体改动无关，但说明余量仍然紧张。
