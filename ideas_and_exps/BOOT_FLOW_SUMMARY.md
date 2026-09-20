# BOOT_FLOW_SUMMARY — NightPlayer 开机流程

> 本文档汇总 FakePodNanoNightPlayer 中**开机流程**（电池 / SD 卡 / 播放列表 三重检查）的设计、实现与验证结果。
> 前置依赖：`PLAYLIST_SCAN_SUMMARY.md` 所述的播放列表（PLST）扫描与存储系统（`playlist_build_from_sd` / `playlist_store` / pldata 分区）。

---

## 1. 目标与背景

- 设备：ESP32-S3（16MB Flash，8MB PSRAM），460x460 AMOLED，SD 卡（FAT32）。
- 需求：每次开机时**先不进入主界面**，按以下顺序判断并进入对应界面：
  1. **电池电压 < 3.2V** → 拒绝启动，显示"电池电压过低，请充电"警告屏（含当前电压 + 充电后重启提示），**不自动重启**，停在界面直到用户关机/充电。
  2. **未插入 SD 卡** → 提示无卡，并提供**清除扫描数据**（failsafe）按钮，停在界面直到重启。
  3. **有卡但内部分区数据无效** → 提示扫描 + **开始扫描**按钮（无拒绝选项，只能关机）；点击后进入扫描界面，**每 0.3s 更新已扫描音乐数量**；扫描无音乐则提示"未发现音乐"（停在界面直到重启）；否则进入常规界面。
  4. **有卡且数据有效** → 直接进入常规界面。

---

## 2. 开机决策流程

```
开机
  ├─ 电池电压 < 3.2V? ──是──▶ 低电量警告屏（停在此屏）
  ├─ SD 卡未挂载? ──────是──▶ 无卡提示 + 清除扫描数据（failsafe，停在此屏）
  ├─ 播放列表数据无效? ──是──▶ 提示扫描 → 开始扫描按钮
  │                             └─ 扫描界面（300ms 更新数量）
  │                                ├─ 无音乐 → "未发现音乐"（停在此屏）
  │                                └─ 有音乐 → 常规界面
  └─ 有卡 + 数据有效 ──────────▶ 常规界面
```

决策函数为 `boot_flow_run()`，在 `lvgl_port_init()` 之后、持有 LVGL 锁时调用。它**只创建首屏并启动后台扫描任务/定时器，然后立即返回**（不阻塞），后续界面切换由 LVGL 定时器 / 按钮事件 / 扫描完成标志驱动。开机封面 `boot_splash_show()` 由 `main.c` 在更早的 `display_init()` + `lvgl_port_init()` 之后持锁调用（见 4.6 与第 6 节）。

---

## 3. 代码模块

| 文件 | 作用 |
|---|---|
| `ui/boot_flow.h/.c` | 开机流程控制器 + 全部开机界面 + 扫描进度 UI |
| `core/storage/playlist_builder.h/.c` | 新增**进度回调**（`playlist_build_progress_cb`，带 `phase`），并接通**持久化写回** |
| `core/hardware/sd_card.h/.c` | 新增 `sd_card_is_mounted()` 查询接口 |
| `main/main.c` | 用 `boot_flow_run()` 替换旧的"扫描后内存读回测试"流程 |
| `main/CMakeLists.txt` | 注册 `ui/boot_flow.c` |

### 开机界面状态机（`boot_flow.c`）
- `BOOT_ST_SPLASH`：开机封面（默认封面 `DEFAULT_COVER_IMG`，**屏幕最早点亮的那一屏**；由 `boot_splash_show()` 在 app_main 早期创建，随后被下面任一界面替换）
- `BOOT_ST_LOW_BAT`：低电量警告屏（**停在此屏**）
- `BOOT_ST_NO_SD`：无卡提示 + 清除扫描数据（**停在此屏**）
- `BOOT_ST_PROMPT_SCAN`：提示扫描 + 开始扫描按钮
- `BOOT_ST_SCANNING`：扫描中（300ms 更新数量；写入阶段提示"正在写入扫描数据..."）
- `BOOT_ST_NO_MUSIC`：无音乐提示（**停在此屏**）
- `BOOT_ST_NORMAL`：进入常规界面（`app_ui_create()`）

---

## 4. 关键设计

### 4.1 线程安全
- 扫描跑在**独立后台任务**（`xTaskCreatePinnedToCore`，**内部 RAM 32KB 栈**，Core1），因 `esp_partition_mmap` 需冻结 flash cache，PSRAM 栈会触发断言。
- 扫描任务与 LVGL 之间通过 `volatile` 共享变量传进度（只写 / 只读），**扫描任务内不调用任何 LVGL API**。
- 一个 **300ms 的 LVGL 定时器**（LVGL 任务上下文，已持锁）负责刷新计数标签、按 `phase` 切换状态提示、并在 `s_scan_done` 置位后切换到下一界面。

