# IRAM / 内部 RAM 占用审计（LVGL 9.4 + TTF 迁移之后）

> 测量时间：2026-10-16；固件基线：`04b144a`（工作区干净）+ `build/cs43131.ttffont.debug`（debug / cs43131 / ttffont）。
> 硬件：ESP32-S3 成品播放器，8MB Octal PSRAM @80MHz，16MB Flash。
> 起因：LVGL 8.4 → 9.4、字体换成 TTF 之后，LVGL 对象已全部搬到 PSRAM，但"日常可用的 IRAM"看起来没有改善。

---

## 0. 结论摘要（先看这里）

1. **术语先说清**：`mem_monitor` 打印的 `iram=` 并不是指令 RAM，而是
   `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`，即**内部 DRAM 堆的空闲字节**
   （`main/core/utils/performance_monitor.c:123,130`）。本项目口头说的"IRAM 剩余 xx KB"一直是这个数。
2. **在 ESP32-S3 上，IRAM 与内部 DRAM 是同一块 SRAM 的两个别名，所以二者是零和的**：
   `.iram0.text` 里超出前 16 KB（`0x40374000~0x40378000`）的**每一字节，都直接从内部 DRAM 堆里扣掉**
   （机制与证据见 §1）。**"把 LVGL 对象搬去 PSRAM"省的是 DRAM 堆，而"LVGL 代码进 IRAM"吃的也是 DRAM 堆**——
   两者在同一个池子里对冲，这就是"搬走了却没改善"的根源。
