# ESP32-S31 PPA 加速实验记录（LVGL 9.4）

> 2026-11，S31（FakePodNano Pro V1）移植后尝试给 LVGL 接 PPA。
> 结论先行：**PPA 的纯色 FILL 在本 UI 上是负收益**，已默认关闭；
> 但过程中修掉的两个真实 bug（绘制缓冲未 64B 对齐、LVGL 内置 PPA FILL
> 忽略局部缓冲偏移）必须保留，否则以后接 blend/SRM 还会踩。

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

## 5. 为什么没有直接照搬官方 “blend + fill handle”

官方/`esp_lvgl_adapter` 的 v9 handler 是**自定义 SW blend 回调**：
`lv_draw_sw_register_blend_handler()` + 阻塞 PPA blend/fill，
对 RGB565 图像混合、不透明填充做硬件加速，其他（A8 文本、复杂 mask、
小面积）回退 CPU。

本 UI 的 CPU 热点是**文本**（FreeType 光栅化 + A8 字形混合）和对象树布局：

- PPA 的 A8 前景混合需要**每个 glyph 一次 PPA 调用**，15k px 的固定开销
  ~0.3ms，几十个 glyph/帧会直接吃掉收益；
- 纯色填充 CPU 已经更快；
- 剩余可加速的只有大尺寸 alpha 图像（封面淡入、全屏遮罩），在本 UI 里
  频次很低，救不了列表滚动。

因此**没有把这条路径接进来**，避免“看起来用了 PPA、实际更卡”。

## 6. 当前代码状态

- `sdkconfig.defaults`：`CONFIG_LV_USE_PPA` **默认关闭**（保留 64B 对齐两项）。
- `main/CMakeLists.txt`：PPA 组件接线保留（`if(CONFIG_LV_USE_PPA)`），
  打开开关即可复现实验。
- `main/core/ui/lvgl_mem.c`：所有分配 64B 对齐（**保留**，与 PPA 开关无关，
  修的是真实的 LVGL 绘制缓冲对齐问题）。
- `managed_components/lvgl__lvgl/src/draw/espressif/ppa/`：
  实验期直接改过 `lv_draw_ppa.c` / `lv_draw_ppa_fill.c`（偏移、阻塞、
  buffer_size 对齐、面积门槛、burst 32）。**尚未转成 `patches/` 正式补丁**，
  组件管理器重新拉取会覆盖；下次启用 PPA 前应先把这些修改整理成
  `patches/lvgl_ppa_partial/` 并由 `main/CMakeLists.txt` 幂等回写。

## 7. 后续可选方向

1. **官方 adapter 式 blend handler（推荐先做小范围验证）**
   - 只对 `src_color_format == RGB565`、无 mask、面积大的图像混合走 PPA；
   - 文本/填充全部回退 CPU；
   - 用 OtoClock 的 `lvgl_ppa_accel_v9.c` 做参考（它有完整的
     `block_offset`、C2M/M2C cache 同步、面积/对齐门槛）。
   - 预期收益：封面切换、全屏 alpha 遮罩；列表滚动不变。

2. **异步 PPA 流水线（收益最大、风险最高）**
   - 让 PPA FILL 非阻塞，与 CPU 文本渲染重叠；
   - 需要解决同一绘制缓冲的并发写依赖，可能要分图层/双缓冲；
   - LVGL 9.4 的 `LV_PPA_NONBLOCKING_OPS` 目前是 `#error` 实验状态。

3. **换渲染架构**
   - 全屏 PSRAM framebuffer + PPA 合成，QSPI 40MHz 下全屏刷新上限约 47fps
     （423KB/21ms），不一定适合本 UI 的局部刷新策略。

## 8. 复现实验的最小步骤

```powershell
# 1) sdkconfig.defaults 打开：
#    CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE=64
#    CONFIG_LV_DRAW_BUF_ALIGN=64
#    CONFIG_LV_USE_PPA=y
#    （图像路径 CONFIG_LV_USE_PPA_IMG 先不要开）
# 2) 按 §2 的 CMake 接线 & §3.2 的 fill 修正
idfc build
idfc flash
idfc monitor --grep "not aligned|PPA"
```

⚠ 复现时重点看：

- 启动/滑动是否还有 `lv_draw_buf_init: Data is not aligned`（有就检查
  `lvgl_mem.c` 是否真的 64B 对齐）；
- 滑动时是否有固定间距横黑线（有就检查 `block_offset` / `pic_w/h`）；
- 是否出现 PPA 与 QSPI 总线争用导致的 `wait_for_flushing` 卡死/重启
  （实验中曾出现一次，未完全定位；可能与 PPA 阻塞期间刷屏 DMA 并发有关）。