### 4.2 扫描进度回调（`playlist_build_progress_cb`）
- 签名：`void (*)(playlist_build_phase_t phase, uint32_t song_count, void *user_ctx)`。
- `SCAN` 阶段每处理完一个目录回调一次；进入写分区前回调一次 `WRITE` 阶段。
- UI 据此在扫描中显示"正在扫描..."，写入阶段切换为"正在写入扫描数据..."。

### 4.3 无音乐的处理
- 若扫描成功但 `song_count == 0`，扫描任务调用 `playlist_store_invalidate()` 擦除分区头部，使分区保持**无效**状态，保证下次重启仍提示扫描，而非误入常规界面。

### 4.4 failsafe 清除扫描数据
- 无卡界面提供「清除扫描数据」按钮，点击调用 `playlist_store_invalidate()`（只擦 pldata 头部，数据区保留），界面停留并提示"已清除扫描数据"。

### 4.5 常规界面入口
- `boot_enter_normal()` 先删除开机 UI 根容器（`s_root`），再调用 `app_ui_create()`（创建主界面 + 顶部状态栏 + 下拉通知栏）。

### 4.6 开机封面（LVGL 提前启动 + 默认封面首屏，2026-09 增加）
- **动机**：原来 LVGL 排在所有硬件（touch/audio/pldata/button/SD）之后启动，且首帧要等 `app_ui_create()` 完成才提交，实测屏幕到 **1922ms** 才第一次亮，之前一直是纯黑（用户感觉"按了没反应"）。
- **做法**：`lvgl_port_init()` 提前到 `display_init()` 之后**立刻**执行（它只依赖面板句柄与 `config_store`，触摸句柄传 `NULL`、由异步 ready 回调补上），紧接着持 LVGL 锁调用 `boot_splash_show()`（全屏黑底 + 默认封面）→ `lv_refr_now()` → `display_set_brightness(用户亮度)`，然后才做其余硬件初始化。
- **界面回收**：封面用的是同一个 `s_root` 容器，`boot_flow_run()` 的任一分支都会经 `boot_root_create()` 删除它并建自己的界面 → **无需额外清理代码**。
- **不变的约定**：防白屏顺序（亮度 0 时先把整屏写满再置亮度）不变；亮度命令与刷屏必须在同一个 LVGL 锁内连续执行不变；低电（<3.45V）**仍会先显示封面**，约 1 秒后切到低电警告屏（用户确认可接受）。
- **效果**：首次点亮从 **1922ms 提前到实测 704ms**（省约 1.2s），"进入主界面"的时刻不变（约 1.9s）——即封面会停留约 1.24 秒；内存终态与改动前几乎逐字节相同。

---

## 5. 验证结果（真机 COM12）

- 烧录 `FakePodNanoNightPlayer.bin`（约 0xD0x 字节，86% 分区余量）到 COM12，设备硬复位重启。
- 开机流程各分支真机验证**正常**：
  - 电池 / 无卡 / 提示扫描 / 扫描进度（300ms 更新）/ 无音乐 / 数据有效进入常规界面 均符合预期。
- 扫描完成后**写回分区**（`playlist_build_from_sd` 写回 pldata）在开机流程中实际启用（此前该写回为待办，本次在扫描任务中接通）。

---

## 6. 开机时间线实测（debug 版 / COM5，ESP_LOG 时间戳 = 芯片启动后 ms）

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
- 唯一的行为差异：低电（<3.45V）时也会先亮封面约 1 秒再切到低电警告屏（用户已确认接受）。

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
- [x] 开机电池电压检查（<3.2V 拒绝启动，展示当前电压 + 提示）
- [x] 无 SD 卡提示 + 清除扫描数据（failsafe）
- [x] 数据无效 → 开始扫描（无拒绝选项）
- [x] 扫描界面 300ms 实时更新已扫描音乐数
- [x] 扫描无音乐 → "未发现音乐"（停在界面）
- [x] 扫描完成 → 常规界面
- [x] 数据有效 → 直接进入常规界面
- [x] 扫描写回分区（持久化）接通
- [x] 真机 COM12 全流程验证通过

### 待办 / 规划
- [ ] 播放列表 UI：目录/歌单列表 → 歌曲列表 → 播放（后续在常规界面中实现）
- [ ] 扫描阶段如遇读取失败等异常，是否提示具体错误信息（当前统一按"无音乐"处理）
- [ ] （可选）开机各阶段阈值 / 文案做成 Kconfig 可配置

---

*文档整理时间：NightPlayer 开机流程阶段。*
