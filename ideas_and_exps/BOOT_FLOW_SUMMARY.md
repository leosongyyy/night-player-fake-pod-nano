# BOOT_FLOW_SUMMARY — NightPlayer 开机流程

> 本文档汇总 FakePodNanoNightPlayer 中**开机流程**（电池 / SD 卡 / 播放列表缓存 三重检查）的设计、实现与验证结果。
> 前置依赖：`PLAYLIST_SCAN_SUMMARY.md` 所述的播放列表（PLST）扫描与存储系统（`playlist_build_from_sd` / `playlist_store` / pldata 分区）。

---

## 1. 目标与背景

- 设备：**ESP32-S31**（FakePodNano Pro V1，320MHz 双核，16MB Flash + 16MB Octal PSRAM），460x460 AMOLED（CO5300 QSPI），SD 卡（FAT32/exFAT）。
- 需求：每次开机时**先不进入主界面**，按以下顺序判断并进入对应界面：
  1. **插着充电器开机** → 直接进**充电专用界面**（大环电量 + 充电状态；拔电后由 PMU 关机，见 4.7）。
  2. **未插入 SD 卡** → 提示无卡，并提供**清除扫描数据**（failsafe）按钮，停在界面直到重启。
  3. **有卡时核对播放列表缓存** → 无有效缓存 / 缓存元信息缺失 / SD 卡 ID 变化 / 曲库指纹变化 → **自动进入扫描界面**（无需用户点按钮），**每 0.3s 更新已扫描音乐数量**；扫描无音乐则提示"未发现音乐"，扫描/写入失败则显示"扫描失败"，两种异常都停在界面直到重启；否则进入常规界面。
  4. **有卡且缓存与当前卡/曲库一致** → 直接进入常规界面。

> ⚠ S3 时代的「电池电压 < 3.2V 拒绝启动」在 S31 迁移时随分压 ADC 删除；2026-11 已按 AXP2101 的**电量百分比**接回：**电量 < 2% 且未充电 → 只显示「电量过低，请充电」，5 秒后由 PMU 直接断电**（不停在此屏）。运行中的低电关机在 `power_service`（<5% 且未充电持续 60 秒），见 `PMU_RTC_DRIVERS.md` §7。

---

## 2. 开机决策流程

```
开机
  ├─ 插充电器开机? ────是──▶ 充电专用界面（拔电后由 PMU 关机）
  ├─ 电量 <2% 且未充电? ─是──▶ 「电量过低，请充电」→ 5 秒后 PMU 断电
  ├─ SD 卡未挂载? ──────是──▶ 无卡提示 + 清除扫描数据（failsafe，停在此屏）
  ├─ 有卡：缓存有效且卡 ID/指纹一致? ──否──▶ 自动扫描
  │                                           └─ 扫描界面（300ms 更新数量）
  │                                              ├─ 无音乐 → "未发现音乐"（停在此屏）
  │                                              ├─ 扫描/写入失败 → "扫描失败"（停在此屏）
  │                                              └─ 有音乐 → 常规界面
  └─ 有卡 + 缓存一致 ────────────────────────▶ 常规界面
```

决策函数为 `boot_flow_run()`，在 `lvgl_port_init()` 之后、持有 LVGL 锁时调用。它**只创建首屏并启动后台扫描任务/定时器，然后立即返回**（不阻塞），后续界面切换由 LVGL 定时器 / 按钮事件 / 扫描完成标志驱动。开机封面 `boot_splash_show()` 由 `main.c` 在更早的 `display_init()` + `lvgl_port_init()` 之后持锁调用（见 4.6 与第 6 节）。

---

## 3. 代码模块

| 文件 | 作用 |
|---|---|
| `ui/boot_flow.h/.c` | 开机流程控制器 + 全部开机界面 + 扫描进度 UI + 自动缓存核对 |
| `core/storage/playlist_builder.h/.c` | 进度回调（`playlist_build_progress_cb`）、持久化写回、**快速指纹探针** |
| `core/storage/playlist_store.h/.c` | 缓存头部元信息（卡 ID / 曲库指纹 / 文件数）与 `get_meta()` |
| `core/hardware/sd_card.h/.c` | `sd_card_is_mounted()` 查询接口 + `sd_card_get_id()`（CID 派生卡身份） |
| `main/main.c` | 用 `boot_flow_run()` 替换旧的"扫描后内存读回测试"流程 |
| `main/CMakeLists.txt` | 注册 `ui/boot_flow.c` |