3. **当前 IRAM 代码共 166.4 KB，其中 LVGL 独占 82.8 KB（50%）**，几乎全部是软件混合（blend）函数，
   由项目自己打开的 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`（`sdkconfig.defaults:86`）决定。
   其余才是 FreeRTOS/SPI flash/heap 这些"必须常驻"的部分（见 §2.1、§2.2）。
4. **实测（同一台设备、同一天、只改这一行配置）**：把它关掉，内部 RAM 稳态空闲
   **23.5 KB → 106.7 KB（+83.2 KB）**，开机完成时最大连续块 **3.5 KB → 31.7 KB**。
   map 也逐字节对上：`.iram0.text −82.8 KB` = `.dram0.dummy −82.8 KB` = 堆起点前移 84 736 B（见 §4）。
5. **升级后"没改善"的账**：v9 把 LVGL 对象搬进 PSRAM，确实让 `app_ui_create` 少吃了 ~66 KB；
   但被两笔新开销吃掉了 —— ① LVGL 9 的 blend 多格式实现全在 IRAM（本项 82.8 KB）；
   ② TTF 迁移带来的栈增量共 ~26 KB（LVGL 12→20 KB、deep_worker 8→12 KB、主任务 8→16 KB）。
   净结果就是"看起来没动"，详见 §3。
6. ⚠ **比"还能不能再省"更紧迫的事实**：当前 baseline 开机完成后内部 RAM 只剩 **6.3 KB / 最大连续块 3.5 KB**、
   DMA 只剩 **2.7 KB / 连续 2.6 KB**（`17-perf_monitor`），**已经跌破项目自己定的 4 KB 底线**
   （SD 读 512 B + 内部任务栈 4 KB）。这比 `LVGL9_MIGRATION.md` §10.4 记录的 29.5 KB / 20 KB 差得多，
   建议把"恢复内部 RAM 余量"当成当前的首要目标，而不只是"能省则省"（见 §2.4、§7）。
7. **推荐动作**：一行配置关掉 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 即可拿回 83 KB（§5 P0）。
   代价比直觉小得多：`CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` 会把这些代码在启动时拷进 PSRAM 执行
   （IDF Kconfig 原文："PSRAM access speed is faster than Flash access. So the performance is better."），
   不是丢给慢速 flash（见 §5 P0 的"为什么代价小"）。
8. ✅ **2026-10-16 已落地**：用户在真机上实测"关掉 FAST_MEM 只掉 1 帧"，于是
   `sdkconfig.defaults` 里关掉 `FAST_MEM_USE_IRAM`、裁掉 6 种用不到的颜色格式、关掉 LVGL examples/demos 编译。
   结果：内部 RAM 稳态 **23.5 KB → 106.3 KB**、开机完成最大连续块 3.5 → 31.7 KB、DMA 2.7 → 83.4 KB；
   全量构建步数 **1941 → 1691**；固件体积反而 **−62 KB**。详见 §9。

---

## 1. 机制：为什么"IRAM 代码"会吃掉"内部 RAM 堆"

### 1.1 IDF 的分区表把 IRAM 与 DRAM 配成别名

`components/heap/port/esp32s3/memory_layout.c:84-95`（IDF 5.5.5）：

```c
const soc_memory_region_t soc_memory_regions[] = {
#if CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB && !defined(CONFIG_ESP_SYSTEM_MEMPROT_FEATURE)
    { 0x40374000,           0x4000,   SOC_MEMORY_TYPE_IRAM,   0,          false}, //Level 1, IRAM
#endif
    { 0x3FC88000,           0x8000,   SOC_MEMORY_TYPE_DIRAM,  0x40378000, false}, //Level 2, IDRAM
    { 0x3FC90000,           0x10000,  SOC_MEMORY_TYPE_DIRAM,  0x40380000, false}, //Level 3, IDRAM
    ...
```

- `0x40374000 + 0x4000`（16 KB）是本芯片**唯一的"纯 IRAM"堆区**，只支持 `MALLOC_CAP_32BIT|MALLOC_CAP_EXEC`；
- 从 `0x40378000` 往上，IRAM 别名与 DRAM `0x3FC88000` 往上**指向同一块物理 SRAM**（表中每行第 4 列就是配对地址）。
  也就是说 IRAM 里放的每一字节代码，DRAM 视图里就少一字节可用空间。

### 1.2 链接产物里的直接证据

| 段 | baseline 地址/大小 |
|---|---|
| `.iram0.vectors` | `0x40374000`，0x404 |
| `.iram0.text` | `0x40374404`，**0x299c7 = 170 439 B** |
| `.dram0.dummy` | `0x3fc88000`，**0x25e00 = 155 136 B** |
| `.dram0.data` | `0x3fcade00`，0x406c |
| `.dram0.bss` | `0x3fcb1e70`，0xc968 |

`.iram0.text` 结束于 `0x40374404 + 0x299c7 = 0x4039DDCB`（对齐到 `0x4039DE00`），
`0x4039DE00 − 0x40378000 = 0x25E00` —— **正好等于 `.dram0.dummy` 的大小**。
`.dram0.dummy` 就是链接器为"IRAM 占用的那部分 SRAM"在 DRAM 视图里打的占位（NOLOAD），
它撑到哪，堆就从哪开始。

### 1.3 实验验证（§4 的 A/B 就是这条机制的实测）

只改 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`：

| 项 | baseline | 实验 | 变化 |
|---|---|---|---|
| `.iram0.text` | 170 439 | 85 623 | **−84 816（−82.8 KB）** |
| `.dram0.dummy` | 155 136 | 70 400 | **−84 736（−82.8 KB）** |
| `.dram0.data` @ | `0x3fcade00` | `0x3fc99300` | 前移 84 736 |
| `.dram0.bss` @ | `0x3fcb1e70` | `0x3fc9d370` | 前移 84 736 |
| `.flash.text` | 877 372 | 959 652 | +82 280 |

堆起点随之前移 84 736 B，实测每一阶段的内部 RAM 空闲都恰好 `+84 736 ~ +85 928`（§4.2）。
**结论：IRAM 里省下的代码字节，1:1 变成可用内部 RAM。**

---

## 2. 当前账本

### 2.1 静态 IRAM 代码 166.4 KB（按组件）

来源：对 `build/cs43131.ttffont.debug/FakePodNanoNightPlayer.map` 中地址落在
`0x40374000~0x4039DE00` 的输入段按源归档聚合（169 883 B 有归属，其余为对齐填充）。

| 组件 | KB | 说明 |
|---|---|---|
| **lvgl** | **82.8** | 见 §2.2，**唯一的"想省就能省"的大项** |
| freertos | 14.9 | `CONFIG_FREERTOS_IN_IRAM=y`：调度器/flash 操作期间仍需运行，必需 |
| esp_hw_support | 12.6 | 关缓存、spi_bus_lock、rtc_clk、sleep 等，绝大部分必需 |
| hal | 11.3 | `spi_hal_iram` / `spi_flash_hal_iram`，flash 操作必需 |
| spi_flash | 11.0 | `SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM`，必需 |
| heap | 6.2 | `tlsf.c`，关缓存期间要能分配内存，必需 |
| esp_driver_spi | 4.6 | `SPI_MASTER_ISR_IN_IRAM` |
| esp_ringbuf | 4.2 | ISR 侧 ringbuf 路径 |
| esp_system | 3.7 | 启动/关缓存/panic 相关 |
| esp_psram | 2.6 | 八线 PSRAM 初始化 |
| xtensa | 2.4 | 中断向量/上下文，必需 |
| newlib | 2.1 | `LIBC_MISC_IN_IRAM` + `LIBC_LOCKS_PLACE_IN_IRAM` |
| esp_mm / esp_timer / driver / 其它 | ~10 | 零散 |

> 注：`CONFIG_ESP_WIFI_ENABLED=y`、`ESP_PHY_IRAM_OPT=y`、`ESP_WIFI_IRAM_OPT=y` 虽然都开着，
> 但固件里**没有任何 WiFi/PHY 的目标文件**（组件未被任何一方引用，链接器没拉进来），
> 所以它们当前**不是** IRAM 消耗项，不必为它们改动。若将来引入 WiFi，这一块会立刻显著变大。

### 2.2 LVGL 占的 82.8 KB：全是 blend

| 文件 | KB | 是否当前 UI 真的需要 |
|---|---|---|
| `lv_draw_sw_blend_to_argb8888.c` | 17.61 | ✅ 需要（圆角裁剪 / slider 的 ARGB8888 图层，见 §5 P1） |
| `lv_draw_sw_blend_to_al88.c` | 12.09 | ❌ 无 AL88 图源 |
| `lv_draw_sw_blend_to_argb8888_premultiplied.c` | 11.62 | ❌ 纯死代码（无 ThorVG/Lottie，SW 核心从不生产该格式） |
| `lv_draw_sw_blend_to_rgb565.c` | 10.91 | ✅ 需要（屏幕就是 RGB565） |
| `lv_draw_sw_blend_to_rgb888.c` | 10.00 | ⚠ 仅水平/三角渐变需要，当前无渐变 |
| `lv_draw_sw_blend_to_i1.c` | 8.86 | ❌ 无 I1 图源 |
| `lv_draw_sw_blend_to_l8.c` | 6.39 | ❌ 无 L8 图源 |
| `lv_draw_sw_mask.c` | 2.86 | ✅ 需要（圆角/异形遮罩） |
| `lv_draw_sw_letter.c` | 0.52 | ✅ 需要（字形绘制） |
| `lv_draw_sw_box_shadow.c` | 0.50 | ✅ 需要（阴影） |
| `lv_draw_sw_grad.c` 等 8 个 | ~1.5 | 视功能而定 |

**为什么 v9 比 v8.4 涨了**：v9 把软件混合按"目标/源颜色格式"拆成独立文件，
而 `LV_ATTRIBUTE_FAST_MEM`（→ `IRAM_ATTR`）把这些函数整份推进 IRAM；
v8.4 的格式支持集中在少数文件里，量级小得多。
10 个 `CONFIG_LV_DRAW_SW_SUPPORT_*` 目前**全部为 y**（`sdkconfig:2436-2445`）。

### 2.3 静态内部 DRAM 68 KB（`.data` 16 492 + `.bss` 51 560）

按组件：`main` 47.0、`hal` 6.5、`freertos` 3.8、`spi_flash` 3.0、`esp_hw_support` 1.7、其余 ~6。

`main` 里最大的是**音频路径的静态缓冲，合计 40.2 KB**（nm 符号级实测）：

| 符号 | 字节 | 位置 | 可否搬 PSRAM |
|---|---|---|---|
| `read_buf`（MP3 文件读缓冲） | 16 384 | `mp3_player.c:51` | ✅ 纯 CPU 访问，最适合 |
| `silence_flush`（I2S 预加载静音） | 11 520 | `audio.c:51` | ✅ 只作 `i2s_channel_preload_data` 的源 |
| `out_buf` / `stereo_buf` | 4 608 ×2 | `mp3_player.c:52-53` | ✅ 解码输出，CPU 访问 |
| `pcm_buf`（写 I2S 的最终缓冲） | 4 096 | `audio.c:54` | ✅ 只作 `i2s_channel_write` 的源 |

其它较大的：`port_IntStack` 3 072（Xtensa 中断栈）、`s_files` 1 536（SD/VFS）、
`s_rt`/`s_eq_rt` 各 732（EQ 的 DSP 运行时）、`lv_global` 492。

> `i2s_channel_write()` / `i2s_channel_preload_data()` 都是 **copy 语义**（IDF 会把数据拷进内部 DMA 描述符缓冲），
> 所以这些缓冲放在 PSRAM 不违反 DMA 可达性；代价只是每次多一次 PSRAM→内部的 memcpy。

### 2.4 运行时内部堆

IDF 启动日志（实验固件，baseline 时每块前移 84 736）：

```
heap_init: At 3FCA9CD8 len 0003FA38 (254 KiB): RAM      ← 主内部堆
heap_init: At 3FCE9710 len 00005724 ( 21 KiB): RAM      ← ROM 保留区，app_main 时回收
heap_init: At 3FCF0000 len 00008000 ( 32 KiB): DRAM     ← DRAM-only
heap_init: At 600FE038 len 00001FA0 (  7 KiB): RTCRAM
esp_psram: Reserving pool of 32K of internal memory for DMA/internal allocations
```

→ 内部堆总量约 **314 KiB（实验）/ 231 KiB（baseline）**，其中 32 KB 被
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 预留。

**baseline 实测（本次，设备正在播放 + 用户操作）**：

| 阶段 | int free | int largest | dma free | dma largest |
|---|---|---|---|---|
| `01-config_store` | 201 767 | 110 592 | 194 051 | 110 592 |
| `04-lvgl_port`（绘制缓冲已分配） | 76 907 | 31 744 | 69 191 | 31 744 |
| `ui:before app_ui_create` | 51 231 | 31 744 | 43 515 | 31 744 |
| `cover:req` | 12 259 | 7 680 | 4 543 | 2 560 |
| `ui:after app_ui_create` | 11 595 | 7 680 | 3 879 | 2 560 |
| **`17-perf_monitor`（开机完成）** | **6 327** | **3 584** | **2 711** | **2 560** |
| 稳态（`MEM:` 行） | **24 095** | — | — | — |

即：**开机后要过一会儿（临时缓冲释放、封面链路收尾）才回到 23.5 KB**，
而启动过程中一度只剩 6.3 KB / 连续 3.5 KB、DMA 连续 2.6 KB。

> ⚠ 对比 `LVGL9_MIGRATION.md` §10.4 记录的 2026-10-15 复测（开机完成 29.5 KB / 最大连续块 20 KB），
> 今天同口径实测只有 **6.3 KB / 3.5 KB**。差异幅度超出正常抖动，**建议单独复核一次**
> （最可能是这期间"字体延迟加载 + 扫描写入持锁冻结"等改动带来的运行时分配变化，也可能是测量时正在播放/加载封面）。
> 无论原因如何，**"最大连续块 3.5 KB"已经低于项目 4 KB 的底线**，值得优先处理。

**运行时大头（估算，供定向优化用）**：

| 项 | 量级 | 说明 |
|---|---|---|
| LVGL 绘制缓冲（2 × 460×46×2 B） | **84.6 KB** | `EXAMPLE_LVGL_BUF_HEIGHT = V_RES/10`，必须 DMA |
| 任务栈（内部 RAM 部分） | ~60 KB | LVGL 20 KB、deep_worker 12 KB、mp3_play 8 KB、mp3_out/player_ctrl/button/sleep_timer/boot_scan 各 4 KB… |
| 驱动 / 协议栈 / 其它 | ~25 KB | SDMMC、I2S、USB-serial-jtag、NVS、VFS 等 |
| LVGL 对象/样式/图层 | 0（在 PSRAM） | `core/ui/lvgl_mem.c` 接管 |

---

## 3. 为什么"LVGL 对象搬去 PSRAM 了，IRAM 却没改善"

用同一批插桩点把 v8.4 时代（`IRAM_DMA_AUDIT.md`，2026-09-10）与今天对齐：

| 时点 | v8.4（2026-09-10） | v9.4+TTF（今天） | 差 |
|---|---|---|---|
| `ui:before app_ui_create` | 136 955 / 63 488 | 51 231 / 31 744 | **−85.7 KB** |
| `ui:after app_ui_create` | 31 839 / 23 552 | 11 595 / 7 680 | −20.2 KB |
| `app_ui_create` 段消耗 | −105 116 | **−39 636** | 少吃了 **65.5 KB** ✅ |
| 开机完成 | 26 967 / 18 432 | 6 327 / 3 584 | −20.6 KB |
| 稳态 | 35 191 | 24 095 | −11.1 KB |

读法：

- **`app_ui_create` 这一段确实大幅变好了**（−105 KB → −39.6 KB，省 65.5 KB）——这是 LVGL 对象进 PSRAM 的功劳；
- **但"起点"同时低了 85.7 KB**，所以最终稳态反而比 v8.4 少 11 KB。

起点这 85.7 KB 的缺口，主要由两笔构成（**属推算，非直接实测**）：

1. **LVGL 9 的 IRAM 代码膨胀**：v9 的 blend 按格式拆分成多份并全部 `IRAM_ATTR`（今天实测 82.8 KB）。
   v8.4 时代同样开着 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM`，但格式实现集中、总量小得多。
   ⚠ 旧版本 map 未保留，**这个差值没有直接实测**，只能由 §4 的实验反推"v9 这一项就是 82.8 KB"。
2. **TTF 迁移的栈增量 ~26 KB**（`FontSystem.md` §5 有据）：
   LVGL 任务 12 → 20 KB（+8，后续又有一笔 6→12 的既有增量）、
   deep_worker 8 → 12 KB（+4）、主任务 8 → 16 KB（+8，仅 app_main 期间占用，但 `ui:before app_ui_create` 时刻确实在）。

一句话：**"搬走 65 KB"和"新增 ~85 KB"对冲了，"没改善"是真的，不是错觉。**

---

## 4. 实测 A/B 实验（本次做的那个实验）

### 4.1 方法

只改一行、其它全不动：

```diff
 sdkconfig.defaults:86
-CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y
+# CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM is not set
```

`idfc build --reconfigure` → `idfc flash`（COM12）→ `idfc monitor --grep MEM`。
验证完已**改回原值并重新构建/烧录**，工作区 `git status` 为空。

### 4.2 结果

map（静态）：

| 项 | baseline | 实验 | 变化 |
|---|---|---|---|
| `.iram0.text` | 170 439 | 85 623 | **−84 816（−82.8 KB）** |
| `.dram0.dummy` | 155 136 | 70 400 | **−84 736** |
| `.dram0.data` / `.bss` | 不变 / 不变 | 不变 / 不变 | 0 |
| `.flash.text` | 877 372 | 959 652 | +82 280 |
| app bin | 1 935 328 | 1 935 088 | −240 |

运行时（每阶段都 +84 736 ~ +85 928，与 §1.3 的机制完全一致）：

| 阶段 | baseline int free | 实验 int free | 变化 |
|---|---|---|---|
| `01-config_store` | 201 767 | 286 503 | +84 736 |
| `04-lvgl_port` | 76 907 | 161 643 | +84 736 |
| `ui:before app_ui_create` | 51 231 | 136 267 | +85 036 |
| `ui:after app_ui_create` | 11 595 | 97 523 | +85 928 |
| `17-perf_monitor` | 6 327 | 91 511 | +85 184 |
| **稳态 `MEM: iram=`** | **24 095** | **109 279** | **+85 184（+83.2 KB）** |
| `17-perf_monitor` 最大连续块 | 3 584 | 31 744 | +28 160 |
| `17-perf_monitor` DMA | 2 711 / 2 560 | 83 795 / — | +81 KB |

功能：完整启动日志 235 行，无 panic / Guru / abort，正常 `App started`、正常播放。
（monitor 开头那几行 `--- 0x...: 符号名` 是 esp-idf-monitor 对 ROM `load:0x...` 日志的自动解码，不是崩溃回溯。）

---

## 5. 优化方案（按性价比排序）

### P0 ✅ 推荐：关掉 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`

- **收益（已实测）**：内部 RAM 稳态 **+83.2 KB**（23.5 → 106.7 KB），
  开机完成最大连续块 3.5 → 31.7 KB，DMA 池 +81 KB。IRAM 代码 166.4 → 83.6 KB。
- **代价比直觉小**：因为这些代码不是丢给 flash —— 项目已开 `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y`
  （`sdkconfig.defaults:27`），启动时会把 `.flash.text` 整体拷到 PSRAM 并映射执行
  （`esp_psram.c:266-276` 的 `mmu_config_psram_text_segment`）。
  IDF 对该选项的说明原文：*"instructions in flash will be moved into PSRAM on startup…
  PSRAM access speed is faster than Flash access. So the performance is better."*
  所以实际变化是 **"IRAM 零等待执行" → "PSRAM 执行"**，而不是"flash 执行"。
  代价：PSRAM 里多占 ~82 KB 代码空间（当前 PSRAM 空闲 6.4 MB，无影响）。
- **性能风险与验证**：blend 是绘制热点，帧率可能下降。**必须实测**：
  亮屏下看 `CONFIG_LV_USE_PERF_MONITOR` 的 FPS、滚动长列表、进设置页、下拉控制中心；
  并对比 `MEM:` 行的 `cpu0=` 占用。若掉帧明显再考虑 P1。
- **功能风险**：`--gc-sections` 之外唯一要确认的是"关缓存窗口内不能执行 flash/PSRAM 代码"。
  本项目的 flash 擦写（重扫描、pldata 写入）全部经 `deep_worker` 串行执行且 LVGL 已被挂起/持锁冻结，
  期间不会走绘制；实测本次固件（关掉后）重扫描路径未做验证，**建议补一次"设置 → 重新扫描"回归**。
- **落地方式**：改 `sdkconfig.defaults:86` 一行；若要按构建类型区分，可放进
  `sdkconfig.defaults.release` 之外的公共文件（debug 同样受益）。

### P1 备选：保留 FAST_MEM 在 IRAM，但裁掉用不到的颜色格式

如果实测发现 P0 掉帧不可接受，就退回"只留真正需要的格式"：

- **必须保留**：`RGB565`（屏幕与所有缓冲的唯一目标格式）、
  `ARGB8888`（`clip_corner` 圆角帽 / `lv_slider` 圆角分支 / simple layer 都要它，
  项目下拉控制中心 `notification_panel.c` 与播放页进度条正踩这两条路）。
- **建议保留**：`RGB565A8`（将来任何旋转/缩放都要，缺了会**静默花屏**）、`RGB888`（水平/复杂渐变）。
- **可安全关闭**：`AL88`(12.09 KB)、`ARGB8888_PREMULTIPLIED`(11.62 KB)、
  `L8`(6.39 KB)、`I1`(8.86 KB)、`XRGB8888`、`A8`。
  → **IRAM 预计 −39 KB**（前四项之和），同时 flash 也变小。
- **判据（来自 LVGL 9.4 源码调研）**：没被支持的格式是**静默降级**——
  目标格式不支持时 `lv_draw_sw_blend.c:223-225/278-280` 直接 `default: break;`（无日志无断言）；
  源格式不支持时打 `LV_LOG_WARN("Not supported source color format")` 后不画；
  变换不支持时 `lv_draw_sw_transform.c:296-301` 警告 + 用**未初始化缓冲**混合 → 花屏。
- **验证**：`idfc monitor --grep "not enabled"` / `--grep "Not supported source"`，
  只要出现任一条就说明关错了；再刷一遍下拉控制中心、进度条、设置页、Toast、息屏/亮屏。

### P2：把音频静态缓冲 40.2 KB 搬到 PSRAM

`read_buf`(16 KB)、`silence_flush`(11.25 KB)、`out_buf`/`stereo_buf`(各 4.5 KB)、`pcm_buf`(4 KB)。
- 方式 A（最小改动）：开 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`，给这几个变量加 `EXT_RAM_BSS_ATTR`；
- 方式 B：改成 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` 动态分配（启动时一次）。
- 可行性：两个 I2S API 都是 copy 语义，不要求源缓冲 DMA 可达；`read_buf`/`out_buf`/`stereo_buf` 纯 CPU 访问。
- 风险：解码热路径多一次 PSRAM 访问，**需实测是否影响播放流畅度与 CPU 占用**（I2S 有 DMA 缓冲垫着，通常无感）。
- 保守做法：先只搬 `read_buf`（16 KB，收益最大、风险最低）。

### P3：LVGL 绘制缓冲 84.6 KB（最大单项，但也是性能关键）

`EXAMPLE_LVGL_BUF_HEIGHT = V_RES/10`（460/10 = 46 行 → 每块 42 320 B ×2 = 84.6 KB，必须 `MALLOC_CAP_DMA`）。
- 调成 `V_RES/20` 可回收约 42 KB，历史上实测约 20 fps（原 30 fps 左右）——**属于拿帧率换内存**；
- 也可评估把刷屏缓冲也放 PSRAM（ESP32-S3 的 GDMA 可访问 PSRAM），但 QSPI 刷屏带宽/延迟需实测，且关缓存窗口的约束更硬。
- 建议：**在 P0 拿到 83 KB 之后，这一项优先级下调**。

### P4：零碎项（合计 ~10 KB，风险/收益比一般，仅列举）

- `CONFIG_LIBC_MISC_IN_IRAM` / `LIBC_LOCKS_PLACE_IN_IRAM`（newlib 2.1 KB）；
- `CONFIG_ESP_DRIVER_SPI` 的 `SPI_MASTER_ISR_IN_IRAM`（4.6 KB）——只有当 QSPI LCD 的刷屏不会撞上关缓存窗口才安全；
- `CONFIG_ESP_RINGBUF`（4.2 KB）——被 I2S/其它 ISR 路径使用，风险较高，不建议动。

### 组合建议

| 场景 | 动作 | 预期内部 RAM 稳态 |
|---|---|---|
| 想要"一劳永逸" | P0 | ~106.7 KB（已实测） |
| 实测 P0 掉帧不可接受 | P1（裁格式） | ~62 KB（估） |
| 还想再抠 | P0 + P2 | ~120 KB（估） |

---

## 6. 附带发现（与 IRAM 无关，但顺手查到了）

### 6.1 LVGL 的 examples/demos 确实在编译，但**没有进固件**

- `CONFIG_LV_BUILD_EXAMPLES=y`（LVGL Kconfig 默认 `y if !LV_CONF_MINIMAL`，项目未关）→
  `env_support/cmake/esp.cmake:18-20` 递归 glob `examples/*.c`，**实测编译了 250 个目标文件**（3.6 MB 的 .obj，纯编译时间成本）；
- `CONFIG_LV_BUILD_DEMOS=y` 但 12 个 `LV_USE_DEMO_*` 全为 n → `demos/` 下**编译 0 个文件**（`demos/lv_demos.c` 不在任何 glob 里），所以 Demos 开关本身**不产生成本**；
- **关键验证**：对最终 ELF 做 `nm`，匹配 `lv_example|lv_demo` 的符号数 = **0**；map 里同样 0 处出现。
  即 `--gc-sections` 把未引用的 example 全部丢弃，**它们不占 flash、不占 RAM**。
- **结论**：关掉 `CONFIG_LV_BUILD_EXAMPLES` 的收益是**全量构建时间**（250 个文件），不是空间。
  建议关掉（零功能风险），但不要指望它能省内存。

### 6.2 ✅ 已实机排除（原"疑似 bug"不成立）：debug 版默认封面能正常显示

- 起因：静态调研发现 debug 版默认封面 `default_cover.c` 的 `.header.cf = LV_COLOR_FORMAT_I2`，
  而 grep `src/draw/sw/**` 只看得到 `LV_COLOR_FORMAT_I1`，于是曾推断
  "LVGL 9.4 软渲染不支持 I2 → 开机封面什么都不画"。
- **2026-10-16 用户在 debug 构建上实测：开机默认封面正常显示**，该推断不成立。
  最可能的解释：I2 源图在进入 `lv_draw_sw_blend()` 之前，已经被
  `lv_image_decoder` 的内置解码路径转换掉了（I2/I4/I8 只出现在
  `lv_bin_decoder.c` / `lv_canvas` / GPU 后端，而不在 `src/draw/sw/**`），
  静态 grep 漏掉的正是这一层。
- 结论：**无需任何处理**。留作"静态推断必须实机验证"的一个例子。


---

## 7. 建议的后续动作

1. **优先消除"余量跌破底线"**：当前 baseline 开机完成最大连续块只有 3.5 KB（< 4 KB 底线）、
   DMA 连续 2.6 KB。先做 P0，再复测这三项指标。
2. **补一次回归**（P0 之后必做）：重扫描 / 息屏-亮屏 / 长列表滚动 / 设置页 / 下拉控制中心 + FPS 对比。
3. **复核 §2.4 的口径差异**：为什么今天同口径比 `LVGL9_MIGRATION.md` §10.4 少 23 KB。
   排查手段：在 `main.c` 各阶段之间加一次 `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`，
   或在"正在播放/静置"两种状态下各读一次 `MEM:` 行。
4. **文档同步**：`AGENTS.md` 里"内部 RAM 空闲 ~29.5 KB（最大连续块 20 KB）"与今天的实测不符，
   建议在 P0 落地后按新数据更新；`LVGL9_MIGRATION.md` §10.4 的表格也值得加一行本次复测。

---

## 8. 本次审计的命令与产物

```powershell
# 静态：map 段级 + 符号级
#   .iram0.text / .dram0.dummy / heap 起点  ← FakePodNanoNightPlayer.map
#   组件与文件归属、最大静态符号              ← xtensa-esp32s3-elf-nm --print-size --size-sort
# 实验 A/B（已回滚）
idfc build --reconfigure     # 改 sdkconfig.defaults 的 FAST_MEM_USE_IRAM 后
idfc flash                   # COM12
idfc monitor --timeout 40000 --grep "MEM"
# 数值核对
idfc menuconfig desc CONFIG_LV_BUILD_EXAMPLES / CONFIG_LV_BUILD_DEMOS
```

- 备份：`tempdir/baseline_fastmem_iram.map`（baseline map）、`tempdir/exp_boot_full.log`（实验固件完整启动日志）
- 状态：`sdkconfig.defaults` 已恢复、`git status` 干净、设备已烧回 baseline（`FAST_MEM_USE_IRAM=y`）

---

## 9. 落地记录（2026-10-16，三项一起改）

### 9.1 改了什么（`sdkconfig.defaults` 的 LVGL 段）

| 项 | 改动 | 依据 |
|---|---|---|
| `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | y → **n** | §4 实测 +83.2 KB 内部 RAM，用户实测帧率只掉约 1 帧 |
| `LV_DRAW_SW_SUPPORT_XRGB8888` | y → **n** | 仅 `LV_COLOR_DEPTH==32` 才是原生格式 |
| `LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED` | y → **n** | 无 ThorVG/Lottie 时 SW 核心从不生产该格式（纯死代码） |
| `LV_DRAW_SW_SUPPORT_L8` | y → **n** | 无 L8 图源 |
| `LV_DRAW_SW_SUPPORT_AL88` | y → **n** | 无 AL88 图源 |
| `LV_DRAW_SW_SUPPORT_A8` | y → **n** | 只影响"被旋转缩放的 A8 图源"；文字字形是当蒙版用的，不走这个开关 |
| `LV_DRAW_SW_SUPPORT_I1` | y → **n** | 无 I1 图源 |
| `LV_BUILD_EXAMPLES` | y → **n** | 250 个文件编了也被 `--gc-sections` 全丢，纯编译时间成本 |
| `LV_BUILD_DEMOS` | y → **n** | 12 个 `LV_USE_DEMO_*` 全 n 时它本来就不编译任何文件，一并关掉只为清爽 |
| **保留** | `RGB565` / `ARGB8888` / `RGB565A8` / `RGB888` | 前两个必需；后两个是"以后加旋转/渐变不花屏"的保险（子代理源码调研结论） |

### 9.2 实测结果

map（对比 `baseline = FAST_MEM=y + 全格式`）：

| 项 | baseline | 关 FAST_MEM（全格式） | 再裁格式（最终） | 最终 vs baseline |
|---|---|---|---|---|
| `.iram0.text` | 170 439 | 85 623 | **85 623** | **−84 816（−82.8 KB）** |
| `.dram0.dummy` | 155 136 | 70 400 | **70 400** | **−84 736（−82.8 KB）** |
| `.flash.text` | 877 372 | 959 652 | **899 448** | +22 076 |
| `.flash.rodata` | 871 832 | — | **871 160** | −672 |
| `.dram0.bss` | 51 560 | 51 560 | **51 560** | 0 |
| app bin | 1 935 328 | 1 935 088 | **1 873 200** | **−62 128** |
| 全量构建步数 | 1941 | — | **1691** | **−250** |

> **裁格式的净收益 = 60.2 KB flash**（959 652 → 899 448）。
> ⚠ 它**不省 IRAM**：在 FAST_MEM 已关的前提下，这些混合函数本来就已经不在 IRAM 了。
> 若哪天想把 blend 放回 IRAM，裁掉的 4 个文件（AL88 12.09 + PREMULTIPLIED 11.62 +
> L8 6.39 + I1 8.86 KB）可再省约 **39 KB IRAM** —— 这是 §5 P1 的量化值。

运行时（烧录后实测）：

| 时点 | baseline | 最终 | 变化 |
|---|---|---|---|
| `01-config_store` | 201 767 | 286 503 | +84 736 |
| `04-lvgl_port` | 76 907 / 31 744 | 161 643 / 86 016 | +84 736 |
| `ui:after app_ui_create` | 11 595 / 7 680 | 96 859 / 31 744 | +85 264 |
| `17-perf_monitor`（开机完成） | 6 327 / 3 584，dma 2 711 | **91 075 / 31 744，dma 83 359** | +84 748 |
| 稳态 `MEM: iram=` | 24 095 | **108 843** | **+84 748（+82.8 KB）** |

功能：完整启动正常、`App started` 正常；`idfc monitor --grep "MEM|not enabled|Not supported"`
在整个开机过程中**没有捕获到任何格式相关警告**。

### 9.3 ⏳ 仍需实机目视验证的点位

串口无警告只能证明"开机路径没踩到被删格式"，运行时路径要靠眼睛：

1. **下拉控制中心**（`clip_corner` → ARGB8888 图层）：竖条填充、图标是否完整，拖动是否正常；
2. **正在播放页拖进度条**（`lv_slider` 的 `lv_bar` 圆角分支）；
3. **播放设置页（第 4 页）**、**全屏设置菜单**、**EQ 弹窗**（半透明遮罩）；
4. **Toast**（`lv_arc` 环形倒计时）与开发者选项触摸白点动画；
5. **息屏 → 亮屏**（`deep_worker` 的 `lv_refr_now` 全屏重刷会把所有图层路径再跑一遍，最容易暴露问题）；
6. 顺带看一眼 **debug 版开机默认封面**（§6.2 的 I2 疑问）。

**判定方法**：操作一轮后用 `idfc monitor --timeout 15000 --grep "not enabled|Not supported"`
复查串口 —— 只要捕获到任一条，就说明某个被关掉的格式仍在使用，把对应开关改回 y 即可（改回不涉及代码）。

### 9.4 当前工程状态

- `sdkconfig.defaults`：已包含 §9.1 的全部改动（**尚未提交**，随本报告一起待提交）；
- 设备 COM12：烧的是最终固件（开机 → 播放正常，串口无格式警告）；
- 备份产物：`tempdir/baseline_fastmem_iram.map`（baseline map）、
  `tempdir/exp_boot_full.log`（"关 FAST_MEM、全格式"固件的完整启动日志）、
  `tempdir/build_trim.log`、`tempdir/build_fastmem_trim.log`（两次裁剪构建日志）。

---

## 10. 追加对照实验：把 FAST_MEM 改回 y（保留裁剪格式）

> 起因：用户真机实测"关掉 FAST_MEM 只掉 1 帧"后，要求再对比
> "**blend 留在 IRAM + 裁掉用不到的格式**"这一组合能省多少。

### 10.1 关键发现：裁格式的 IRAM 收益远不止"少几个独立文件"

每个 `lv_draw_sw_blend_to_XXX.c`（"混合**到** XXX 目标"）内部都带一份
`switch (dsc->src_color_format)` 的**源格式分发表**，并被打上同一个
`LV_ATTRIBUTE_FAST_MEM`。所以关掉 AL88 / L8 / I1 这些**源**格式后，
连 RGB565 / ARGB8888 / RGB888 的文件本身也会缩水近一半：

| LVGL 文件 | y + 全格式 | y + 裁格式 |
|---|---|---|
| `lv_draw_sw_blend_to_argb8888.c` | 17.61 KB | **9.83 KB** |
| `lv_draw_sw_blend_to_rgb565.c` | 10.91 KB | **5.06 KB** |
| `lv_draw_sw_blend_to_rgb888.c` | 10.00 KB | **5.08 KB** |
| `lv_draw_sw_blend_to_al88.c` | 12.09 KB | **0** |
| `lv_draw_sw_blend_to_argb8888_premultiplied.c` | 11.62 KB | **0** |
| `lv_draw_sw_blend_to_l8.c` | 6.39 KB | **0** |
| `lv_draw_sw_blend_to_i1.c` | 8.86 KB | **0** |
| 其它（mask / letter / box_shadow / grad / …） | ~5.3 KB | ~5.3 KB |
| **LVGL IRAM 合计** | **82.8 KB** | **25.3 KB** |

### 10.2 map 与运行时（对比 baseline）

| 项 | baseline（y + 全格式） | y + 裁格式 | 变化 |
|---|---|---|---|
| `.iram0.text` | 170 439 | **111 523** | **−58 916（−57.5 KB）** |
| `.dram0.dummy` | 155 136 | **96 256** | **−58 880** |
| `.flash.text` | 877 372 | 874 788 | −2 584 |
| `.flash.rodata` | 871 832 | 871 160 | −672 |
| app bin | 1 935 328 | 1 874 432 | −60 896 |

| 时点 | baseline | y + 裁格式 | 变化 |
|---|---|---|---|
| `01-config_store` | 201 767 | 260 647 | +58 880 |
| `04-lvgl_port` | 76 907 / 31 744 | 135 787 / 61 440 | +58 880 |
| `ui:after app_ui_create` | 11 595 / 7 680 | 72 487 / 31 744 | +60 892 |
| `17-perf_monitor` | 6 327 / 3 584，dma 2 711 | **66 475 / 31 744，dma 58 759** | +60 148 |
| 稳态 `MEM: iram=` | 24 095 | **84 243** | **+60 148（+58.7 KB）** |

串口无任何格式警告，播放与 UI 正常。

### 10.3 四种配置总表（这是最终决策表）

| 方案 | IRAM 代码 | 内部 RAM 稳态 | 开机后最大连续块 | DMA（开机完成） | blend 执行位置 | 帧率 |
|---|---|---|---|---|---|---|
| ① **baseline**：FAST_MEM=y + 全格式 | 166.4 KB | 23.5 KB | 3.5 KB | 2.7 KB | IRAM | 11 fps |
| ② FAST_MEM=y + 裁格式（**已采用**） | 108.9 KB | **82.3 KB** | 31.7 KB | 58.8 KB | **IRAM** | **11 fps** |
| ③ FAST_MEM=n + 全格式 | 83.6 KB | 106.7 KB | 31.7 KB | 83.4 KB | PSRAM | 9 fps |
| ④ FAST_MEM=n + 裁格式 | 83.6 KB | **106.3 KB** | 31.7 KB | 83.4 KB | PSRAM | 9 fps |

（帧率来自 §12 的用户实测，均为列表快速滚动；①② 的对象都走 CUSTOM malloc 在 PSRAM，
③④ 也是——两者的差异只在 blend 的执行位置。）

读法：

- **②↔④ 的差别只有 24 KB 内部 RAM，换 1 帧**。两者都比 ① 好得多（+59 ~ +83 KB）；
- **裁格式在两条路线上都有价值**：路线 y 上省 57.5 KB IRAM，路线 n 上省 60.2 KB flash；
- **②是"性能零代价"的方案**：blend 仍全部在 IRAM 执行，内部 RAM 同时从 23.5 KB 涨到 82.3 KB，
  已经远高于项目 4 KB 底线（最大连续块 31.7 KB）；
- **④是"内存优先"的方案**：再拿 24 KB 内部 RAM，代价是 1 帧；
- ①（当前线上配置）无论从哪个角度看都应该淘汰。

> ⚠ 别被"裁格式在路线 n 上不省 IRAM"误导：那是因为 blend 已经不在 IRAM 了。
> 只要把 FAST_MEM 打开，同一批裁剪立刻变成 57.5 KB 的 IRAM 收益。

### 10.4 定稿

方案 ②（FAST_MEM=y + 裁格式）最终采用，已随本报告一起提交（`b043234`）：
`sdkconfig.defaults` 与设备 COM12 上跑的都是方案 ②。

---

## 11. 失败实验：把 LVGL 对象搬回内部 RAM（2026-10-16，两次都失败）

**结论先行：这条路不可行，`CONFIG_LV_USE_CUSTOM_MALLOC=y`（对象进 PSRAM）必须永久保留。**
两次实验都跑出了明确证据，不要再重试。

### 11.1 复测-1：方案②（blend 留 IRAM，内部 RAM 82.3 KB）

把 `LV_USE_CUSTOM_MALLOC` 换成 `LV_USE_CLIB_MALLOC`（LVGL 对象走标准 `malloc()` → 内部 RAM）：

```
ui:before app_ui_create  int  86,527 / 31,744 (338)
cover:req                int      23 /      0 (1677)   ← 内部 RAM 见底
abort() was called ... newlib/src/abort.c:38            ← 建锁失败 → 重启循环
```

### 11.2 复测-2：方案④（关 FAST_MEM，内部 RAM 106.3 KB —— 本项目最宽裕的配置）

| 阶段 | CUSTOM（方案④） | CLIB（复测-2） | 差 |
|---|---|---|---|
| `04-lvgl_port` | 161 643 / 86 016 | 152 691 / 77 824 | −8 952 |
| `ui:before app_ui_create` | 136 267 / 63 488 | 112 383 / 38 912 | **−23 884** |
| `cover:req` | 135 395 / 63 488 | 26 155 / 15 360 | −109 240 |
| `ui:after app_ui_create` | 97 523 / 31 744 | **7 735 / 7 680**，dma **19 / 0** | **−89 788** |
| `17-perf_monitor` | 91 511 / 31 744 | **715 / 176** | −90 796 |

失败链条（完整复现 `IRAM_DMA_AUDIT.md` §5 的经典故障）：

```
E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101   ← 连 512B 都分配不到
E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
W mp3_player: decode never started (...), treat as damaged
W player_ctrl: bad track (reason=3), auto skip next
I toast: toast (2000 ms): 歌曲数据损坏
E MEM: create mem_monitor task failed (internal free=55)
```

### 11.3 定量结论

- **LVGL 对象堆总量 ≈ 88~90 KB**
  （推算：CLIB 下 `ui:before → ui:after` 掉 104 648 B，其中 38 744 B 是与 malloc 策略无关的
  非对象开销——封面链路等，CUSTOM 下同样存在——故对象净增 ≈ 65.9 KB；再加上
  `lv_init` + 开机封面 + 列表页阶段就先吃掉的 23 884 B，合计 ≈ 89.8 KB。
  这与复测-1 里"86 527 B 可用仍然撞到 23 B"完全吻合。）
- **即使关掉 `FAST_MEM_USE_IRAM` 多出 83 KB 也不够**：要让 `ui:after app_ui_create` 还留下
  ~40 KB 安全余量，`04-lvgl_port` 时刻需要约 185 KB 空闲；而方案④ 那一刻只有 152.7 KB
  （84.6 KB 的 LVGL 绘制缓冲已经先拿走了）。
- **内部 RAM 的账算死了**：总量 ≈ 231 KB（方案④），扣掉静态段 68 KB、LVGL 绘制缓冲 84.6 KB、
  各任务栈 ≈60 KB 之后，不可能再挤出 90 KB。
- 副产品：这次失败精确复现了"**内部 RAM 见底 → SD 读 512 B 失败 → 误判坏曲 → 自动跳曲**"
  这条链，可以直接当作该故障的复现手册（见 `IRAM_DMA_AUDIT.md` §5）。

---

## 12. 帧率实测（用户提供，2026-10-16，均为"列表快速滚动"）

| 配置 | LVGL 对象位置 | 字体 | FAST_MEM_USE_IRAM | 帧率 |
|---|---|---|---|---|
| A | **内部 RAM** | TTF | y | **21 fps** |
| B | PSRAM（= 当前方案②） | TTF | y | **11 fps** |
| C | PSRAM | **BIN**（内嵌位图字库） | y | **12 fps** |
| D | PSRAM（= 方案④） | TTF | n | **9 fps** |

### 12.1 三个杠杆的排序（这是本次审计最重要的结论）

| 杠杆 | 效果 | 幅度 |
|---|---|---|
| **① LVGL 对象放哪**（内部 RAM vs PSRAM） | 21 → 11 fps | **−48%（接近腰斩）** |
| ② blend 函数放哪（`FAST_MEM_USE_IRAM`） | 11 → 9 fps | −18% |
| ③ 字体方案（TTF vs 内嵌位图） | 11 → 12 fps | +9%（BIN 略快） |

**对象位置的影响远大于 blend 位置**，这与"blend 是绘制热点"的直觉相反。合理解释：
列表滚动的主要开销不是像素混合，而是**对象树的遍历/布局/样式解析**——
每次滚动要读取大量 `lv_obj_t` 及其样式（本项目对象堆约 88 KB，见 §11.3），
而 ESP32-S3 只有 32 KB 数据 cache，88 KB 的对象在工作集里来回换入换出，
cache 命中率极低。相比之下 blend 的函数体是纯代码，16 KB 指令 cache 反而够用。

### 12.2 由此得到的行动含义

- **当前方案②（11 fps）的选择是对的**：它比方案④（9 fps）快 2 fps，
  而这 2 fps 的代价正是"blend 退到 PSRAM"；
- **下一个真正值得做的性能优化不是碰 blend，而是想办法让"最热的那部分对象"回到内部 RAM**。
  §11 已经证明**全部**对象放不进去（需要 ≈90 KB，而全部可用量最多也就 112 KB，
  还得给 SD/DMA/任务栈留余量）；但**只放最热的一部分**（例如 virtual_list 的行对象，
  几十个对象、约 10~20 KB）在原理上是可行的，收益可能相当可观。
  实现路径：在 `core/ui/lvgl_mem.c` 里把"一律优先 PSRAM"改成
  "**先吃一份内部 RAM 配额、用完再走 PSRAM**"，配额按内部 RAM 实时余量动态上限，
  避免再出现 §11 的见底事故；
- 字体维度（TTF 11 vs BIN 12）只差 1 fps，**不值得为性能换字体**，
  继续按 Flash 体积 / 缺字覆盖来选（当前 TTF 方案优势明显）。

---

## 13. 路径B 落地：把滚动热对象钉在内部 RAM（2026-10-16）

> ⚠ **2026-11 S31 更新：路径B 已默认关闭。** 迁到 ESP32-S31 并准备接 WiFi/BT 后复测：
> S31 上 `LVGL_INTERNAL_RESERVE`(40KB) 很早就触发，路径B 实际只钉得进约 20.8KB（不是
> 本文 S3 时代的 82.8KB），关掉后内部 RAM 稳态 +20.5KB、最大连续块 31.7→86KB，
> 列表快速滚动 24~36fps（真机确认可接受）。开关是 Kconfig
> `CONFIG_UI_LVGL_PIN_HOT_OBJECTS`（默认 n）。完整数据见
> **`IRAM_OBJECTS_PSRAM_S31.md`**。本节以下内容保留为 S3 时代的设计记录。

§12 指出"对象放内部 RAM"是最大的性能杠杆，但 §11 证明**全部**对象装不下。
本节是折中方案：**按创建时机圈定热对象**，只把滚动时反复访问的那部分钉在内部 RAM。

### 13.1 最终实现（5 处改动）

| 文件 | 改动 |
|---|---|
| `core/ui/lvgl_mem.h`（新增）/ `.c` | 窗口开关 `lvgl_mem_set_internal_prefer(bool)`：窗口内 LVGL 的所有分配优先内部 RAM。要点：**嵌套深度计数**（`main_ui` 与 `virtual_list_create` 两层窗口会同时存在）；余量不足自动回退 PSRAM；`lv_realloc_core` 让"已在内部 RAM"的对象在 realloc 后继续留在内部 RAM（否则改一次文本就被跨堆搬回 PSRAM，钉住白做）；`LVGL_INTERNAL_HARD_FLOOR`(12KB) 硬底线兜底；日志按 2s 节流（绑定窗口在滚动时高频开关） |
| `core/ui/virtual_list.c` | `virtual_list_create` **整个函数**（列表容器 / 状态结构 / 透明占位 / 行槽数组 / 每个行槽的 root+icon+text 及局部样式）纳入窗口；`vlist_bind_row` 再开一个小窗口，覆盖 `lv_label_set_text` 产生的**文本缓冲** |
| `ui/main_ui.c` | **只**把两个滚动列表页（`dir_list_create` + `playlist_page_create`）纳入窗口；正在播放页、播放设置页、状态栏、下拉控制中心照旧走 PSRAM |
| `ui/now_playing.c` / `.h` | 新增 `now_playing_enable_cover_load()`：首次封面加载先**挂起**，等 UI 全部创建完再放行（放行后切歌照常实时加载）；`cover_load_request()` 在此期间只记 pending、不通知任务 |
| `ui/app.c` | `app_ui_create` 末尾调用 `now_playing_enable_cover_load()` 放行封面 |

### 13.2 演进过程与最终数据（真机实测，列表快速滚动）

| 版本 | 钉住范围 | 钉住量 | 稳态 `iram=` | DMA 最大连续块 | 帧率 |
|---|---|---|---|---|---|
| 方案②（对照） | 无 | 0 | 84.2 KB | 58.8 KB | 11 fps |
| v1 | 只行槽对象 | ~14 KB | 73.7 KB | — | 慢速 **20+** / 快速 14 |
| v2 | +容器/占位/文本缓冲 | ~29.5 KB | 57.8 KB | — | 与 v1 持平 |
| v3 | 整个 `app_ui_create` | ~87.6 KB | 36.5 KB | 7.7 KB | 比 v2 **+4 帧** |
| 试验：封面推迟 + 整段窗口 | 同上，但消除了时序抖动 | 106.4 KB | 24.9 KB | ⚠ **208 B** | 与 v3 持平 |
| **最终：只两个列表页 + 封面推迟** | **两个滚动列表页** | **82.8 KB** | **38.8 KB** | **11.3 KB** | **与 v3 持平（采用）** |

三条结论：

1. **对象位置是最大杠杆**：v1 只钉 14 KB 就拿到慢速 20+ fps（对照 11 fps）；
   而快速滚动还需要更多，v3 把整段 UI 钉上才再 +4 帧。
2. **收益在 ~83 KB 处进入平台期**：把钉住量从 82.8 KB 加到 106.4 KB（多出正在播放页、
   播放设置页、状态栏、下拉控制中心），**帧率完全没有变化**——所以最终只保留两个
   滚动列表页（它们占了 UI 对象总量的约 **78%**：82.8 KB / 106.4 KB）。
3. **缩小范围救回了内存余量**：限定两个列表页后，DMA 最大连续块从 208 B 回到
   **11.3 KB**，内部 RAM 稳态从 24.9 KB 回到 **38.8 KB**，而帧率不变。

### 13.3 封面加载延迟放行：消除启动抖动

`now_playing_create()` 建好封面任务后会**立刻**请求加载当前歌曲封面，而那次加载
（`song_meta_load` + `esp_jpeg`）会与 UI 创建并行争抢内部 RAM，实测使钉住量每次开机
相差约 10 KB（`87,576 B` vs `76,987 B`），帧率随之波动。

修法：首次加载先挂起（`cover_load_request` 只记 pending），等 `app_ui_create()` 末尾
调用 `now_playing_enable_cover_load()` 再一次性补发。实测钉住量随之稳定在
**106,369 B**（同一窗口内不再有封面竞争）。

### 13.4 注意点（留给以后）

1. **`LVGL_INTERNAL_RESERVE`(40KB) 不是真实余量**：它按"申请 size 之和"卡，
   而每笔分配还有 ~16B 元数据/对齐开销，且 realloc 路径只受 12KB 硬底线约束。
   真实安全网是那条硬底线，不是这个常量。
2. **不要再无脑加大窗口**：§13.2 已证明 83 KB 之后没有收益，只会吃掉内存余量
   （整段窗口时 DMA 最大连续块一度只剩 208 B，低于 SD 读一次所需的 512 B）。
   若要继续提速，方向在别处（绘制管线、虚拟列表的重绑策略等），不是继续钉对象。
3. `now_playing_create()` 里的 `cover_load_request()` 调用仍然保留（记录 pending），
   只是不再立即唤醒任务；如果以后改动开机顺序，**记得同步 `app_ui_create` 末尾的
   放行调用**，否则封面永远不会加载。







