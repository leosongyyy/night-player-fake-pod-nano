## FakePodNanoNightPlayer 项目概览

### 分支说明（2026-11 起）

本仓库有两个长期分支，改代码前先确认自己在哪条线上：

- **`master`（本分支）**：面向**最新硬件（ESP32-S31 / FakePodNano Pro，见 `ideas_and_exps/reference_docs/hardware_v1.md`）**的固件开发主线。新功能、平台迁移都在这里做；S3 时代的构建矩阵（`dac_chip` / `font` 维度）与 `releasecandidate` 构建类型已移除，只剩 debug / release / sync，DAC 固定 CS43131、字体固定为位图（NPF1 全字库）。
- **`legacy_s3`**：S3 已出货硬件的维护分支，只收 bug 修复与稳定性改进，**不加新功能、不做平台迁移**。

⚠ 两条线都要的修复：先在 `legacy_s3` 改好并验证，再 `git cherry-pick` 到 `master`；**不要 `merge`**（sdkconfig/分区表/硬件层会大幅分叉）。

### 环境说明

**1. 硬件**（详细引脚/外设见 `ideas_and_exps/reference_docs/hardware_v1.md`）

ESP32-S31（rev v0.0，320MHz 双核 + LP core，512KB SRAM）+ **16MB Flash** + **16MB Octal PSRAM（200MHz）**；CO5300 AMOLED 460×460 QSPI（窗口 X 偏移 10，**VCI_EN=GPIO44 必须先拉高**）；CST820 触摸（I²C0）；AXP2101 PMU（I²C0 0x34，电源键也接它）+ PCF85063 RTC（0x51）+ QMI8658 IMU（0x6b，未接）；CS43131 耳放（I²C1 0x30 + I²S1，24.576MHz 板载晶振）；SD 4-bit SDMMC。

- **PMU/RTC 已接入**：电压/电量/充电状态/温度、电源键 IRQ、RTC 读时间与首次校时、**软关机（真断电）**、**开机原因与插电开机的充电专用界面**、**电池健康保护（4.0V 软件限充）**、**低电保护按电量百分比**（开机 <2% 且未充电 → 5 秒后关机；使用中 <5% 未充电持续 60s → 自动关机）。见 `PMU_RTC_DRIVERS.md`、`POWER_OPTIONS_AND_CHARGE_BOOT.md`。
- **按键**：上键 = AXP2101 PWRON（无主控 GPIO，经 GPIO49 中断维护按下/抬起）+ 下键 = GPIO61。`button_service` 生成"上/下键短按、长按、双键"五类事件，`key_handler` 做应用映射（详见 `ui/key_handler.c` 注释）。

**2. 开发环境**

- ESP-IDF **6.1**（`C:\esp\v6.1\esp-idf`，GCC 15.2.0），激活脚本 `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`；本机同时保留 5.5.5（供 `legacy_s3`）。**构建/烧录统一用 `idfc`**（见第 4 节）。迁移过程与两次真机崩溃的根因见 `ideas_and_exps/IDF6_MIGRATION.md`。
- ⚠ **exFAT 需改 IDF 本体**：`C:\esp\v6.1\esp-idf\components\fatfs\src\ffconf.h` 里 `FF_FS_EXFAT=1`。换电脑/重装 IDF 必须重打，否则 SD 挂载失败并进"无卡"界面（`IDF6_MIGRATION.md` §6.7）。
- ⚠ **托管组件有构建期补丁**（`patches/`，由 `main/CMakeLists.txt` 在配置阶段幂等回写；托管组件会被组件管理器覆盖，只能这样改）：`lvgl_fast_mem_decl/`（修 LVGL 声明/定义上 `LV_ATTRIBUTE_FAST_MEM` 与 IDF 6.1 `IRAM_ATTR` 的 `-Werror=attributes` 冲突，上游 PR #10737）、`esp_lcd_touch_deps/`（给 `esp_lcd_touch` 补 `PRIV_REQUIRES esp_driver_gpio`）。**属自定义写法，勿当无用代码清理**。仓库内 `components/` 的本地组件直接改各自 CMakeLists，不走补丁。
- ⚠ **构建只编 `COMPONENTS` 白名单里的组件**（根 `CMakeLists.txt`，约 90 个，IDF 全量是 155 个）。规则：**新增任何 `#include` 的组件必须加进白名单**，否则报找不到头文件；4 个本地组件（AMOLED + 3 个触摸驱动）必须显式列着。**白名单在 `include(project.cmake)` 之前执行、拿不到 `CONFIG_*`，所以不能按固件角色裁剪**——`esp_wifi/esp_netif/esp_event/lwip/wpa_supplicant`（只给对时固件）只能无条件列着，靠"没被引用就不进镜像"来保证播放器零成本（实测 0 个 `.obj`，见 `DUAL_APP_TIME_SYNC.md` §4.1）。完整说明见 `MBEDTLS_BUILD_AUDIT.md` §9。
- ⚠ **`components/spi_flash/` 是 IDF 组件的本地 fork**，唯一改动 `cache_utils.c` 在 flash 操作窗口关/恢复分支预测，修 S31 的双核 cache 竞态（IDFGH-18107 / espressif/esp-idf#18948）。**不要删除**；上游修好后才能删。详见 `S31_FLASH_CACHE_RACE.md`。
- LVGL **9.4.0**（托管组件 `lvgl/lvgl`）；2026-10 由 8.4 升级，**碰 LVGL 前先读 `LVGL9_MIGRATION.md`**（历史文档里的 API/内存结论多为 8.4 时代）。
- 字体 = font 分区里的 `spiffs/system.npf`（NPF1 位图全字库，4.44MiB，运行时 mmap）。**字体不在固件里**：刷完固件要手动 `esptool write-flash 0x600000 spiffs/system.npf`（见 `FONT_FLASH.md`）；换字体用 `tools/font` 重新烘焙。发版可用 `tools/make_full_firmware.py` 生成含字体（+对时固件）的全量固件（见 `FULL_FIRMWARE.md`）。

