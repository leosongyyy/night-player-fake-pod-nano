# ESP32-S31 PPA 加速实验记录（LVGL 9.4）

> 2026-11，S31（FakePodNano Pro V1）移植后尝试给 LVGL 接 PPA。
> 结论先行：**PPA 的纯色 FILL 和"不透明图像混合"都比 CPU 慢**；
> 只有"半透明 RGB565 图像混合"PPA 才快（1.4–2.4 倍），
> 而**这套 UI 里这种混合一次都不发生**（实测 0%）。
> 因此 PPA 混合 handler 虽已按官方做法接入（`core/ui/lvgl_ppa_accel.c`），
> 实际一次也不会命中；真正的性能收益来自调查过程中修掉的两个 bug
> （绘制缓冲/封面缓冲未 64B 对齐、诊断日志刷屏）。
> 详细数据：§4（FILL，第一阶段）、§6（blend，第二阶段）、§6.2（混合流量画像）。

## 1. 背景

- 硬件：ESP32-S31，PPA（Pixel Processing Accelerator）可用
  （`CONFIG_SOC_PPA_SUPPORTED=y`），IDF 6.1 的 `esp_driver_ppa`。
- 软件：LVGL 9.4.0，`CONFIG_LV_USE_PPA` 是 LVGL 自带的实验性 draw unit
  （`src/draw/espressif/ppa/`），在 `lv_init()` 里自动注册。
- 官方固件日志（用户提供）：
  ```
  PPA: register lvgl blend handle ok
  PPA: register lvgl fill handle ok
  PPA: register blend handle ok.
  PPA: register fill handle ok.
  PPA: register srm handle ok.
  ```
  说明官方不是用 LVGL 内置 draw unit，而是**自己注册 blend/fill client +
  自定义 blend handler**（与 OtoClock 用的 `esp_lvgl_adapter` v9 handler 同思路）。

## 2. 编译期问题（已解决）

### 2.1 托管组件 lvgl 的 `PRIV_REQUIRES` 没生效

`CONFIG_LV_USE_PPA=y` 后编译 `lv_draw_ppa*.c` 报：

```
fatal error: driver/ppa.h: No such file or directory
```

LVGL 的 `env_support/cmake/esp.cmake` 写了
`PRIV_REQUIRES esp_driver_ppa esp_mm ...`，但本工程（IDF 6.1 + 组件管理器）
实测没有把 include 目录传进 lvgl 的编译。`main/CMakeLists.txt` 里手动补：

```cmake
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(ppa_lib esp_driver_ppa COMPONENT_LIB)
idf_component_get_property(mm_lib esp_mm COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PRIVATE ${ppa_lib} ${mm_lib})
```

### 2.2 `CONFIG_CACHE_L1_CACHE_LINE_SIZE` 在 S31 不存在

`lv_draw_ppa_private.h` 的编译期检查：

```c
#if CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE != CONFIG_CACHE_L1_CACHE_LINE_SIZE
#error "For using PPA buffers need to be aligned to 64-byte boundary!"
#endif
```

S31 的 Kconfig 里叫 `CONFIG_CACHE_L1_DCACHE_LINE_SIZE`（=64），没有
`CONFIG_CACHE_L1_CACHE_LINE_SIZE`（那是 P4 的符号名）。补宏：

```cmake
target_compile_definitions(${lvgl_lib} PRIVATE
    "CONFIG_CACHE_L1_CACHE_LINE_SIZE=${CONFIG_CACHE_L1_DCACHE_LINE_SIZE}")
```

并在 `sdkconfig.defaults` 里设：

```
CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE=64
CONFIG_LV_DRAW_BUF_ALIGN=64
```

## 3. 运行期问题（已解决，且与 PPA 无关也必须修）

### 3.1 LVGL 图层缓冲没有 64B 对齐

`CONFIG_LV_DRAW_BUF_ALIGN=64` 后，左右滑动会刷屏：

```
W lv_draw_buf_init: Data is not aligned, ignored
```

