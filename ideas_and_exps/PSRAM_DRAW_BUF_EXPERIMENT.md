# LVGL 绘制缓冲放 PSRAM 实验（2026-11-15，结论：不保留）

> 一次性实验记录，代码改动**已全部回滚**（Kconfig 开关、`lvgl_port.c` 分配、
> `lvgl_ppa_accel.c` 的外部 RAM 输出限制、debug 默认值均恢复原状）。
> 结论：**能跑，但帧率掉回 S3 时代**；省下的约 83KB 内部 RAM 足以让 WiFi 与蓝牙
> 在同一固件里共存，但项目选择保留内部 RAM 绘制缓冲 + 双固件对时方案
> （见 `DUAL_APP_TIME_SYNC.md`）。

## 1. 动机

S31 的内部 RAM（与 IRAM 同一块 SRAM）是稀缺资源：

- WiFi 协议栈（esp_wifi/lwip/wpa_supplicant）常驻约 **27KB 静态 + 65KB 运行时峰值**
  （`DUAL_APP_TIME_SYNC.md`）。
- 正因如此，WiFi 对时被拆成独立固件（`ota_0`），播放器不编 WiFi 协议栈。
- 播放器里最大的内部 DMA 消费者是 **LVGL 双绘制缓冲**：`460×46×2B ≈ 41KB/块`，
  双缓冲 **2×42368B ≈ 83KB**。

如果这 83KB 能挪进 16MB PSRAM，播放器就能同时容纳 WiFi + 蓝牙，取消双固件方案。
本次实验就是验证"挪过去之后还能不能用、帧率掉多少"。

## 2. 可行性判断（实验前核对）

| 环节 | 事实 |
|---|---|
| S31 PSRAM 可 DMA | `soc_caps.h`：`SOC_PSRAM_DMA_CAPABLE 1` |
| PPA 读 PSRAM | `ppa_srm.c` 允许 in/out 在外部 RAM，并对 PSRAM 输入自动做 `esp_cache_msync(C2M)` |
| 默认 90° 路径 | LVGL 渲染到 PSRAM 绘制缓冲 → PPA SRM 转到内部 `s_tile` → SPI 发内部 RAM，**SPI 不直接碰 PSRAM** |

## 3. 当时的改法（已回滚，仅记录复现要点）

1. `main/Kconfig.projbuild` 加 `UI_DRAW_BUF_IN_PSRAM`（bool，默认 n）。
2. `main/core/ui/lvgl_port.c` 的分配处二选一：
   `MALLOC_CAP_DMA` ↔ `MALLOC_CAP_SPIRAM`（仍 `heap_caps_aligned_alloc(64, …)`）。
3. `main/core/ui/lvgl_ppa_accel.c` 放开"输出必须是内部 RAM"的检查，允许
   外部 RAM 输出，附加 `data_size >= PPA_ALIGN_UP(w*h*2)` 防越界。
4. `sdkconfig.defaults.debug` 里 `CONFIG_UI_DRAW_BUF_IN_PSRAM=y`（仅 debug 测）。

## 4. 实测数据（debug 构建，用户手动测试 + 串口 `MEM:` 采样）

### 4.1 帧率

| 场景 | 基线（内部 RAM） | PSRAM 绘制缓冲 |
|---|---|---|
| 0° 列表快速滚动 | **30 fps** | **23~31 fps（约 24）** |
| 90° 列表快速滚动 | 约 24 fps（旋转路径本来就降 20%） | **19~27 fps** |
| 静止 / 跑马灯 | 61~63 fps | 61~63 fps（不掉） |
| 开机到首帧 | 约 2.2s | 约 2.2s（无退化） |

结论：0° 直发路径约 **-20%**；90° 与基线 90° 相当或略低。这个水平已经回到
S3 时代（S3 列表快速滚动约 21fps，见 `BUILD_CONFIG.md` 时代的记录）。

### 4.2 PPA 旋转耗时（决定 90° 路径）

| 条件 | 单块旋转平均 | 等效吞吐 |
|---|---|---|
| 基线（内部 RAM 绘制缓冲） | 1107 µs | 15.2 Mpx/s |
| PSRAM 绘制缓冲 | **1352 µs** | 14.4 Mpx/s |
| PSRAM + 放开 PPA 混合到 PSRAM | **1209 µs** | 13.2 Mpx/s |

PSRAM 输入让 PPA 单块旋转慢约 22%；把 PPA 混合也放到 PSRAM 后略好，但仍在
1.2ms/块量级（每帧 8~10 块 → 10~12ms 的关键路径）。

### 4.3 内部 RAM 收益

- 启动日志：`draw buffers: V_RES/10 (42368 B/块, 双缓冲 84736 B, PSRAM 余量 16687804 B …)`。
- `MEMTRACE: 05-boot_splash` 对照（同一启动阶段、同样已分配绘制缓冲）：
  基线 `int 174111/108544` vs PSRAM 版 `int 258895/151552`，**差 84784 B ≈ 双缓冲**，
  即释放约 83KB 内部 RAM；DMA 余量同步增加，SD 卡读的 `allocate_dma_buf` 压力明显缓解。