### 开机界面状态机（`boot_flow.c`）
- `BOOT_ST_SPLASH`：开机封面（默认封面 `DEFAULT_COVER_IMG`，**屏幕最早点亮的那一屏**；由 `boot_splash_show()` 在 app_main 早期创建，随后被下面任一界面替换）
- `BOOT_ST_LOW_BAT`：**低电拒启**（电量 < 2% 且未充电）——只显示一行「电量过低，请充电」，5 秒后用 AXP2101 `0x10[0]=1` **直接断电**（不停在此屏；5 秒内画面不会变化，也不需要用户操作）
- `BOOT_ST_NO_SD`：无卡提示 + 清除扫描数据（**停在此屏**）
- `BOOT_ST_SCANNING`：扫描中（300ms 更新数量；写入阶段提示"正在写入扫描数据..."）
- `BOOT_ST_NO_MUSIC`：无音乐提示或扫描失败提示（**停在此屏**）
- `BOOT_ST_NORMAL`：进入常规界面（`app_ui_create()`）
- `BOOT_ST_CHARGING`：**插电开机充电界面**（`boot_flow_run_charger_mode()`，由 `main.c` 按 `boot_reason_is_charger_boot()` 选择，不经过 `boot_flow_run()`；大环电量 + 充电状态文字，10 秒无操作自动息屏，拔掉充电器后经宽限期由 PMU 真关机）

---

## 4. 关键设计

### 4.1 线程安全
- 扫描跑在**独立后台任务**（`xTaskCreatePinnedToCore`，**内部 RAM 栈**，Core1），因 `esp_partition_mmap` 需冻结 flash cache，PSRAM 栈会触发断言。⚠ 2026-11 修复：原调用处传 `BOOT_SCAN_STACK_BYTES / 4u`，而 IDF 的 `xTaskCreate` 栈单位就是**字节** → 实际只有 **8KB**。现在传真正的 **32KB**，分配失败回退 **16KB**；任务创建失败会删除进度定时器并显示「扫描失败」页，不再带着空列表进入主界面。
- 扫描任务与 LVGL 之间通过 `volatile` 共享变量传进度（只写 / 只读），**扫描任务内不调用任何 LVGL API**。
- 一个 **300ms 的 LVGL 定时器**（LVGL 任务上下文，已持锁）负责刷新计数标签、按 `phase` 切换状态提示、并在 `s_scan_done` 置位后切换到下一界面。

### 4.2 自动缓存核对（2026-11 反哺）
开机在 SD 挂载后、扫描前先核对 pldata v2 头部：

- `playlist_store_is_valid()` + `playlist_store_get_meta()` 读取 `card_id / signature / file_count`；
- 卡 ID 与当前 `sd_card_get_id()` 不同 → `config_store_reset_position()` 清旧卡播放位置，自动扫描；
- 卡相同 → `playlist_builder_probe_signature()` 快速遍历曲库并与缓存比对；不一致自动扫描；
- 一致则直接进入常规界面。

详细算法、性能与边界见 `PLAYLIST_SCAN_SUMMARY.md` §4.1。

### 4.2 扫描进度回调（`playlist_build_progress_cb`）
- 签名：`void (*)(playlist_build_phase_t phase, uint32_t song_count, void *user_ctx)`。
- `SCAN` 阶段每处理完一个目录回调一次；进入写分区前回调一次 `WRITE` 阶段。
- UI 据此在扫描中显示"正在扫描..."，写入阶段切换为"正在写入扫描数据..."。

### 4.3 无音乐与扫描失败的处理
- 若扫描成功但 `song_count == 0`，扫描任务调用 `playlist_store_invalidate()` 擦除分区头部，使分区保持**无效**状态，保证下次重启仍自动扫描，而非误入常规界面。
- 若扫描/写分区失败或扫描任务创建失败，`boot_progress_timer_cb` 在 `s_scan_ok == false` 时切到「扫描失败」页并停留；不会调用 `player_controller_init()` 进入空列表主界面。

### 4.4 failsafe 清除扫描数据
- 无卡界面提供「清除扫描数据」按钮，点击调用 `playlist_store_invalidate()`（只擦 pldata 头部，数据区保留），界面停留并提示"已清除扫描数据"。

### 4.5 常规界面入口
- `boot_enter_normal()` 先删除开机 UI 根容器（`s_root`），再调用 `app_ui_create()`（创建主界面 + 顶部状态栏 + 下拉通知栏）。