根因：本项目的 LVGL 内存走 `core/ui/lvgl_mem.c` 自定义分配器（PSRAM），
原来只保证堆默认对齐（4/8B），而 LVGL 9.4 的 `lv_draw_buf_init()` **只警告
不纠正**，继续用未对齐指针。PPA 和部分绘制路径要求 64B 对齐，于是错位/掉帧。

修法：`lvgl_mem.c` 的所有分配改成 `heap_caps_aligned_alloc(64, ...)`，
`realloc` 改为 `aligned_alloc + memcpy + free`（`heap_caps_realloc` 不保证对齐）。
实测修复后警告消失，无 PPA 时帧率与之前一致（约 45fps 左右滑动）。

### 3.2 LVGL 内置 PPA FILL 不支持局部渲染

上游 `lv_draw_ppa_fill.c` 把目标块固定画在 buffer 左上角：

- `out.pic_w = 块宽`、`out.pic_h = 块高`；
- `block_offset_x/y = 0`；
- 忽略 `target_layer->buf_area`。

本项目是 `LV_DISPLAY_RENDER_MODE_PARTIAL`（缓冲高 46 行），于是每个缓冲块
的填充都错位；表现为**每 46 行一条横黑线**（正好一个绘制缓冲块）。
此外它用 `PPA_TRANS_MODE_NON_BLOCKING` 却立刻把 task 标记完成，刷屏可能
早于 PPA 写完。

修正（实验代码，未入库为正式 patch）：

```c
int buf_x = coords->x1 - layer->buf_area.x1;
int buf_y = coords->y1 - layer->buf_area.y1;
cfg.out.pic_w = draw_buf->header.w;   // 整块缓冲的步长
cfg.out.pic_h = draw_buf->header.h;
cfg.out.block_offset_x = buf_x;
cfg.out.block_offset_y = buf_y;
cfg.out.buffer_size = PPA_ALIGN_UP(draw_buf->data_size, 64);
cfg.mode = PPA_TRANS_MODE_BLOCKING;
```

修完后黑线消失，但**帧率没有提升**（见下）。

## 4. 实测数据（决定性）

在 debug 构建（320MHz、40MHz QSPI、16ms 刷新）下，给
`lv_draw_ppa_fill()` 加计时（每 128 次打印一次平均）：

| 版本 | PPA FILL 平均耗时 | 平均面积 | 现象 |
|---|---|---|---|
| 修偏移 + 阻塞 + burst 128 | **~290–330 µs/次** | 14k–18k px | 黑线消失，但滑动 45→14fps |
| burst 32 | **~285 µs/次** | 12k–17k px | 略好，但仍远慢于 CPU |
| 加面积门槛 8192px | 同左 | 14k px | 仍低于无 PPA |
| 去掉每帧整块 cache msync | 同左 | 14k px | 无变化，说明不是 cache 同步 |

结论：**PPA FILL 吞吐约 50M px/s**（1.5 万像素 0.3ms），而 320MHz CPU 的
RGB565 软件填充（32bit 写 + memset 类路径）远快于它。PPA 在 S31 上适合
blend（alpha 合成）与 SRM（缩放/旋转），**不适合纯色填充**。

补充：左右滑动时的 `not aligned` 警告已由 §3.1 的 64B 对齐修复消除；
官方适配器的 fill handler 也有 `面积 <= 100px 走 CPU` 的门槛，但那是
在 SW blend 回调内部调用，没有独立 draw unit 的调度开销；本项目用内置
draw unit，门槛要更高，可即便如此大块填充仍是负收益。

## 5. 第二阶段：照官方做法接自定义 blend handler（已实现）

按官方日志（`PPA: register lvgl blend handle ok`）与 OtoClock 的
`esp_lvgl_adapter` v9 实现，落地为 **`main/core/ui/lvgl_ppa_accel.c`**
（Kconfig：`CONFIG_UI_PPA_ACCEL`，实现要点见文件头注释）：