**3. 参考文档**（都在 `ideas_and_exps/`）

`IDFC_USAGE.md`（idfc 完整用法/历史坑）、`BUILD_CONFIG.md`（构建配置）、`BUILD_MATRIX.md`（配置矩阵格式与 idfc 验收）、`AGENTS_DETAILS_ARCHIVE.md`（**2026-11-15 从本文件移出的历史实测数据与逐项理由**，考据时查这里）、`reference_docs/`（原厂资料整理版：`hardware_v1.md`、`pmu_axp2101.md`、`rtc_pcf85063a.md`）、`DUAL_APP_TIME_SYNC.md`（双固件对时）、`WIFI_TIME_SYNC.md`（对时实现细节）、`BLUETOOTH_AUDIO.md`、`NPF_FONT.md` + `BINFONT_RESEARCH.md`、`FONT_FLASH.md`、`FULL_FIRMWARE.md`、`LVGL9_MIGRATION.md`、`IRAM_DMA_AUDIT.md`、`IRAM_AUDIT_LVGL9.md`、`IRAM_OBJECTS_PSRAM_S31.md`、`PSRAM_TASK_STACK.md`、`FLASH_WRITE_VS_PSRAM.md`、`NVS_WRITE_BUDGET.md`、`CONFIG_SLOTS.md`、`COVER_JPEG_HW.md`、`PPA_ACCEL.md`、`MARQUEE_STYLE.md`、`CLICK_WHEEL_USAGE.md`、`DISPLAY_FREEZE_TRIAGE.md`、`S31_FLASH_CACHE_RACE.md`、`DONE_PLANS.md` / `FUTURE_PLAN.md`。

**4. idfc 使用说明（重要）**

`idfc.exe` 是"服务器 + 客户端"封装：服务器激活 IDF 环境后常驻监听 `http://localhost:19781/idf-client-cli/`，客户端把 `idf.py` 命令发给它执行。下载地址 <https://gitee.com/ZYFDroid/idf-client-cli/releases/>，**放在 PATH 目录（工作区外）**，之后在项目目录里直接 `idfc <子命令>`（客户端靠当前目录找 `.idfc/idfc_config.json`）。

> ⚠ 本项目是「子命令版」idfc：`--server`、`-B build/<cfg> build`、`-B ... reconfigure` 等旧写法一律报"没有这样的命令"。**历史文档里的 idfc 命令不要照抄**；完整说明见 `IDFC_USAGE.md`，全部参数用 `idfc help --all`。

```powershell
idfc set-idfenv "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"  # 每项目一次
idfc set-variant debug ; idfc ports ; idfc set-port COMn ; idfc status      # 每项目一次
idfc start-server        # 用户手动启动（控制台=启动它的窗口，关窗口即停）
idfc build [--reconfigure|--no-reconfigure|--fullclean] [--dry-run] [-t release|sync] [-v]
idfc flash [-t ...]      # ⚠ 只警告、不重建：改了配置要先 build
idfc monitor [--wait-for <regex>] [--grep <regex>] [--timeout ms] [--no-reset]
idfc size [-t ...] [--detailed] ; idfc run --monitor-wait-for "..."
idfc idfpy <任意 idf.py 参数...>   # 逃生舱，用前先读 IDFC_USAGE.md
idfc menuconfig search "关键词" / desc <选项名>   # 纯客户端查 Kconfig（需先 build 过）
```