### 4.6 开机封面（LVGL 提前启动 + 默认封面首屏，2026-09 增加）
- **动机**：原来 LVGL 排在所有硬件（touch/audio/pldata/button/SD）之后启动，且首帧要等 `app_ui_create()` 完成才提交，实测屏幕到 **1922ms** 才第一次亮，之前一直是纯黑（用户感觉"按了没反应"）。
- **做法**：`lvgl_port_init()` 提前到 `display_init()` 之后**立刻**执行（它只依赖面板句柄与 `config_store`，触摸句柄传 `NULL`、由异步 ready 回调补上），紧接着持 LVGL 锁调用 `boot_splash_show()`（全屏黑底 + 默认封面）→ `lv_refr_now()` → `display_set_brightness(用户亮度)`，然后才做其余硬件初始化。
- **界面回收**：封面用的是同一个 `s_root` 容器，`boot_flow_run()` 的任一分支都会经 `boot_root_create()` 删除它并建自己的界面 → **无需额外清理代码**。
- **不变的约定**：防白屏顺序（亮度 0 时先把整屏写满再置亮度）不变；亮度命令与刷屏必须在同一个 LVGL 锁内连续执行不变。封面在所有开机分支之前点亮，随后被对应界面替换（原「低电时先显示封面约 1 秒再切低电警告屏」的记录已随低电拒启删除而失效）。
- **效果**：首次点亮从 **1922ms 提前到实测 704ms**（省约 1.2s），"进入主界面"的时刻不变（约 1.9s）——即封面会停留约 1.24 秒；内存终态与改动前几乎逐字节相同。

### 4.7 插电开机：充电专用界面（S31 新增，2026-11）
- **判据**：`main.c` 在 `boot_reason_init()` 之后调用 `boot_reason_is_charger_boot()`（`esp_reset_reason` + PMU 0x20/0x21 的开机原因），为真时走 `boot_flow_run_charger_mode()`，**不经过** `boot_flow_run()`，所以 SD/播放列表检查一个都不做。
- **界面**：330×330 大环（12 点方向起顺时针填充）显示电量百分比，环心为大号百分比 + 状态文字；`battery_read_info()` 每秒刷新，PMU 读数失败时显示 `--%`/「未知」而不是假 0。
- **交互**：10 秒无按键/触摸自动息屏（`charging_screen_off()`，走既有息屏深链）；拔掉充电器后经宽限期由 PMU 真关机（`charging_shutdown()`），关机中/失败后不再重复触发。
- **代价**：这一分支完全不碰播放列表与播放引擎，也不建主界面——插电开机只充电，不播放。

---

## 5. 验证结果（真机 COM12）

- 烧录 `FakePodNanoNightPlayer.bin`（约 0xD0x 字节，86% 分区余量）到 COM12，设备硬复位重启。
- 开机流程各分支真机验证**正常**：
  - 电池 / 无卡 / 自动扫描 / 扫描进度（300ms 更新）/ 无音乐 / 扫描失败 / 缓存一致进入常规界面 均符合预期。
- 自动发现真机验证（2026-11，同一 S31 硬件）：
  - 清空 pldata 头部 → `auto scan required (no valid cache)` → 自动扫描并保存卡 ID/指纹；
  - 重启 → `library probe ... in 161 ms` + `playlist cache match`，直接进主界面；
  - 修改缓存指纹 → `library changed` → 自动重扫；
  - 修改缓存卡 ID → `SD card changed` + `position reset` → 自动重扫。
- 扫描完成后**写回分区**（`playlist_build_from_sd` 写回 pldata）在开机流程中实际启用（此前该写回为待办，本次在扫描任务中接通）。

---

## 6. 开机时间线实测（**S3 时代（2026-09）**；debug 版 / COM5，ESP_LOG 时间戳 = 芯片启动后 ms）

> ⚠ 本节是 S3（240MHz / 8MB PSRAM）上的实测，S31 上未逐项复测；数值只作历史对照，不代表 2.x 现状。

改动**前**（LVGL 在所有硬件之后启动）实测：

| 时刻 | 阶段 |
|---|---|
| 334 | `app_main` 开始（之前 ~330ms 是 ROM/bootloader/IDF） |
| 404–599 | `display_init`（195ms，其中 120ms 是 0x11 SLEEP OUT 规格等待），全黑、亮度 0 |
| 599–641 | `touch_init`（I2C 扫描；控制器初始化异步） |
| 650–772 | battery + audio + EQ 恢复 |
| 779–819 | pldata + button |
| 828–947 | SD 挂载（119ms，120GB 卡） |
| 957–1008 | **`lvgl_port_init`**（绘制缓冲 2×41KB DMA，内部 RAM 239,983 → 143,115） |
| 1014–1092 | `boot_flow_run`：播放列表 mmap（172 目录 / **8965 首**）+ `player_controller_init` |
| 1102–1804 | **`app_ui_create` ≈ 700ms**（main_ui 1112–1394；状态栏+通知栏 1400–1455；**settings_menu 1457–1803 = 346ms**） |
| 1811–1885 | 进常规界面 + 首帧渲染 |
| **1922** | **`Turn on display`（亮度 193）→ 屏幕第一次亮** |
| 1960 | `App started` |

