# XIP from PSRAM（代码/只读数据搬进 PSRAM 执行）：结论、坑与字体改造

> 2026-11-16 实验。**结论：值得保留**——列表滚动最差 40fps（内部绘制缓冲）/
> 34fps（PSRAM 绘制缓冲），此前是 24~30fps。
> 但**必须同时把 font 分区从 flash mmap 改成 PSRAM 副本**，否则开机几秒内
> 必然 Core1 access fault 重启（原因见 §3）。

## 1. 配置

`sdkconfig.defaults`：

```ini
CONFIG_SPIRAM_XIP_FROM_PSRAM=y   # 会 select FETCH_INSTRUCTIONS + RODATA + FLASH_LOAD_TO_PSRAM
```

- ⚠ IDF 6.1 里 `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA` 已是
  **无 prompt 的隐藏符号**（只能被 `SPIRAM_XIP_FROM_PSRAM` select），以前把它们
  直接写进 sdkconfig.defaults 是**死配置**（实测启动后 PSRAM 只用了 2.5KB，
  说明 .text/.rodata 一直没搬）。
- 启动日志会打：
  ```
  I mmu_psram: .rodata xip on psram
  I mmu_psram: .text xip on psram
  I esp_psram: Adding pool of 13824K of PSRAM memory to heap allocator
  I esp_psram: Adding pool of  57K ... (irom 尾部对齐 gap)
  I esp_psram: Adding pool of  58K ... (drom 尾部对齐 gap)
  ```
- map：`.flash.text` 从 `0x40000020` 变成 **`0x50000020`（PSRAM）**，
  `.flash.rodata` → `0x50180120`；`.iram0.text` 反而从 104.5KB 涨到 111.6KB
  （XIP 支持相关代码进 IRAM），内部 RAM 稳态约 109~147KB（视绘制缓冲在哪）。

## 2. 收益（真机实测）

| 组合 | 列表快速滚动最差 | 备注 |
|---|---|---|
| 旧基线（Flash XIP + 内部绘制缓冲） | 24~30 fps | 用户历史实测 |
| **XIP + 内部绘制缓冲** | **40 fps** | 本轮用户实测 |
| **XIP + PSRAM 绘制缓冲** | **34 fps** | 本轮用户实测，省 83KB 内部 RAM |

其他实测（debug / COM11，串口）：

- 0° 播放中 `fps=33~42`，静止 56~62；`cpu0` 仍接近 100%（LVGL 刷屏 + bounce）。
- `App started` 2.30s → **2.57s**（XIP 拷贝 .text/.rodata 约 +95ms，font 拷贝 +167ms）。
- PSRAM 占用：XIP 段 **2.39MB**（text 1,513,312 + rodata 987,812）+ font 副本
  **4.65MB**；开机池 14.27MB，App started 时剩余约 **7.95MB**（PSRAM 绘制缓冲版）。

## 3. 坑：font 分区的 flash mmap 会让整机几秒内崩

### 3.1 现象

只打开 XIP、其它都不改（font 仍用 `esp_partition_mmap`）时，**开机 2~10 秒内
必然 panic 重启**，且与用户操作无关（放在那里也崩）。典型现场：

```
Guru Meditation Error: Core 1 panic'ed (Load/Store access fault)
MEPC 0x5002xxxx  (minimp3 的 L3_imdct36 / mp3dec_decode_frame，PSRAM 代码)
MTVAL 0x501bd7c0 (落在 .rodata 的 g_mdct_window 表) 或 0x5035exxx（PSRAM 栈）
Core0 Saved PC: 0x500fa824 (LVGL wait_for_flushing)
```

即：Core1 的 MP3 解码任务正在跑，**PSRAM 里的代码/只读数据/栈突然不可访问**。

### 3.2 定位过程

1. 先怀疑"绘制缓冲在 PSRAM + SPI bounce"，把绘制缓冲改回内部 DMA：
   **照样崩**（排除绘制缓冲）。
2. 在 `spi_flash_disable_interrupts_caches_and_other_cpu` 入口加临时打印，
   记录每次关外部 cache 的核与调用点：**开机只有 6 次（NVS 读，core=1），
   崩溃前没有任何 flash 操作**。