- 💡 执行回显里那行 `# Executing idfc idfpy -B build\<cell> -D SDKCONFIG_DEFAULTS=... -D SDKCONFIG=... <子命令>` 就是本次实际命令；配置矩阵只剩 `variant`，构建目录 = `build/<variant>`。
- 🚨 **PATH 里找不到 `idfc.exe` 时**：不要去找/不要用旧版顶替，**提醒用户下载**（上面的发布页）并放到 PATH 目录，然后重试。**绝不能把 idfc.exe 放进工作区**——工作区带 Low 完整性标签，`danger-full-access` 也救不了。
- 🚨 报 `IDF 服务器可能尚未运行（退出码 -1）` = 服务器没起：提示用户 `idfc start-server`。
- ⚠ `idfc.exe` 必须**常驻的服务器**（由用户启动），不要自己 `start-server`（沙箱内会拒绝）。

**5. 开发命令**

```powershell
# debug（性能监视 + FPS/日志全开 + 2bit 封面）—— 默认
idfc build ; idfc flash ; idfc monitor --wait-for "App started"
idfc build --reconfigure        # 改了 sdkconfig.defaults* / Kconfig / idfc_buildconfig.json 之后

# release（调试全关 + 极速版封面）
idfc build -t release ; idfc flash -t release -p COMn

# sync（WiFi对时专用固件，烧 ota_0 @0x310000；见 DUAL_APP_TIME_SYNC.md）
idfc build -t sync              # 输出 build\sync\FakePodNanoNightPlayer.bin，必须 ≤2.5MB
# 🚨 绝不能用 idfc flash -t sync（会把对时固件写到 factory 覆盖播放器）！只能：
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\esptool.exe' --chip esp32s31 -p COM3 `
    --before default-reset --after hard-reset write-flash 0x310000 `
    build\sync\FakePodNanoNightPlayer.bin
```

其它：`idfc run -t debug --monitor-wait-for "App started"`、`idfc size -t debug`、`idfc clean|fullclean -t debug`。

- **过期检测已自动化**：改了 `sdkconfig.defaults*` / `idfc_buildconfig.json` / `main/Kconfig.projbuild` 后 `build` 会自己清理+重配；`--reconfigure` 强制，`--no-reconfigure` 纯增量，`--fullclean` 从零。
- ⚠ 原理仍要知道：**已存在的 `build/<variant>/sdkconfig` 优先级高于 defaults**（`replace=False` 合并），普通增量构建时新配置**不生效也不报错**。手工绕过 `build` 直接用 `idfpy -B ...` 时必须自己注意。
- 串口优先级 `-p` > `.idfc` 里 `set-port`；构建类型 `-t` > `set-variant`（当前 debug）。**这两个是隐式状态，不确定先 `idfc status`。**
- 默认构建类型 = idfc 当前选择的类型（除非用户指定）。

**6. 构建配置**

