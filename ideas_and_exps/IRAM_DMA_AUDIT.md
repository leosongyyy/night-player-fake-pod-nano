# IRAM / DMA 内存审计（FUTURE_PLAN 第 34 点）

> 测量时间：2026-09-10；固件基线：`42ed5a3`（LVGL 绘制缓冲已改回 `V_RES/10`）+ 本次新增的 `core/utils/mem_trace` 插桩。
> 硬件：ESP32-S3 成品播放器，8MB Octal PSRAM，SD 卡 61000MB（曲库 171 目录 / 8899 首）。

---

## 0. 结论摘要（先看这里）

1. **封面加载几乎不占内部 RAM，也不占 DMA。**它的"大开销"全部落在 PSRAM：
   `.npd` 文件缓冲 61~77KB + JPEG 数据 61~77KB（均 PSRAM）+ 解码像素 423200B（PSRAM，常驻作为当前封面）。
   整条链路在**内部 RAM 上的净消耗只有 2~4KB**（JPEG 解码的 3100B work buffer 等），且切歌结束即释放。
2. **`PSRAM_TASK_STACK.md` 里"开机流程吃掉 126KB DMA"的归因是错的**：那 126KB 的真凶是
   **`app_ui_create()` 创建的 LVGL 控件对象**（走 `LV_MEM_CUSTOM=stdlib.h` 的 `malloc`，
   单笔 < 16KB → 全部落内部 RAM）。实测 debug 版这一段吃掉 **105KB 内部 RAM / +1795 个分配块**。
3. **releasCandidate 比 debug 少 18~28KB 内部 RAM，差异只有一个：`CONFIG_HEAP_POISONING_COMPREHENSIVE=y`。**
   debug 早在 2026-09 就改成了 `DISABLED`，但 `sdkconfig.defaults.releasecandidate` 没同步（注释还写着"与 debug 一致"）。
4. **故障链条已在 RC 版完整复现**：`app_ui_create` 结束时 DMA 只剩 **955B / 最大连续块 80B**
   → `sdmmc_cmd: allocate_dma_buf: not enough mem` → 读曲失败 → 被判坏曲 → **开机跳曲**；
   同一时刻内部 RAM 最大连续块只有 **2804B**，而 `sleep_timer` 需要 4KB 内部栈 → **睡眠定时器服务起不来**。
5. **JPEG 解码不需要 DMA**（用户此前的疑问）：工作缓冲用 `MALLOC_CAP_DEFAULT`、像素缓冲走 PSRAM，全程不碰 DMA 池。

---

## 1. 测量方法

新增 `main/core/utils/mem_trace.c`（只读统计，不做分配）：

- `mem_trace_log(stage)` 打印一行：`int free/largest(已分配块数) | dma free/largest | psram free`；
- 插桩点：`main.c` 每个初始化阶段后、`boot_flow.c` 的 `player_controller_init` / `app_ui_create` 前后、
  `now_playing.c` 封面任务（`cover:req` / `npd_loaded` / `decoded` / `applied`）；
- 另有落点日志用于**证明**分配落在哪个堆：`song_meta: npd ... -> PSRAM`、
  `cover_jpeg: pixel 423200 B -> PSRAM`（用 `esp_ptr_external_ram()` 判定）。

判读要点：ESP32-S3 上内部 DRAM 同时是 DMA 池的来源，所以"内部 RAM 空闲"与"DMA 空闲"高度同步；
**`blocks` 的增量**是关键——少量块 + 大字节 = 少数大缓冲；大量块 + 中等字节 = 许多小对象。

---

## 2. 实测数据

### 2.1 debug（堆毒化已关，`V_RES/10`）