3. 跳过 font 分区 mmap（`font_npf_init` 直接回退内嵌字库）：
   **连续 60 秒无崩溃**；把 font 改成"mmap → 拷到 PSRAM → munmap"后同样稳定。

### 3.3 机制（两个都成立的怀疑，被同一处修改消除）

- **① 有 flash mmap 时，任何 flash 写都要关外部 cache**：
  `spi_flash_os_func_app.c` 的 `flash_mmap_remain()` 分支里，写/擦会走
  `cache_disable()`；IDF 文档也明确写 "access to cache mapped regions in flash
  is still not allowed while the flash is being erased/written ... cache should
  still be disabled"。XIP 模式下 `.text/.rodata` 就在 PSRAM，一关 cache，
  另一颗核（MP3 解码，PSRAM 栈）立刻 fault。
- **② 4.65MB 的 font mmap 占 72 个 64KB MMU 页**，和 XIP 映射（38 页）、
  PSRAM 堆抢页表；S31 的 MMU 页表容量与 flash/PSRAM 共用，容易踩边界。

诊断打印显示崩溃前没有 flash 写，所以不能 100% 归因于 ①；但 **"XIP 下不要长期
保留 flash mmap"** 这个实践结论是确定的：mmap 消失后 ① 和 ② 同时消失。

## 4. 修复：font 镜像改放 PSRAM

`main/core/fonts/font_npf.c`，`CONFIG_SPIRAM_XIP_FROM_PSRAM` 构建下：

1. 仍然先 `esp_partition_mmap`（走 cache，读 4.65MB 比逐块 `esp_partition_read` 快）；
2. `heap_caps_aligned_alloc(64, total, MALLOC_CAP_SPIRAM)` 拿一块 PSRAM，
   `memcpy` 整个镜像；
3. `esp_partition_munmap()` **立刻解除映射**，把 72 个 MMU 页还回去；
4. `s_base` 指向 PSRAM 副本，后续 cmap/glyph_dsc/位图指针全部指向它。

日志：

```
I font_npf: font 镜像已拷入 PSRAM：4653106 B，167 ms，释放 72 个 64KB MMU 页
I font_npf: NPF1 全字库已挂载: 24px bpp2, 34462 个字形, 183 个 cmap ...
```

非 XIP 构建保持原来的 mmap（省 4.4MB PSRAM，且没有上述问题）。

副作用/代价：

- PSRAM +4.65MB（16MB 里仍余约 8MB）；
- 开机 +167ms（memcpy 4.65MB 走 PSRAM，约 28MB/s）；
- 字形位图从 PSRAM 读，理论上比 flash cache 映射更快（未单独测）。

## 5. 还没验证的

- ~~NVS 写~~ **已验证**（2026-11-16）：反复切换配置槽位无崩溃（此前必崩），
  确认 munmap 之后写 flash 走 `OP_TYPE_MMAP_LOCK`、不关 cache。
- release 构建也会带 XIP（配置在 `sdkconfig.defaults`），release 未实测。
- 90° PPA 旋转路径下的帧率未复测（当前 NVS 旋转是 0°）。

## 6. 最终采用状态（2026-11-16）

- **保留 XIP from PSRAM**（`sdkconfig.defaults`），字体按 §4 拷进 PSRAM；
- **绘制缓冲切回内部 DMA RAM**（`CONFIG_UI_DRAW_BUF_IN_PSRAM` 关，见
  `PSRAM_DRAW_BUF_EXPERIMENT.md` §9.7）——用户实测内部缓冲 40fps、PSRAM 缓冲
  34fps，选择留流畅度；
- 最终 debug 实测：`App started` 2.55s，0° 静态 62fps、列表滚动最差 40fps，
  内部 RAM 余量约 62KB（最大连续块 31.7KB），PSRAM 余量约 8.0MB。

## 7. 回退

删掉 `sdkconfig.defaults` 里的 `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`（或改 n），
`idfc build` 会自动重配；font 自动回到 mmap 路径，不需要改代码。