- 事实源只有三处（都入库）：`sdkconfig.defaults*` + `idfc_buildconfig.json` + `main/Kconfig.projbuild`。生成的根 `sdkconfig`、`build/<variant>/sdkconfig` 被 gitignore。
- `sdkconfig.defaults`（+ `.debug` / `.release` / `.sync`）：esp32s31、QIO 16MB、自定义分区表、16MB Octal PSRAM 200MHz（含 XIP/RODATA）、320MHz、64B 缓存线、FreeRTOS 1000Hz、FATFS cp936+UTF-8、`CONFIG_UI_COVER_FLASH=y`（release 的极速封面）。
- **LVGL 不可改的关键项**：`LV_USE_CUSTOM_MALLOC=y`（对象堆进 PSRAM，`core/ui/lvgl_mem.c`；改回 CLIB 会因内部 RAM 不足直接崩，理由见 `LVGL9_MIGRATION.md` §10）、`LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`、`LV_FONT_FMT_TXT_LARGE=y`（NPF 的 glyph_dsc 是 LARGE 布局）、`LV_DEF_REFR_PERIOD=16`、`LV_USE_SYSMON=y` + `LV_USE_PERF_MONITOR=y`（v9 起性能监视器挂在 SYSMON 下）、裁掉 6 种用不到的颜色格式（只留 RGB565/ARGB8888/RGB565A8/RGB888）、关 `LV_BUILD_EXAMPLES`/`LV_BUILD_DEMOS`、`CONFIG_LV_USE_PPA=n`（改为自定义 SW blend handler `core/ui/lvgl_ppa_accel.c`，见 `PPA_ACCEL.md`）。逐项理由与实测数据见 `BUILD_CONFIG.md`、`IRAM_AUDIT_LVGL9.md` §5、`PPA_ACCEL.md`。
- **配置矩阵只有 `variant` 一个维度，共 3 个值**：`debug` / `release` / `sync`（WiFi对时固件，烧 ota_0）。构建目录 `build/<variant>`，配置链 `sdkconfig.defaults;sdkconfig.defaults.<variant>`。`-o`/`set-overlay` 已不存在（overlay 维度已删）。
- ⚠ **分区表（`partitions.csv`，偏移写死）**：`nvs`@0x9000(0x6000) / `phy_init`@0xF000(0x1000) / `factory`@**0x10000(3M)**（播放器）/ `ota_0`@**0x310000(2.5M)**（对时固件）/ `otadata`@**0x590000(8K)** / `font`@0x600000(6M) / `pldata`@0xC00000(4M)。bootloader@0x2000 与分区表@0x8000 由 sdkconfig 控制（不在 CSV 里）。**四条原则**：① **`pldata` 必须留最末尾**——偏移一变旧播放列表作废、首次开机会重扫（锚定尾部后调 font/app 都不动它）；② `font`/`pldata` 偏移按 64KB 对齐（才能整段 mmap）；③ **app 分区偏移也必须 64KB 对齐**（所以 otadata 放两个 app 之后，否则白丢 64KB）；④ **播放器必须装进 factory(3MB)、对时固件必须装进 ota_0(2.5MB)**——`check_sizes` 只在"所有 app 分区都装不下"时报错，部分装不下只打 Warning（release 播放器 2.66MB 超出 ota_0 属预期）。**nvs/phy_init/font/pldata 的偏移不要再动**；改过 pldata 偏移的两次重排都导致播放列表重扫（历史见 `partitions.csv` 头部注释）。
- 字体与构建类型无关（都读同一份 `spiffs/system.npf`）。

### 项目内容

**FakePodNanoNightPlayer —— FakePodNano 播放器的第三方固件**：为成品开源播放器 **FakePodNano Pro**（S3 初代见 `legacy_s3`）做更好用、专注音乐播放的替代固件。

**当前实现状态**（细节见各设计文档与源码注释）：