- `MEM: iram=` 空闲在 PSRAM 版 0° 时约 100~101KB、90° 时约 142KB（基线约 90KB 量级）。
- 这是实验唯一的实质收益。

### 4.4 0° 下的隐藏代价：SPI bounce 缓冲

0°（未旋转直发）时，`MEM: iram=` 在滑动过程中会**瞬时下降 30~42KB**，
停止滑动后恢复（日志里 `-24068 / -29700 / -32260 / -33796 / -39428 / -42504`
的波动，对应一次 42368B 分配 + 堆开销）。90° 时 `iram` 是一条直线。

根因链：

1. `lvgl_port.c` 未旋转分支把 PSRAM 的 `px_map` 交给 `esp_lcd_panel_draw_bitmap()`。
2. `esp_lcd_panel_io_spi.c`：`color_in_psram=true`，但 `io_config.flags.psram_dma_direct`
   没开（`display.c` 只设了 `pclk_hz`）→ 不设 `SPI_TRANS_DMA_USE_PSRAM`。
3. `spi_master.c` 的 `setup_priv_desc()`：`use_psram=false`。
4. `spi_common.c` 的 `spicommon_dma_setup_priv_buffer()`：外部指针 + 不用 PSRAM 直发
   → `need_malloc=true` → 从 `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL` 分配一块与块同样大
   的缓冲并 `memcpy` 41KB，传输完成后 free。
5. 每帧 10 个 flush 块 = 每帧 10 次"分配 41KB → 拷贝 → 释放"，全在 LVGL 刷屏同步路径里。

**不是泄漏**，是瞬时峰值。但低内存场景（蓝牙 + 封面 + 重扫同时进行）下存在
分配失败/碎片风险。

PSRAM 直发在现有块几何下**无解**：IDF 要求 PSRAM tx 缓冲地址和长度都按
cache line（64B）对齐才能 msync，而 `460×46×2 = 42320` 不是 64 的倍数
（行宽 920B 本身也不是）。改绘制块几何或让 SPI 支持非对齐 msync 都不现实。

## 5. 为什么慢

1. **LVGL 渲染写 PSRAM**：缓冲 41KB > L1 cache，基本每帧都在 cache miss/回写；
   列表滑动时 `cpu0` 直接打满（98%）。
2. **PPA 读 PSRAM 慢**：单块旋转 1107→1352µs（+22%）。
3. **0° 的 bounce memcpy**：每帧额外 10×41KB 内部拷贝。
4. **PPA 混合受限**：`lvgl_ppa_accel.c` 原本硬性要求 `esp_ptr_internal(dst)`，
   PSRAM 缓冲下所有图像混合回退 CPU；放开后（本实验第 3 步）旋转略好，但帧率
   仍在 20~30fps——瓶颈是 1、2，不是混合。

## 6. 决策：不保留

- 收益（83KB 内部 RAM）真实且可观，足以覆盖 WiFi 的 27KB 静态 + 65KB 峰值，
  理论上可以让 WiFi + 蓝牙同固件。
- 代价（列表滚动 30→24fps、90° 19~27fps）是**日常操作可感知**的，
  等于把 S31 的 UI 流畅度退回 S3 时代。
- 项目已有更划算的方案：**双固件对时**（播放器不含 WiFi，需要时切 `ota_0`），
  既保住 61~63fps 的 UI，又不用把 WiFi 塞进播放器。
- 因此绘制缓冲继续留在内部 RAM（`MALLOC_CAP_DMA`）。

## 7. 如果将来要重新启用（兜底方案）

适用条件：内部 RAM 再次告急（例如必须 WiFi + BT 同固件、且不能再加 app 分区）。

- 必须保持 90° 旋转（PPA 中转），否则 0° 会走 SPI bounce：每帧 10 次
  41KB 分配/拷贝/释放，低内存时有失败风险。
- 若必须支持 0°，别用"PPA 0° 拷贝"（每块 1.2ms，更慢），改用**常驻一块内部
  bounce**（42KB 常驻、flush 里 memcpy）：净收益从 83KB 降到约 41KB，但没有碎片。
- PPA 混合输出到 PSRAM 时必须保证 `data_size >= PPA_ALIGN_UP(w*h*2)`，
  否则驱动 M2C msync 会越过缓冲尾部（LVGL 临时图层就不满足，需回退 CPU）。

## 8. 一句话结论

**能跑，但帧率掉回 S3 时代（0° 列表 30→24fps，90° 19~27fps）；省下的 83KB
足够 WiFi + 蓝牙共存，项目选择保留内部 RAM 绘制缓冲与双固件对时方案。**
