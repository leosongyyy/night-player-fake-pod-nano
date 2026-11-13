# 封面改用硬件 JPEG 解码（ESP32-S31 JPEG codec，2026-11）

> 触发：用户提出「esp32s31 好像有硬件 jpeg 解码器，或许可以将它接在显示封面的地方」。
> 结论：**有，而且接上以后封面解码从百毫秒级降到 20ms 以内**，真机确认画面正确。
> 本文记录接线方式、必须处理的三个坑，以及实测数据。

## 1. 硬件能力核实

`components/soc/esp32s31/include/soc/soc_caps.h`：

```
#define SOC_DMA2D_SUPPORTED              1
#define SOC_PPA_SUPPORTED                1
#define SOC_JPEG_CODEC_SUPPORTED         1
#define SOC_JPEG_DECODE_SUPPORTED        (1)
#define SOC_JPEG_ENCODE_SUPPORTED        (1)
```

⇒ S31 **同时具备** PPA（已在用，LVGL 绘制加速）和 JPEG 编解码硬件。
驱动是 IDF 自带的 `esp_driver_jpeg`（`driver/jpeg_decode.h`），底层走 **DMA2D**
（`dma2d_acquire_pool`，与 PPA 各自的 client 注册互不干扰——PPA 不走 dma2d）。

## 2. 接线方式（`core/image/cover_jpeg.c`）

对外接口一个字没改，`cover_jpeg_decode()` 内部变成两条路径：

| 路径 | 实现 | 何时用 |
|---|---|---|
| 硬件（默认） | `esp_driver_jpeg`：`jpeg_decoder_get_info` → `jpeg_alloc_decoder_mem` → `jpeg_decoder_process` | `CONFIG_UI_COVER_HW_JPEG=y` 且引擎创建成功 |
| 软件（兜底） | `espressif/esp_jpeg`（tjpgd） | 上面任一环节失败，或 Kconfig 关掉硬件 |

- 引擎**懒创建一次、常驻**（`s_engine` + `s_engine_tried`）：第一次解码时
  `jpeg_new_decoder_engine()`，之后复用。只占几 KB 内部 RAM（codec handle +
  两个 DMA2D 描述符 + 事件队列）。创建失败只试一次，之后整机走软解，避免每首歌
  都重试并刷日志。
- 只在 `cover_load` 任务（Core1）里调用，天然串行；驱动内部还有 `codec_mutex`。
- 单次解码失败会**退回软解并仍返回成功**——封面不能因为硬件路径出问题而消失。
- Kconfig 开关 `CONFIG_UI_COVER_HW_JPEG`（`main/Kconfig.projbuild`，默认 y，
  `depends on SOC_JPEG_DECODE_SUPPORTED`），关掉即可 A/B 对比或回退。
- `esp_driver_jpeg` 已加入根 `CMakeLists.txt` 的 `COMPONENTS` 白名单。

## 3. 三个必须处理的坑

### 3.1 输出缓冲要按 **MCU 对齐尺寸** 分配

硬件按最小编码单元整块输出：YUV420 的宽高都向上对齐到 16、YUV422 只有宽对齐到
16（`jpeg_parse_marker.c`：`mcux = hi[0]*8`、`mcuy = vi[0]*8`）。

- 460×460 的 YUV420 封面 → 实际输出 **464×464**（430 592 B，不是 423 200 B）。
- `jpeg_decoder_process` 会用对齐后的尺寸校验：`real_size = process_h*process_v*bpp/8`，
  缓冲给小了直接 `ESP_ERR_INVALID_ARG`。

### 3.2 输出缓冲必须用 `jpeg_alloc_decoder_mem()` 分配

驱动会校验"地址按 cache 行对齐 **且** 大小按 cache 行对齐"（`_check_buffer_alignment`），
不满足直接报错。`jpeg_alloc_decoder_mem(size, &mem_cfg, &allocated_size)` 同时满足
两条，并把 `size` 向上对齐后通过 `allocated_size` 返回——**必须原样传回**
`jpeg_decoder_process` 的 `outbuf_size`。它内部是 `heap_caps_aligned_calloc`，
所以用普通 `free()` 释放（与 `cover_jpeg_free` 一致），可以一直留作 LVGL 图片数据。