| 阶段 | int free | int largest | int blocks | dma free | psram free |
|---|---|---|---|---|---|
| 01-config_store | 273167 | 180224 | 70 | 265451 | 7200676 |
| 02-deep_worker | 264327 | 180224 | 75 | 256611 | 7200676 |
| 03-display | 259399 | 180224 | 96 | 251683 | 7200676 |
| 05-battery | 258231 | 180224 | 106 | 250515 | 7196232 |
| 06-audio | 246351 | 172032 | 124 | 238635 | 7195672 |
| 08-button | 241895 | 163840 | 126 | 234179 | 7195672 |
| 09-sd | 240579 | 163840 | 142 | 232863 | 7164892 |
| **10-lvgl_port**（绘制缓冲 2×42320B） | 143711 | 69632 | 227 | 135995 | 7164892 |
| ui:after player_ctrl | 137567 | 65536 | 235 | 129851 | 7164892 |
| ui:before app_ui_create | 136955 | 63488 | 237 | 129239 | 7163736 |
| **ui:after app_ui_create** | **31839** | 23552 | **2032** | 24123 | 6590048 |
| 11-boot_flow+first_frame | 33095 | 23552 | 2020 | 25379 | 6593252 |
| 12-sleep_timer | 28127 | 19456 | 2023 | 20891 | 6593252 |
| 16-perf_monitor | 26967 | 18432 | 2029 | 19251 | 6645580 |
| 稳态（mem_monitor） | 35191 | — | — | — | 6680771 |

### 2.2 releasecandidate（堆毒化 **COMPREHENSIVE** 开，`V_RES/10`）

| 阶段 | int free | int largest | dma free | dma largest | psram free |
|---|---|---|---|---|---|
| 01-config_store | 268923 | 172020 | 261219 | 172020 | 4203844 |
| 10-lvgl_port | 136283 | 63476 | 128579 | 63476 | 4167888 |
| ui:before app_ui_create | 129167 | 57332 | 121463 | 57332 | 4166732 |
| **ui:after app_ui_create** | **4087** | **2804** | **955** | **80** | 3576364 |
| 11-boot_flow+first_frame | 5507 | 2804 | 2375 | 660 | 3579696 |
| 12-sleep_timer | 9963 | 3956 | 6831 | 3956 | 3648700 |
| 16-perf_monitor | 8311 | 3188 | 5179 | 3188 | 3638828 |
| 稳态（mem_monitor） | 16835 | — | — | — | 3655663 |

### 2.3 debug 与 RC 的差额（同一时点）

| 时点 | debug int free | RC int free | 差额 |
|---|---|---|---|
| 01-config_store | 273167 | 268923 | −4.2KB |
| 10-lvgl_port | 143711 | 136283 | −7.4KB |
| ui:before app_ui_create | 136955 | 129167 | −7.8KB |
| ui:after app_ui_create | 31839 | 4087 | **−27.8KB** |
| 16-perf_monitor | 26967 | 8311 | −18.7KB |
| 稳态 | 35191 | 16835 | −18.4KB |