改动**后**（第 1 步：LVGL 提前启动 + 开机封面；实测，同上设备/同一张 8965 首卡）：

| 时刻 | 阶段 |
|---|---|
| 403–598 | `display_init` |
| 598–648 | **`lvgl_port_init`**（提前到此处，48ms） |
| 654 | `boot splash: default cover shown`（创建全屏黑底 + 默认封面） |
| **704** | **`Turn on display (boot splash, brightness 193)` → 屏幕第一次亮（原来 1922ms，省 1.22s）** |
| 709–1060 | touch / battery / audio / pldata / button / SD（全部在封面背后进行） |
| 1070–1128 | 播放列表 mmap + `player_controller_init` |
| 1147–1817 | `app_ui_create`（约 670ms） |
| 1831 | `enter normal UI` |
| **1943** | 复核亮度（`restore brightness 193`）→ 封面切换到主界面 |
| 1960 | `App started`（与改动前相同） |

- 封面停留时间 = 1943 − 704 ≈ **1.24 秒**；内存终态与改动前几乎逐字节相同（`12-boot_flow+first_frame` int free 28979 / dma 21263，改动前 28991 / 21275），SD 读无 `allocate_dma_buf` 失败、无坏曲误判。
- 原记录的「低电（<3.45V）时先亮封面约 1 秒再切低电警告屏」差异已随低电拒启删除而消失。

改动**后**（第 2 步：再叠加**设置菜单分级懒加载**，见 `DONE_PLANS.md` 第 44 点）：

| 时刻 | 阶段 |
|---|---|
| 654 | `boot splash: default cover shown` |
| **704** | **屏幕第一次亮（不变）** |
| 709–1064 | touch / battery / audio / pldata / button / SD |
| 1151–1497 | **`app_ui_create` 约 357ms**（原约 700ms；`settings_menu` 已移出开机路径） |
| **1638** | **复核亮度 → 封面切主界面（原 1943ms，再提前 305ms）** |
| 1648 | `App started`（原 1960） |
| 3452→3505 | 首次点开设置：`settings menu shell created (44 ms)` → `settings menu shown` |
| 19100 | `developer options page built`（关于页子页，首次进入才构建） |

- 内存终态：`12-boot_flow+first_frame` 内部 RAM 余量 **28.9KB → 58.3KB**、DMA 余量 **21.3KB → 50.5KB**（设置菜单实测约 **29KB**，`IRAM_DMA_AUDIT.md` 早先估的 55KB 偏大，已修正）。
- 设置菜单 6 个子页在**首次点进时**各构建一次（真机逐页验证无卡顿、无崩溃）。

---

## 7. 当前状态与后续

### 已完成
- ~~[x] 开机电池电压检查（<3.2V 拒绝启动，展示当前电压 + 提示）~~ —— S3 的电压判据已随分压 ADC 删除；2026-11 改为按 AXP2101 电量百分比：**<2% 且未充电 → 提示 5 秒后 PMU 关机**（见 §2/§4.7）
- [x] 插电开机 → 充电专用界面（2026-11，S31；见 4.7）
- [x] 无 SD 卡提示 + 清除扫描数据（failsafe）
- [x] **自动缓存核对 + 换卡识别 + 曲库变化自动扫描（2026-11）**
- [x] **扫描任务栈修正为真正 32KB（失败回退 16KB）**
- [x] 扫描界面 300ms 实时更新已扫描音乐数
- [x] 扫描无音乐 → "未发现音乐"（停在界面）
- [x] 扫描/写入失败 → "扫描失败"（停在界面）
- [x] 扫描完成 → 常规界面
- [x] 缓存一致 → 直接进入常规界面
- [x] 扫描写回分区（持久化）接通
- [x] 播放列表 UI：目录/歌单列表 → 歌曲列表 → 播放（后续在常规界面中实现）
- [x] 真机 COM12 全流程验证通过

### 待办 / 规划
- [ ] 扫描阶段如遇读取失败等异常，是否提示具体错误信息（当前统一按"无音乐"处理）
- [ ] （可选）开机各阶段阈值 / 文案做成 Kconfig 可配置

---

*文档整理时间：NightPlayer 开机流程阶段。*