### 3.3 对齐宽度 ≠ LVGL 要的紧凑行宽 —— 必须**压实每行**

这是最容易踩的一个：**LVGL 的 PPA 图片绘制单元完全不看 `header.stride`**。
见 `managed_components/lvgl__lvgl/src/draw/espressif/ppa/lv_draw_ppa_img.c`：

```c
.in_fg = {
    .buffer = (void *)img_dsc->data,
    .pic_w  = width,        /* = 绘制区域宽，PPA 据此**隐式**认为行距就是 pic_w */
    .block_w = width,
    ...
}
```

所以若把 464 宽的缓冲直接配 `stride = 464*2` 交给 LVGL，PPA 会按 460 的行距去读，
画面会斜切/错行。两条出路：

- 关掉 `LV_USE_PPA_IMG`（损失绘制加速）——不可接受；
- **就地把每行往上搬成紧凑布局**——采用这条。

压实循环的顺序不能随便写：

```c
for (y = 1; y < h; y++)                       /* 必须从第 1 行往后 */
    memmove(buf + y * row, buf + y * pitch, row);
```

- 正向安全：第 y 行的目的地 `[y*row, (y+1)*row)` 与第 y+1 行的源
  `[(y+1)*pitch, …)` 不相交（`pitch > row`），而 `memmove` 能处理"目的 < 源"的自重叠；
- 反向（从最后一行往前）会**踩掉还没搬的源数据**：源行 y 的尾部会落进
  已搬完区域（最多 `y*(pitch-row)` 字节），实测会错行。

压完之后 `stride = 宽*2`、`data_size = 宽*高*2`，与软件路径**完全一致**，界面代码零改动。

### 3.4 颜色通道顺序

`jpeg_decode_cfg_t`：

- `.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR` = **小端** RGB565，正好是 LVGL 9
  渲染缓冲的原生字节序（等价于软解那套 `swap_color_bytes = 0`）。
  取 `RGB`（大端）会让红蓝整体错位。
- `.conv_std = JPEG_YUV_RGB_CONV_STD_BT601`：JFIF/JPEG 的标准矩阵就是 BT.601。

## 4. 实测（debug 构建，COM3）

```
I (1863) cover_jpeg: hw jpeg decoder ready (ESP32-S31 JPEG codec)
I (1874) cover_jpeg: pixel(hw) 430592 B -> PSRAM          ← 464x464x2
I (1884) cover_jpeg: decoded 460x460 (423200 B) in 21 ms [hw]
```

- **21 ms** 是**第一次**解码（含引擎创建 + 分配 + 解码 + 压实），后续切歌更快；
  软件解码同尺寸是 **100~300 ms** 量级（`cover_jpeg.h` 里的历史值）。
  用户实测："图片秒切，比以前快很多"。
- PSRAM 占用从 423 200 B（软解）变为 430 592 B（硬解的对齐缓冲），多 7 KB，
  压实后实际使用仍是 423 200 B。
- **输出正确性对账**（同一张 JPEG 两条路径各解一次，对比稀疏采样签名）：

| 路径 | avgR | avgG | avgB | 中心像素 | 左上角 |
|---|---|---|---|---|---|
| 硬件 | 28 | 55 | 25 | `0xffff` | `0xc638` |
| 软件 | 27 | 52 | 24 | `0xffdd` | `0xbdd7` |

差 1~3 个 5/6bit 级（两条路径的 YUV→RGB 舍入不同，硬件把接近饱和的像素直接夹到
纯白）。通道顺序、行列位置都一致——**没有 R/B 对调、没有错行**。用户另做了目视确认。

## 5. 排查入口

- `cover_jpeg.c` 里保留了一个 `cover_log_signature()`（`ESP_LOGD`），换芯片/改解码
  配置时把日志级别开到 debug 就能对账两条路径。
- 想回退：`CONFIG_UI_COVER_HW_JPEG=n`（`idfc build` 会重配），一行 Kconfig。
- 若硬件解码报错，日志会是 `hw jpeg decode failed: …，本次退回软件解码` +
  紧随的 `decoded … [sw]`，封面照常显示。