- `lv_draw_sw_register_blend_handler()` 注册 RGB565 目标的混合回调；
- **不透明图像直接回退 CPU**（PPA 在这条路上更慢，见 §6）；
- 半透明 / alpha 的 RGB565 图像且面积 ≥ `CONFIG_UI_PPA_BLEND_MIN_PX` 才走 PPA；
- 复杂 mask、A8、其它格式、小面积全部回退 CPU；
- 回退路径逐行复刻 `lv_draw_sw_blend.c` 的默认实现（含与 src_area /
  mask_area 的二次求交），保证位置/裁剪与原来完全一致；
- 纯色填充默认仍走 CPU（`CONFIG_UI_PPA_FILL_MIN_PX=0`）。

## 6. 第二阶段实测：PPA 混合到底划不划算

### 6.1 CPU vs PPA 单次耗时（460x46 内部 RAM 缓冲，源在 PSRAM，100 次平均）

| 场景 | CPU | PPA | 结论 |
|---|---|---|---|
| 纯色填充 21160 px | **51 µs** | ~300 µs | CPU 快 6 倍 |
| 不透明图像 21160 px | **91 µs** | 690 µs | CPU 是整行 memcpy，快 7.6 倍 |
| 半透明图像 21160 px | 1133 µs | **691 µs** | PPA 快 1.6 倍 |
| 半透明图像 10580 px | 566 µs | **393 µs** | PPA 快 1.4 倍 |
| 半透明图像 5290 px | 577 µs | **245 µs** | PPA 快 2.4 倍 |
| 半透明图像 2622 px | 286 µs | **170 µs** | PPA 快 1.7 倍 |

PPA 的固定开销约 100 µs，之后约 36 M px/s；CPU 的半透明混合约 22 M px/s。
**所以 PPA 只对"半透明 RGB565 图像"有正收益，且面积越小越接近打平。**

### 6.2 但这套 UI 根本没有这种混合（关键结论）

给 handler 加分类计数（`CONFIG_UI_PPA_STATS`）后，实测滑动时的混合流量：

| 类别 | 占调用数 | 平均面积 | 说明 |
|---|---|---|---|
| mask 填充（文字字形） | **~87%** | **140–250 px** | `src_buf=NULL` + 字形 mask，PPA 不支持 mask |
| 纯色填充 | ~9% | 10k–15k px | CPU 比 PPA 快 6 倍 |
| RGB565 不透明图像 | ~4% | 10k–19k px | CPU memcpy 比 PPA 快 7.6 倍 |
| RGB565 半透明图像 | **0%** | — | **从未发生** |
| 其它格式 | ~0% | 4x6 级别 | 图标碎片 |

统计里 `PPA 0 次` 是常态：**这套界面的混合流量里没有一条能落进 PPA 的
收益区**。文字是"mask 填充"（不是 A8 图像），PPA 的 blend 引擎没有 mask
输入；而每个字形只有 ~200 px，PPA 每次 100 µs 的固定开销是灾难。

### 6.3 顺带测出的两个真问题（与 PPA 无关，已修）

1. **诊断日志本身会把 UI 卡死**：统计原先是"每 500 次混合打一条"，
   滑动时 500 次只要 ~65 ms ⇒ **15 条/秒**；115200 波特的 UART 打一条
   ~10 ms（ESP_LOG 是同步阻塞写）⇒ 白扔 15% CPU 且阻塞 LVGL 任务。
   现象就是"开了统计的版本明显变卡"。已默认关闭 `CONFIG_UI_PPA_STATS`，
   并在 sdkconfig.defaults.debug 里写明**测性能时不要打开**。
2. **封面像素缓冲没有 64B 对齐**：`cover_jpeg.c` 原来用 `heap_caps_malloc`，
   而 LVGL 会把 `img->data` 包成 draw_buf（`lv_draw_buf_from_image`），
   `lv_draw_buf_init` 每次都对未对齐指针刷
   `W lv_draw_buf_init: Data is not aligned, ignored`。
   切歌后真实封面出现在正在播放页，滑动时每次解码/绘制都刷一条 ⇒
   "切歌之后变卡、只有带图片的页面卡"的直接原因就是它。
   已改为 `heap_caps_aligned_alloc(64, …)`，实测滑动 60 秒零告警。