RC 版**代码完全相同**，唯一配置差异是 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`
（`CONFIG_UI_SYSTEM_FONT_FULL` / `CONFIG_UI_COVER_FLASH` 只影响 PSRAM 与 flash，见 §5）。
毒化给每笔分配加"带调用栈的元数据头"且禁止空闲块合并——**分配越多、放大越狠**：
RC 的 `app_ui_create` 段比 debug 多耗 20KB，正好对应 ~1795 个分配块的开销放大。

---

## 3. 封面加载的真实账目（回答"为什么占这么多"）

RC 版一次切歌的完整日志（`-> PSRAM` 为实测落点）：

```
song_meta: npd 77250 B -> PSRAM            ← .npd 整个文件读入内存（fopen/fread）
song_meta: jpeg data 77136 B -> PSRAM      ← 从 .npd 中切出的 JPEG 数据副本
cover_jpeg: pixel 423200 B -> PSRAM        ← 460×460 RGB565 解码像素（常驻为当前封面）
cover_jpeg: decoded 460x460 (423200 B)
```

| 项目 | 大小 | 落点 | 生命周期 |
|---|---|---|---|
| `.npd` 文件缓冲（`song_meta.c` 的 `malloc(fsize)`） | 61~77KB | PSRAM（`fsize > 16KB` 走 `MALLOC_CAP_DEFAULT｜SPIRAM` 分支） | 本次加载临时，`song_meta_free` 即释放 |
| JPEG 数据副本 | 61~77KB | PSRAM（显式 `MALLOC_CAP_SPIRAM`） | 同上 |
| 解码像素缓冲 | 423200B | PSRAM（显式 `MALLOC_CAP_SPIRAM`） | **常驻**（当前封面），切歌时释放旧图 |
| JPEG work buffer（`esp_jpeg_decode` 内部） | 3100B | 内部 RAM（`MALLOC_CAP_DEFAULT`，无 DMA 要求） | 解码期间临时 |
| title/subtitle 文本 | < 1KB | 内部 RAM（`< 16KB` 阈值） | 本次加载临时 |

**结论**：封面链路在内部 RAM 上的峰值开销只有 ~3~4KB，一次性；在 DMA 上是 **0**。
PSRAM 峰值约 545KB、常驻 423KB——这是"看起来占了很多空间"的全部来源，与内部 RAM/DMA 无关。

---

## 4. 真凶：`app_ui_create()` 的 LVGL 控件对象

`CONFIG_LV_MEM_CUSTOM=y` + `CONFIG_LV_MEM_CUSTOM_INCLUDE="stdlib.h"` → LVGL 所有对象走 `malloc()`；
而 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 使 **≤16KB 的分配强制落内部 RAM**
（IDF 5.5 `heap_caps_malloc_default`：`size <= 阈值` → `MALLOC_CAP_DEFAULT|MALLOC_CAP_INTERNAL`）。

实测（debug）：`ui:before app_ui_create` → `ui:after app_ui_create`
**内部 RAM 136955 → 31839（−105116B），分配块 237 → 2032（+1795 块）**，平均 58B/块——典型的
`lv_obj` / 样式 / label 文本 / 动画 / 定时器小对象。细分时间线（debug）：

- `main_ui_create()`（tabview + 目录页 + 播放列表页 + 正在播放页 + 播放设置页 + ClickWheel）：
  136955 → 95983（−41KB），期间还并行跑了封面任务；
- `create_status_bar()` + `notification_panel_create()`：约 −8KB；
- `settings_menu_create()`（全屏 lv_menu + 暗色主题 + 开发者选项页）：**剩余大头，约 −55KB**，
  创建耗时也是最长的一段（I(1667)→I(2016)，约 350ms）。

即：**开机常驻 UI 对象 ≈ 105KB 内部 RAM（= DMA 池）**，这才是"V_RES/10 放不下"的根本原因；
封面加载只是恰好与它同时期发生，被误记为凶手。

---

## 5. 失败链条（RC 版实测复现）

```
I (2017) MEMTRACE: ui:after app_ui_create int 4087/2804(2033) | dma 955/80 | psram 3576364
E (1787) sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
E (1788) diskio_sdmmc: sdmmc_read_blocks failed (0x101)
```

1. `app_ui_create` 把内部 RAM 压到 4087B、最大连续块 2804B，DMA 池只剩 **955B / 连续 80B**；
2. SD 卡读一次 512B 都分配不出 → `sdmmc_read_blocks failed` → 解码器未启动 →
   `mp3_player` 判"数据损坏" → 自动跳曲 → **用户看到的"开机先播一秒再跳走"**；
3. 同时 `sleep_timer_create` 需要 4KB **内部栈**，而当时最大连续块只有 2804B
   （本次因 `cover:applied` 释放了 `.npd` 而侥幸成功，largest 回到 20468B；时序稍差即失败）→
   **"睡眠定时器服务未能启动"**。

> 这解释了两点历史现象：① 为什么 `/12`（省 14KB DMA）能活、`/10`（多 14KB）就死——
> 余量本来就在几百字节量级；② 为什么"debug 从未出现、只有 RC/release 出现"——
> 差别不是版本号，而是 **RC 独有堆毒化带来的 18~28KB**。

---

## 6. 验证：关闭 RC 堆毒化后故障消失（2026-09-10 实测）

把 `sdkconfig.defaults.releasecandidate` 改成 `CONFIG_HEAP_POISONING_DISABLED=y`（代码一字未改），
删掉 `build/releasecandidate/sdkconfig` 重新 configure 后烧录实测：

| 阶段 | RC（毒化开） | RC（毒化关） | debug |
|---|---|---|---|
| 01-config_store | 268923 / 172020 | 273167 / 180224 | 273167 / 180224 |
| 10-lvgl_port | 136283 / 63476 | 143711 / 69632 | 143711 / 69632 |
| ui:before app_ui_create | 129167 / 57332 | 136955 / 63488 | 136955 / 63488 |
| **ui:after app_ui_create** | **4087 / 2804**，dma **955 / 80** | **31995 / 22528**，dma **24279 / 22528** | 31839 / 23552，dma 24123 / 23552 |
| 12-sleep_timer | 9963 / 3956 | 28787 / 18432 | 28127 / 19456 |
| 16-perf_monitor | 8311 / 3188 | 27135 / 17408 | 26967 / 18432 |
| 稳态 `iram=`（mem_monitor） | 16835 | **35355** | 35191 |

（格式：`内部 RAM free / largest`；dma 同格式）

结论：

- **DMA 池从 955B / 连续 80B 恢复到 24279B / 22528B（+23KB）**，本次启动**不再出现**
  `sdmmc_cmd: allocate_dma_buf: not enough mem`；
- `sleep_timer` 正常启动（`sleep timer ready`），首曲正常播放——**跳曲与定时器起不来的问题同时消失**；
- 关闭毒化后 RC 与 debug 的逐阶段读数几乎逐字节相同（`01-config_store`/`10-lvgl_port`/
  `ui:before app_ui_create` 三项**完全一致**），反证了"两套配置的差异确实只有堆毒化"；
- 切到列表页/滚动时内部 RAM 短暂下降约 1.7KB（虚拟列表行槽），余量仍在 33KB 以上。

**V_RES/10 验收结果（RC，毒化关）**：DMA 稳态 19.4KB（要求 ≥8~10KB ✓）、
内部最大连续块 17.4KB（要求 ≥4KB ✓）、SD 读写无失败 ✓、睡眠定时器正常 ✓、播放正常 ✓。
**第 34 点的验收标准已达成。**

---

## 7. 修复建议（按性价比排序）

| 优先级 | 措施 | 预计收益 | 风险 |
|---|---|---|---|
| **P0 ✅ 已实施** | 把 `sdkconfig.defaults.releasecandidate` 的 `CONFIG_HEAP_POISONING_COMPREHENSIVE` 改为 `DISABLED`（与 debug 对齐）——实测验证见 §6 | 实测内部 RAM **+18~28KB**、DMA 池 **+23KB**，跳曲与定时器问题同时消失 | 失去"释放时立即报 CORRUPT HEAP + 调用栈"能力；需要抓堆破坏时按注释临时开回 |
| **P1** | 降低 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（16384 → 4096/2048）：让 LVGL 小对象落 PSRAM | 内部 RAM 最多 **+80~100KB** | PSRAM 访问延迟高于内部 RAM，需实测列表滚动帧率；建议只降阈值而非全量搬迁 |
| **P2** | LVGL 内存分配改走自定义 `LV_MEM_CUSTOM_ALLOC`（按大小分流：小对象内部、大对象 PSRAM，或反选）+ 统计 | 精确可控，且能直接量出"LVGL 到底占多少" | 需要改 lv_conf 注入点 |
| **P3** | 懒创建重对象：`settings_menu`（约 55KB）与 `notification_panel` 改为首次打开时创建 | 开机常驻内部 RAM **+50KB 以上** | 首次打开设置会有一帧延迟（可配合 loading） |
| **P4** | 保持 `V_RES/10` 的判定基线：`dma free ≥ 8~10KB` 且 `largest ≥ 4KB`（后者保证 SD 512B 与 4KB 内部栈都能分配） | 防止再次踩线 | — |

---

## 8. 本次新增/改动的文件

- **新增** `main/core/utils/mem_trace.h` / `mem_trace.c`：只读内存快照（不做任何分配）。
  由 Kconfig `CONFIG_MEM_TRACE` 控制（`main/Kconfig.projbuild`，默认 y，
  **release 构建关闭**，见 `sdkconfig.defaults.release`）；关闭时头文件把
  `mem_trace_log` / `mem_trace_ptr` 退化为空宏，调用点无需 `#if`，也不产生任何代码与日志。
- `main/main.c`、`main/ui/boot_flow.c`、`main/ui/now_playing.c`：各阶段插桩调用
- `main/core/music/song_meta.c`、`main/core/image/cover_jpeg.c`：改用 `mem_trace_ptr()` 打印落点
- `main/CMakeLists.txt`：`CONFIG_MEM_TRACE` 条件编译 `core/utils/mem_trace.c`
- `sdkconfig.defaults.releasecandidate`：关闭堆毒化（P0 修复，附实测说明）
- `sdkconfig.defaults.release`：`# CONFIG_MEM_TRACE is not set`