- 播放核心：MP3（Helix，实测 5000+ 首）、文件夹与 `.pl` 歌单、循环（全部/列表/单曲）、顺序/**无限确定性随机**（断点续播）、回放增益、**10 段 EQ**、`.npd` 元数据 + **硬件 JPEG 封面**（失败回退软件解码）。
- **经典蓝牙音频**：A2DP Source（SBC，48k 优先/44.1k 兜底）+ AVRCP CT 绝对音量；EQ 在重采样之后；最多 5 设备各自记忆音量/EQ/安静模式；控制中心开关（长按快速面板）、设置菜单蓝牙页（配对/详情）；已配对耳机可主动回连；安静模式（只留蓝牙的伪关机）。见 `BLUETOOTH_AUDIO.md`。
- 硬件层：AMOLED（QSPI，全黑启动防白屏）、触摸（CST820，异步初始化）、电池（AXP2101 融合百分比）、音频（I2S + CS43131）、SD（FAT/exFAT）、**RTC 走时 + 双键映射 + PMU 关机/低电保护**。
- 任务布局：UI（LVGL）固定 Core0、解码/封面固定 Core1，分核不抢占（见"使用技术"）。
- LVGL 移植层：显示驱动、**tick 直接读硬件时间**、专用任务（Core0）、互斥锁、触摸 indev、通用虚拟列表、全局跑马灯节奏、deep_worker 大栈任务。
- 开机流程：开机封面 → 低电拒启 → 未插卡 / 列表无效则一键扫描 → 主界面；**设置菜单懒加载**，不占开机路径。
- UI：四页滑动 tab（目录/歌曲/正在播放/播放设置）、顶部状态栏（时间/页面名/电量+充电图标）、下拉控制中心（亮度/音量竖条、蓝牙、睡眠定时、设置入口）。
- **WiFi 对时（双固件）**：设置 → 时间与日期 →「WiFi对时」→ 确认 → 切到 `ota_0` 的对时固件 → 连 AP + SNTP → 写 PCF85063A → 重启回播放器 + Toast 结果。**播放器固件不含 WiFi 协议栈**。见 `DUAL_APP_TIME_SYNC.md`。
- 工具：双核性能监视（串口每 2s）、开发者选项（触摸测试白点，NVS 运行时开关）、全局 Toast。

> `FUTURE_PLAN.md` / `DONE_PLANS.md` 只记较大型需求与修复；只有 `FUTURE_PLAN.md` 里出现过的内容才能移进 `DONE_PLANS.md`。日常小改动不必记录；踩坑变大研究的单开文件（如 `IRAM_DMA_AUDIT.md`）。

### 代码结构

```
main/
  main.c                  # app_main 只做固件角色分派：播放器 player_app_main() / 对时固件 sync_app_run()
  Kconfig.projbuild       # 应用级 Kconfig（性能监视、内存快照、封面、PPA、LVGL 热点对象、PMU 按键/充电、RTC、固件角色、版本号）
  CMakeLists.txt          # 源文件按角色分：MAIN_SRCS_SHARED / _PLAYER / _SYNC（+ 托管组件构建期补丁）
  core/system/            # fw_switch（otadata 切换 + 跨固件结果，NVS "tsync"）/ sync_app（对时固件最小启动流程）
  core/hardware/          # display touch axp2101 boot_reason rtc battery button_service sd_card pldata
    audio/                # audio(I2S+DAC+输出路由) cs43131 mp3_player eq bt_audio resampler
    bt/bt_service.c       # 经典蓝牙：控制器/Bluedroid/A2DP/AVRCP + bt_svc 命令任务
  core/storage/           # config_store(+配置槽位) bt_store playlist(+_store/_builder/_format) config_slots
  core/player/            # player_controller / shuffle_player
  core/music/song_meta.c  core/image/cover_jpeg.c + default_cover(_flash).c
  core/network/           # wifi_time_sync（只编进对时固件）
  core/fonts/             # system_font / font_npf（mmap 全字库）/ system_global_font_lite / system_smaller_font
  core/ui/                # lvgl_port lvgl_mem lvgl_ppa_accel lvgl_ppa_rotate touch_test virtual_list click_wheel global_style
  core/utils/             # performance_monitor fake_shutdown sleep_timer power_service quiet_mode deep_worker task_psram
  ui/                     # app main_ui boot_flow dir_list playlist_page now_playing settings_page settings_menu
                          # time_date_popup time_sync_ui notification_panel bt_settings bt_quick_panel key_handler
                          # player_ticker eq_popup config_slot_popup toast list_common clock_display clock_text battery_indicator
components/               # esp_lcd_qspi_amoled、esp_lcd_touch_cst816/820/chsc6417、spi_flash（IDF fork）
patches/                  # lvgl_fast_mem_decl / esp_lcd_touch_deps（托管组件构建期补丁）
```

### GPIO 分配（随进度持续发掘，新增外设前先核对本表）

| GPIO | 功能 | 说明 |
|---|---|---|
| 2 / 6 | PDM 麦克风 DATA / CLK | 未启用（`hardware_v1.md` §7） |
| 20 / 21 / 22 / 23 | SD D0~D3 | SDMMC 4-bit |
| 24 / 25 | SD CLK / CMD | |
| 35 | LCD TE | 未使用，保留定义 |
| 36 / 37 / 38 | LCD RST / CS / PCLK | |
| 39 / 40 / 42 / 43 | LCD DATA0~3 | QSPI 四线 |
| 44 | LCD VCI_EN | 高有效，**初始化前必须先拉高** |
| 45 / 46 | 触摸 INT / RST | 低有效 |
| 47 / 48 | I²C0 SCL / SDA | 触摸 / AXP2101 / PCF85063 / QMI8658 共用 |
| 49 | AXP2101 INT | 开漏低有效；电源键短按/长按/按下/抬起（`CONFIG_PMU_KEY_IRQ`） |
| 50 / 51 / 52 | I²S1 DIN / WS / BCK | CS43131 |
| 53 | CS43131 RESET | 低有效 |
| 54 / 55 | I²C1 SCL / SDA | CS43131 专用 |
| 61 | 下方独立按键 | 低电平按下，内部上拉 |

**上键 = AXP2101 电源键**（不占主控 GPIO；PMU 中断经 GPIO49 维护按住状态）。**历史 S3 引脚已作废**，不要再用。
**I²C 设备（开机扫描）**：I²C0 = 0x15 CST820 / 0x34 AXP2101 / 0x51 PCF85063 / 0x6B QMI8658；I²C1 = 0x30 CS43131。
> ⚠ 日志里偶尔的 `0x7E` **不是芯片**：是某颗已有芯片（最可能 QMI8658）对 I²C 保留地址的假应答。`i2c_scan()` 只扫 `0x01~0x77`，故日志只剩上面 5 条。见 `I2C_0X7E_INVESTIGATION.md`。

### 使用技术（架构总览）

> 各模块接口/实现/踩坑都在对应源码头文件与 `ideas_and_exps/` 文档里；这里只留跨模块的**规则**。

**1. 分层与初始化顺序**

- `core/` 放与应用无关的底层（hardware / ui / utils / player / storage / music / image / fonts / network / system），`ui/` 放应用界面。新文件按此分层，snake_case + 模块前缀。
- `main.c`（播放器分支）只做装配：`config_store_init → config_store_load_active_slot → deep_worker_init → display_init → lvgl_port_init → system_font_init → boot_splash_show(持锁首帧+点亮) → touch_init(+ready 回调) → battery_init → boot_reason_init → 充电界面判定 → rtc_init → audio_init/音量/EQ → bt_service_init → pldata_init → button_service_init → sd_card_init → boot_flow_run(持锁切屏+复核亮度) → 读对时结果 Toast → sleep_timer/quiet_mode → key_handler → power_service → startPerformanceMonitor()`。
- ⚠ **LVGL 故意在 display 之后立刻启动**（只依赖面板句柄 + config_store，触摸句柄可 NULL 后补）：先把开机封面画出来点亮，把后续硬件与 `app_ui_create`（约 700ms）藏到封面背后——首次点亮从约 1.9s 提前到约 0.7s，进主界面时刻不变。防白屏顺序（亮度 0 时先写满整屏 → 再置亮度）与"亮度命令必须与刷屏同锁"的约定不要改。

**2. 任务核心布局（UI Core0 / 音频 Core1）**

- Core0：`LVGL`（每帧刷屏）、`button_svc`、`deep_worker`、`player_ctrl`、`cpu_coll0`、系统 esp_timer 任务/ISR、全部驱动中断。
- Core1：`mp3_play`/`mp3_out`、`cover_load`（注册为 lvgl_port 辅助任务，息屏随 LVGL 挂起）、`sleep_timer`、`power_svc`、`cpu_coll1`、`bt_svc` + 蓝牙控制器/Bluedroid 任务；`boot_scan`/`rescan`。
- 不固定核：`touch_init`（一次性自删）、`mem_monitor`/`cpu_coll*`、`pmu_irq`。
- **⚠ 非实时任务栈一律用 `task_create_psram_stack()`**（失败自动回退内部 RAM）：内部 RAM 开机末期只剩几 KB、最大连续块约 8KB，内部栈会创建失败（历史症状：`create power service task failed` → 自动息屏/低电关机全失效）。已在 PSRAM：`mem_monitor`/`cpu_coll0/1`、`rescan`、`dev_restart`、`cover_load`、`power_svc`、`touch_init`、`pmu_irq`。**必须留内部 RAM**：`LVGL`、`mp3_play`/`mp3_out`、`button_svc`、`player_ctrl`、`deep_worker`、`sleep_timer`、`boot_scan`。
- **栈预算**：`LVGL` **12KB**（v9 绘制管线很深：6KB 时"首次渲染设置页"必爆栈；凡可能同步渲染 LVGL 的任务都按 ≥12KB 算）、`deep_worker` **12KB**、主任务 8KB（`CONFIG_ESP_MAIN_TASK_STACK_SIZE`）、button_svc 4KB、power_svc 3KB、sleep_timer 4KB、cover_load 6KB、mp3_play 8K/out 4K、player_ctrl 4KB。⚠ 调栈前看 `MEM: stk:` 高水位，且**必须覆盖目标路径实测**（如息屏/亮屏后才打印的读数），别只信亮屏态。
- **deep_worker**：息屏/亮屏、关机、以及**需要内部栈的关缓存段**（如重扫的 munmap/擦头）唯一执行者，业务任务用 `deep_worker_call(fn, arg)` 投递。约定：fn 内禁止再投递；**调用方禁止持 LVGL 锁投递**。⚠ **必须在 `app_main` 早期创建**（config_store 之后、硬件初始化之前）——内部 RAM 在开机末期碎片化，晚创建会静默失败（曾导致每次深调用等满 5s 超时）；创建处必须检查返回值。

**3. 内部 RAM / IRAM / DMA 的三条硬规则**

- **IRAM 与内部 DRAM 是同一块 SRAM 的两个别名**：代码进 IRAM 等于从内部 RAM 堆里扣（S3/S31 皆然）。所以"把东西搬去 PSRAM"不见得能涨可用内部 RAM，要看 IRAM 代码量。
- **LVGL 绘制缓冲是最大的 DMA 消费者**：`EXAMPLE_LVGL_BUF_HEIGHT = V_RES/10`（双缓冲 ≈84KB；PPA 旋转 tile 同尺寸，等于要 3 块 41KB），并做成 `/10 → /12 → /16` **逐级回退**（失败只降档不 assert）。⚠ 缓冲**必须** `MALLOC_CAP_DMA`（否则触发 SPI 驱动 bounce 缓冲，失败就刷屏阻塞→WDT）。**调这个值前同时看三项：帧率 + DMA 余量 + 内部 RAM 最大连续块（须 ≥4KB）**；DMA 不足的典型症状是 `sdmmc_cmd: allocate_dma_buf: not enough mem` → 切歌被误判"坏曲"跳过。
- **⚠ 堆毒化必须保持关闭**（`CONFIG_HEAP_POISONING_DISABLED=y`）：Comprehensive 多占约 26KB 内部 RAM 且把最大连续块压到 14.8KB，是"debug 才有内部 RAM 问题"的根因。抓堆破坏时才临时打开。历史实测与失败链条见 `IRAM_DMA_AUDIT.md`、`IRAM_AUDIT_LVGL9.md`、`IRAM_OBJECTS_PSRAM_S31.md`、`PSRAM_TASK_STACK.md`。
- 当前实测基线（debug，BT 关）：播放器 `iram≈110KB`、`fps≈61~63`；BT 开连耳机播放中 `iram≈58KB`、`fps≈57~62`。

**4. 关缓存 / PSRAM 栈（硬约束）**

- `esp_partition_mmap/munmap`、`esp_flash_erase/write`、`pldata_write/erase` 都会冻结外部缓存 → 此时 **PSRAM 栈任务不许执行**（IDF 有 `s_task_stack_is_sane_when_cache_frozen` 断言）。这类代码要么让任务栈留内部 RAM，要么 `deep_worker_call()` 投递。
- **写 flash 期间不要持有 IRAM ISR 会碰的 PSRAM 数据**：IRAM ISR 不许碰任何 PSRAM 数据、`lv_*`、播放列表/字体 mmap（无断言，靠纪律；LVGL 对象进 PSRAM 后这条从"不规范"升级为"必崩"）。
- 全项目 NVS 写入口唯一（`config_store.c`），高频拖动必须走 `*_deferred` + `config_store_flush_pending()`（一次 nvs_commit 会停核 5~20ms）。详见 `FLASH_WRITE_VS_PSRAM.md`、`NVS_WRITE_BUDGET.md`。

**5. 息屏/亮屏与低电保护**

- 息屏（经 deep_worker）：AMOLED SLEEP IN → `lvgl_port_suspend` → 固定 80MHz。亮屏：固定 320MHz → 持锁 `display_sleep_exit`（SLEEP OUT + 120ms + 重发完整 init + 全屏重刷）→ 恢复亮度 → `lvgl_port_resume`（**必须在 LVGL 恢复前**）。
- **低电保护（S31，按电量）**：开机 <2% 且未充电 → 提示 5 秒后 PMU 断电；使用中 <5% 未充电持续 60s → `fake_shutdown_enter()`。充电中/读数不可用（0xFF）不动作。S3 的按电压判据与"息屏暂停超时关机"已删除。
- ⚠ **"息屏停掉 Core1"（esp_cpu_stall）方案已彻底移除**（长期 stall 与被停核的 PSRAM/XIP 取指形成总线竞态 → INT WDT），**不要再引入**。

### 关键技术要点与踩坑（务必遵守）

- ⚠⚠ **LVGL tick 必须用 `lv_tick_set_cb()` 直接读硬件时间，绝不要用 esp_timer 周期回调喂 `lv_tick_inc()`**：IDF 6.1 的 esp_timer 有任务/中断两条派发通道，混用会丢任务派发通道的闹钟 → 该通道定时器**永不到期**（本项目 `performance_monitor` 正好有个 ISR 派发定时器）。曾表现为**整屏定格、日志无异常、按键音频正常、息屏再亮屏才"恢复"**。两个细节：① `lv_tick_set_cb()` 必须在 `lv_init()` **之后**调（`lv_init` 会 `lv_memzero` 整个 `lv_global_t`）；② 真要周期回调，用任务 + `vTaskDelay` 或极短的 `ESP_TIMER_ISR` + `IRAM_ATTR`。见 `DISPLAY_FREEZE_TRIAGE.md`。
- **partial 刷屏的两个绘制层事实**：① **每层不透明整块背景 = 每个绘制块多填一遍 2 万像素**。现在 `scr`/`tabview`/tab 页全透明，黑底只由 `main_ui` 的 tabview 内容容器提供；新页面不要再铺底色，但新建**全屏覆盖层**（设置菜单/开机流程/关机提示）必须自己铺不透明底色，否则会露旧帧残影。② 卡片背景是"圆角帽 + 直角主体"两段（视觉等价整卡圆角，但列表区走 `radius==0` 快速填充），改圆角/高度要两段一起核对。
- **图像像素缓冲必须 64B 对齐**（`LV_DRAW_BUF_ALIGN=64`）：`lv_image_dsc_t.data` 不是 64 的倍数时**每次**刷 `W lv_draw_buf_init: Data is not aligned`，115200 波特下直接把 UI 卡死（现象："切歌后变卡、只有带图片的页面卡"）。封面已用 `heap_caps_aligned_alloc(64, …)`，新增动态图像/图层缓冲照此办理。
- **不要用高频 ESP_LOG 做性能统计**：115200 下一条约 10ms 且同步阻塞，15 条/秒就白扔 15% CPU。测帧率先关 `CONFIG_UI_PPA_STATS` 之类。
- **QSPI 像素时钟保持 40MHz**（`AMOLED_QSPI_MAX_PCLK`）：一帧 423KB，列表滚动每帧约 340KB → 光传输就 >21ms，这是帧率带宽天花板。⚠ 80MHz 会留下"偶发 1px 纯黑竖线"（随对象变宽累积），**不要再调回去**；排查画面残留先退回 40MHz 做对照。
- **代码注释一律中文**；snake_case + 模块前缀；关键魔法数字要有注释。
- **LVGL 线程安全**：非 LVGL 任务改 UI 必须 `lvgl_port_lock(-1)` / `unlock`；LVGL 任务回调内已持锁。**config_store 的 `set_xxx` 只置"待通知"标志**，回调由 `ui/app.c` 的 100ms lv_timer（`config_store_notify_dispatch()`）在 LVGL 任务里统一派发——⚠ **不要改回同步执行**（`player_ctrl` 会调 set_xxx，在非 LVGL 任务里改对象树会卡死 `lv_inv_area` 并触发 task_wdt，见 `CONFIG_SLOTS.md` §7）。
- **改完即烧录**：首次烧录若未指定串口就先列串口并询问用户；后续改完直接 build + flash + 串口验证。
- **新模块记得更新 `main/CMakeLists.txt`**（按角色放进 SHARED / PLAYER / SYNC），新增 `#include` 的 IDF 组件记得加白名单。
- **⚠ 禁止用 shell 文本改写工具编辑源文件**（真实事故）：`Get-Content -Raw | -replace | Set-Content` 的编码往返曾把 `settings_menu.c` 里 163 行中文变成 U+FFFD。改文件**只用文件编辑工具**（`edit`/`write`），不要用 `Set-Content`/`Out-File`/`>`/`-replace` 管道改源码。

## 用户背景介绍

### 用户背景

- 用户对嵌入式开发有一定经验（此前完成过 ESP32-P4 时钟项目 OtoClock），代码主要由 AI 协作编写。
- 用户有多年 Java/C# 等高级语言经验，擅长设计美观易用的界面，能读懂并修改 C 代码，但不写 CMake。
- 项目是 DIY 开源项目，注重软硬件自由：固件源码开放、鼓励自行修改；不应对固件数据/配置设不必要的限制。
- GPIO 用途随进度逐渐发掘（见上表），新确认的分配请同步更新文档。

### 用户对代码的要求

- 遵循已建立的分层与命名习惯（`core/` 底层 vs 数据层 vs `ui/` 应用、snake_case、模块前缀），新增文件同时更新 `main/CMakeLists.txt`。
- 注释中文、逻辑清晰、关键常量与魔法数字要解释。
- UI 功能按模块实现、逐个真机验证，不要一次性堆砌。
- 用 Kconfig（`main/Kconfig.projbuild`）控制可裁剪功能，但 debug / release 两套配置已覆盖大多数场景。
- 过程文档/组件文档放 `ideas_and_exps/`，新模块要同步更新对应设计文档。

### 回复/行动说明

- 推理/思考及回复均使用中文。
- 若 IDF 服务器未运行（`idfc` 报 `IDF 服务器可能尚未运行` / 退出码 -1），提示用户手动 `idfc start-server`（普通权限即可；控制台窗口关掉即停）。