### 6.4 flush 占空比（判断瓶颈在谁身上）

在 `lvgl_port.c` 里给"提交 → 面板传输完成"计时，每 2 秒打印一次：

```
flush 2s: 360 次, 平均 2166 us, 合计 779 ms, 占空 38%
```

滑动时 QSPI 占空 **30–41%**、平均每次 2.1 ms，CPU0 同时 70–91%。
即：**QSPI 不是唯一瓶颈，CPU 侧还有一半以上的活**；而这部分活按 §6.2
的分布主要是"大量小 draw task + 对象树/布局"，不是像素混合 —— 这正是
PPA 帮不上的地方（与 `IRAM_AUDIT_LVGL9.md` 的结论一致）。

## 7. 当前代码状态

- `CONFIG_UI_PPA_ACCEL=y`（默认开）：handler 已注册，但按 §6.2 实际
  **一次也不会命中 PPA**；保留它是为了"以后真的出现半透明大图"时自动生效，
  以及作为后续实验的骨架。不想要时可以关掉，行为与 LVGL 默认完全相同。
- `CONFIG_UI_PPA_BLEND_MIN_PX=1500`、`CONFIG_UI_PPA_FILL_MIN_PX=0`。
- `CONFIG_LV_USE_PPA` **仍关闭**（内置 draw unit 的 partial 支持问题见 §3.2）。
- `sdkconfig.defaults` 的 64B 对齐两项保留（PPA 与 LVGL 都要求）。
- `main/core/ui/lvgl_port.c`：绘制缓冲字节数向上取整到 64B（PPA 的
  `buffer_size` 必须对齐），并保留 flush 计时/占空比日志。
- `main/core/image/cover_jpeg.c`：封面像素缓冲 64B 对齐（§6.3）。
- 托管组件里的 `lv_draw_ppa*.c` 实验改动**没有**转成 `patches/`；既然
  `CONFIG_LV_USE_PPA` 不再启用，这些改动可以不管（重拉组件即还原）。

## 8. 后续可选方向

1. **（推荐的下一步）降低每帧 draw task 数量**：滑动时两个页面每帧整页重绘，
   实测 ~170 次混合/帧。可行方向：页面滑动期间用 `lv_snapshot` 把两页各拍成
   一张图，滑动只合成两张图；或把正在播放页的热对象也钉进内部 RAM
   （`lvgl_mem_set_internal_prefer` 目前只覆盖两个列表页）。
2. **异步 PPA 流水线**：收益面本来就窄（§6.2），不建议优先做。
3. **换渲染架构**：全屏 PSRAM framebuffer + PPA 合成，40MHz QSPI 下全屏
   上限约 47fps，与本 UI 的局部刷新策略冲突。

## 9. 复现实验的最小步骤

```powershell
# 1) 确认 sdkconfig.defaults 有：CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE=64 / LV_DRAW_BUF_ALIGN=64
# 2) Kconfig：CONFIG_UI_PPA_ACCEL=y（默认）
#    要看混合流量就临时打开 CONFIG_UI_PPA_STATS / CONFIG_UI_PPA_BENCH
#    ⚠ 统计日志本身会拖慢 UI（§6.3-1），测性能前务必关掉
idfc build
idfc flash
idfc monitor --wait-for "alpha"
```

⚠ 复现时重点看：

- 是否还有 `lv_draw_buf_init: Data is not aligned`（有就检查对应图像的
  `data` 是否 64B 对齐，不是只有 lvgl_mem 一处）；
- 滑动时是否有固定间距横黑线（内置 draw unit 的 `block_offset` 问题）；
- 统计里 `565半透明` 是否为 0 —— 如果是 0，PPA 混合就没有用武之地。
