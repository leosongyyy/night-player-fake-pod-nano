## FakePodNanoNightPlayer 项目概览

### 环境说明

**1. 硬件**

本项目是基于 ESP32-S3 成品播放器（开源项目 **FakePodNano** 的硬件）开发的第三方固件。设备通过 USB 串口（COM?，USB-Serial/JTAG 模式）连接，芯片参数：

- 芯片：ESP32-S3 (QFN56) rev v0.2，WiFi/BLE，240MHz 双核
- 存储：8MB Octal PSRAM（AP_3v3）、16MB Flash（QIO，80MHz）
- 屏幕：鱼鹰光电 AMOLED，**460x460**（型号 AM200Q460460LK，QSPI 接口），驱动 `esp_lcd_qspi_amoled`
- 触摸：**CST816 / CST820 / CHSC6417**（按屏幕自动选择），驱动对应 `esp_lcd_touch_cst816` / `esp_lcd_touch_cst820` / `esp_lcd_touch_chsc6417`
- 电源：电池电压经 10K+10K 分压到 GPIO1（ADC1_CH0）
- 耳放芯片：CS43131，I2C控制端口在0x30，通过I2S写入音频

**2. 开发环境**

- ESP-IDF **5.5.X**（安装于 `C:\esp\v5.5.X\esp-idf`，本机已验证 5.5.4 与 5.5.5），激活脚本 `C:\Espressif\tools\Microsoft.v5.5.X.PowerShell_profile.ps1`。不同的开发电脑上可能是5.5.4也可能是5.5.5。这里统一使用idfc.exe进行调用。
- LVGL **8.4.0**（managed component，`lvgl/lvgl`）
- 构建/烧录通过项目自带的 **`idfc.exe`**（客户端-服务器架构）完成，详见下方"idfc.exe 使用说明"。

**3. 参考资料**

- `components/`：本地组件（esp_lcd_qspi_amoled AMOLED 驱动、esp_lcd_touch_cst816/820/chsc6417 触摸驱动、i2c_bsp）
- `managed_components/`：ESP 组件管理器拉取的依赖（lvgl、esp_lcd_sh8601、esp_lcd_touch、cmake_utilities、chmorgan__esp-libhelix-mp3）
- `IDFC_USAGE.md`（`ideas_and_exps/`）：idfc.exe 的完整使用说明（架构、启动服务器、常见问题排查）
- `BUILD_CONFIG.md`（`ideas_and_exps/`）：三套构建配置（debug/releasecandidate/release）的完整说明

**4. idfc.exe 使用说明（重要）**

`idfc.exe` 是 ESP-IDF 命令行的**服务器 + 客户端**封装：服务器一次性激活 IDF 环境并常驻监听 `http://localhost:19781/idf/`（HTTP + SSE 流式输出），客户端把 `idf.py` 命令发给服务器执行。

- **启动服务器**（需要 `danger-full-access` 权限，否则 HTTP.sys 端口绑定会失败）：
  ```powershell
  .\idfc.exe --server --idfenv "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"   # 版本号按本机安装（5.5.4 或 5.5.5）
  ```
  也可以让用户直接运行项目根目录的 `StartIDFCServer.vbs`。

- **执行命令**（客户端，在 `workspace-write` 权限下运行）：
  ```powershell
  .\idfc.exe build              # 构建
  .\idfc.exe -p COMx flash      # 烧录（端口仅为示例，不同电脑上的配置不同）
  .\idfc.exe -p COMx monitor    # 串口监视
  ```
- **⚠ 若提示 `IDF server is not running at http://localhost:19781/idf.`**：说明服务器未运行或被回收，需要**提示用户手动启动服务器**（运行 `StartIDFCServer.vbs` 或上述 `--server` 命令），不要自行尝试在受限权限下重建服务器。服务器是常驻后台进程，会话结束后可能被回收，每次遇到连接错误都应先检查/提醒启动服务器。
- 服务器由 `idfc.exe` 内部派生的 `--server-child` 进程承载，用 `pwsh` 前台启动更稳定。

**5. 开发命令**

本项目使用多配置构建系统（debug / releasecandidate / release 三套，详见 `ideas_and_exps/BUILD_CONFIG.md`），以下是不同配置的命令速查表：

** debug（精简字体 + 非极速封面 + 调试全开） **

```powershell
# 增量构建（首次需带 -D 配置，见下）
.\idfc.exe -B build/debug build

# 重新配置（改过 sdkconfig.defaults* 后，先删 build/debug/sdkconfig 再执行）
.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" reconfigure

# 烧录
.\idfc.exe -B build/debug -p COMn flash
```

** releasecandidate（完整字体 + 极速封面 + 调试全开） **

```powershell
.\idfc.exe -B build/releasecandidate build
.\idfc.exe -B build/releasecandidate -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.releasecandidate" -D SDKCONFIG="build/releasecandidate/sdkconfig" reconfigure
.\idfc.exe -B build/releasecandidate -p COMn flash
```

** release（完整字体 + 极速封面 + 调试全关） **

```powershell
.\idfc.exe -B build/release build
.\idfc.exe -B build/release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" -D SDKCONFIG="build/release/sdkconfig" reconfigure
.\idfc.exe -B build/release -p COMn flash
```

**备注**：只有首次或"改过 `sdkconfig.defaults.*`"才需要带 `-D` 重新配置；日常增量构建和烧录只带 `-B` 即可。⚠ 已有 sdkconfig 的优先级高于 defaults（`replace=False` 合并），改了 defaults 后仅 reconfigure 不会生效，**必须先删对应 `build/<cfg>/sdkconfig` 再带 `-D` reconfigure**。烧录前记得确认串口号。
默认情况下，构建调试版的，除非用户指定。

- `idfc -p COMx monitor`：查看串口日志（退出 Ctrl+C或者直接结束任务）。
- 已开放 `C:\esp\` 与 `C:\Espressif\` 的读取权限，可查阅 IDF 源码。

**6. 构建配置**

- `main/Kconfig.projbuild`：应用配置项（menuconfig）。配置项目请阅读该文件获得配置项的详细信息。常用配置组合均已使用三套构建配置切换。
- `sdkconfig.defaults`（+ `sdkconfig.defaults.debug` / `.releasecandidate` / `.release`）：esp32s3、QIO 16MB Flash、自定义分区表、8MB Octal PSRAM（含 XIP 指令/只读数据）、240MHz、64B 数据缓存线、FreeRTOS 1000Hz、LVGL 刷新 33ms / indev 轮询 33ms、开启 FreeRTOS 运行时统计（`GENERATE_RUN_TIME_STATS`，performance_monitor 依赖）、LVGL 字体（12/16/20/24/28/32/48，默认 18）、**`CONFIG_LV_USE_LARGE_COORD=y`**（lv_coord_t 升为 int32_t，600+ 歌曲列表必需，见 virtual_list）、FATFS codepage 936 + UTF-8、`CONFIG_UI_COVER_FLASH=y`（公共默认即极速版封面）、`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` 等
- **版本管理：配置事实源只有 `sdkconfig.defaults*` 与 `main/Kconfig.projbuild`（均纳入 git）**；生成的根 `sdkconfig` 与各 `build/<cfg>/sdkconfig` **已被 `.gitignore` 忽略，不入库**。多配置构建下每个 build 目录持有自己的 sdkconfig（`-D SDKCONFIG=build/<cfg>/sdkconfig` 指定）；`fullclean` 只清 `build/`，不会删 sdkconfig。改配置的正确姿势：改 `sdkconfig.defaults*` 或 `Kconfig.projbuild` 后，按第 5 节命令删对应 `build/<cfg>/sdkconfig` 再带 `-D` reconfigure。
- `partitions.csv`（**显式静态布局**，偏移写死不随自动分配漂移）：bootloader@0x0 / 分区表@0x8000（两者由 sdkconfig 默认 `BOOTLOADER_OFFSET_IN_FLASH`、`PARTITION_TABLE_OFFSET` 控制，不在 CSV 内）→ `nvs`@0x9000(0x6000) / `phy_init`@0xf000(0x1000) / `factory`(app，6M)@0x10000 / `pldata`(data 0x40，4M)@0x610000（播放列表数据结构分区，内存映射只读访问）。
- 调试版的字体可能不全，请无视这一点，优先调试为主。实际效果会在发布版确认。用户会不定期更新调试版的字库。

### 项目内容

**FakePodNanoNightPlayer —— FakePodNano 播放器的第三方固件**

> 本项目目标：为成品开源播放器 **FakePodNano**（ESP32-S3 + AMOLED + CS43131 + 触摸）开发**更好用、专注于音乐播放**的第三方固件，替代/改进原厂固件的使用体验。

**项目现状**：核心音乐播放体验已**基本完整**——本地 MP3 按文件夹/歌单浏览、四页滑动 UI、全屏封面正在播放页、下拉控制中心、睡眠定时、EQ、随机播放、记忆播放、自动关机保护均已实现。剩余功能/规划见 `ideas_and_exps/FUTURE_PLAN.md`（可能实现也可能被砍掉）。

**当前实现状态：**
- 播放核心：本地 MP3 播放（Helix 解码，实测 5000+ 首大曲库），按**文件夹**组织、支持 **`.pl` 文本歌单**；**循环模式**（全部/列表/单曲）+ **顺序/随机**；**无限确定性随机播放**（分块 + 确定性洗牌，断点续播）；回放增益；**EQ 均衡器**（10 段浮点 PeakEQ + 预增益，NightPlayer 兼容 `.eq` 文件）；**封面/歌曲信息**（`.npd` 元数据 + JPEG 封面解码）
- 硬件层：AMOLED 显示（QSPI，支持 6 种屏幕尺寸配置切换，**全黑启动防开机白屏闪烁**）、触摸（CST816/820/CHSC6417 按屏幕自动选择，**异步初始化不阻塞开机**）、电池电压 ADC、音频（I2S 主机 + CS43131 DAC + 回放增益 + EQ）、SD 卡（SDMMC 4-bit + FAT，挂载 `/sdcard`）
- **任务核心布局：UI（LVGL）固定 Core0、播放解码（MP3/封面）固定 Core1，二者分核互不抢占（见第 6 节）**
- LVGL 移植层：显示驱动、2ms tick、专用 LVGL 任务（固定 Core0）、互斥锁、触摸 indev（33ms，x/y 坐标 ×29/30 校准）、触摸事件订阅回调、辅助 UI 任务注册（随 LVGL 挂起/恢复）、**通用虚拟列表组件**（virtual_list）；息屏/伪关机的深调用链统一委托 deep_worker 大栈任务（见第 6 节）
- 开机流程（boot_flow）：低电拒启（<3.45V 停充电警告屏）→ 未插卡提示 → 播放列表数据无效则一键扫描（实时进度）→ 正常进入主界面；音乐变动后可在设置菜单"重新扫描"
- UI：主界面（四页滑动 tab：目录/歌单列表 → 歌曲列表 → 正在播放 → 播放设置）、顶部状态栏（左=音量档位、中=当前页面名、右=电池电压）、下拉控制中心（电池/亮度竖条/音量竖条/睡眠定时开关/设置入口，**纯 LVGL 事件驱动**拖拽）
- 工具：双核 CPU/内存性能监视（串口每 2s 输出，Kconfig）、**开发者选项**（设置菜单 → 关于 → 开发者选项：电池电压日志开关/采样间隔、触摸区域测试白点开关，NVS 运行时配置**默认关**，开机检查、修改需重启生效）、全局 Toast 提示

### 代码结构

```
main/
  main.c                    # app_main：config_store → 硬件初始化 → LVGL 移植层 → boot_flow → 深链/睡眠/按键/电源/日志/性能监视
  CMakeLists.txt
  Kconfig.projbuild         # 构建级应用配置项（Kconfig；触摸测试/电池日志已移至运行时开关）
  core/
    hardware/               # 与应用无关的底层硬件驱动
      display.h/.c          # AMOLED LCD（QSPI 总线/面板）+ 屏幕尺寸配置 + 亮度控制
      touch.h/.c            # 触摸控制器（I2C + CST816/820/CHSC6417 按需选择，异步初始化）
      battery.h/.c          # 电池电压 ADC（分压换算 + 校准；BATTERY_CRITICAL_LOW_MV=3450）
      button_service.h/.c   # A/B 双键服务（GPIO48/GPIO0，去抖 + 单击/双按/长按事件）
      sd_card.h/.c          # SD 卡驱动（SDMMC 4-bit + FAT 挂载 /sdcard）
      pldata.h/.c           # 播放列表数据结构分区（4MB）驱动：esp_partition_mmap 内存映射只读
      audio/
        audio.h/.c          # 音频播放 API（I2S 主机 + CS43131 DAC + PCM 流写 + 回放增益 + 统一浮点路径）
        cs43131.h/.c        # CS43131 DAC 驱动（I2C 控制、24.576MHz 直通 MCLK、ASP 从机 I2S、音量）
        mp3_player.h/.c     # MP3 解码播放（Helix + 跳 ID3v2 + 写 I2S，Core1 后台任务）
        eq.h/.c             # EQ 均衡器（10 段浮点 PeakEQ + 预增益，.eq 文件解析/NVS 持久化/DSP 线程安全）
    player/                 # 播放引擎（应用层）
      player_controller.h/.c  # 播放控制器（Core0 独立后台服务：位置维护/播放控制/循环/随机/坏曲事件）
      shuffle_player.h/.c     # 无限确定性随机播放列表（分块 + 确定性洗牌）
    storage/                # 播放器数据持久化
      config_store.h/.c     # 用户配置存储（NVS：音量/亮度/循环/随机/睡眠/息屏超时/EQ/上次位置等）
      playlist.h/.c         # 播放列表只读解析（pldata 内存映射 → 目录/歌曲/字符串）
      playlist_store.h/.c   # 播放列表存储（pldata 分区"整体重写 + 头部 commit"语义）
      playlist_builder.h/.c # 播放列表构建器（SD 递归扫描 → PLST 格式 → 写 pldata）
      playlist_format.h     # PLST 二进制数据格式（目录区/歌曲区/字符串池，固定布局）
    music/
      song_meta.h/.c        # 歌曲元数据（.npd 文件解析：标题/艺术家-专辑/回放增益/封面）
    image/
      cover_jpeg.h/.c       # JPEG 封面解码（esp_jpeg/tjpgd → LVGL RGB565 img，后台任务）
      default_cover.h/.c    # 默认封面（2bit 索引色，省 Flash）
      default_cover_flash.c # 默认封面（RGB565 极速版，423KB，CONFIG_UI_COVER_FLASH）
    fonts/
      system_font.h         # 系统字体统一引用 UI_SYSTEM_FONT（随 CONFIG_UI_SYSTEM_FONT_FULL 切换）
      system_global_font.h/.c    # 完整版中文字库（约 2.4MB）
      system_global_font_lite.h/.c # 精简版（仅系统 UI 用字 + ASCII）
      system_smaller_font.h/.c   # 状态栏小字体
    ui/                     # 通用 UI 基础设施（与应用无关）
      lvgl_port.h/.c        # LVGL 移植层：显示驱动、tick、任务(Core0)、互斥锁、触摸 indev、触摸事件订阅
      touch_test.h/.c       # 触摸区域测试（白点跟随 + 淡出；运行时开关，开发者选项控制）
      virtual_list.h/.c     # 通用虚拟滚动列表（固定行槽复用，600+ 长列表可靠，依赖 LARGE_COORD）
    utils/
      performance_monitor.h/.c  # 双核 CPU/内存监视（从 OtoClock 移植，含 ISR 采样 CPU 频率）
      fake_shutdown.h/.c        # 伪关机（睡眠定时器到点；A 键长按"等待断电"，深链委托 deep_worker）
      sleep_timer.h/.c          # 睡眠定时器（到点音量渐降后触发普通伪关机；清醒感知加时）
      power_service.h/.c        # 电源服务（原 screen_sleep 改名）：息屏/亮屏 + 无操作自动息屏 + 自动关机策略
      battery_logger.h/.c       # 电池电压采样日志（续航测量用；开发者选项运行时控制）
      deep_worker.h/.c          # 深路径执行器（大栈任务）：息屏/亮屏/伪关机深调用链唯一执行者
  ui/                       # 应用 UI
    app.h/.c                # UI 组装入口 + 顶部状态栏（左音量/中页面名/右电池电压）
    boot_flow.h/.c          # 开机流程控制器（低电拒启/无卡/扫描/进主界面）
    main_ui.h/.c            # 主界面（四页滑动 tab：目录/歌曲/正在播放/播放设置）
    dir_list.h/.c           # 目录/歌单列表页（virtual_list，/sdcard 头部 + 行图标）
    playlist_page.h/.c      # 歌曲列表页（virtual_list，点击播放，当前曲目高亮）
    now_playing.h/.c        # 正在播放页（全屏封面 + 标题栏 + 控制栏/进度条，点击封面隐藏呼出）
    settings_page.h/.c      # 播放设置页（九宫格：循环/随机/睡眠定时）
    settings_menu.h/.c      # 全屏设置菜单（睡眠/息屏/按键帮助/关于+开发者选项/重新扫描）
    notification_panel.h/.c # 下拉控制中心（电池/亮度/音量竖条/睡眠开关/设置入口，LVGL 事件驱动）
    key_handler.h/.c        # 按键分发（亮屏：音量±/伪关机/息屏；息屏：播放控制/亮屏）
    player_ticker.h/.c      # 播放器状态轮询刷新 + 坏曲事件 toast 拉取
    eq_popup.h/.c           # EQ 预设选择弹窗（列出 .musicfx 下 .eq 文件）
    toast.h/.c              # 全局 Toast 提示（底部弹出 + 环形倒计时，顶层透传）
    list_common.h/.c        # 列表页公共小工具
components/
  esp_lcd_qspi_amoled/      # AMOLED 面板驱动（QSPI，含亮度 0x51 命令）
  esp_lcd_touch_cst816/     # CST816 触摸
  esp_lcd_touch_cst820/     # CST820 触摸
  esp_lcd_touch_chsc6417/   # CHSC6417 触摸
  i2c_bsp/                  # I2C 总线 BSP
```

### GPIO 分配（随项目进度持续发掘）

> ⚠ **重要**：本项目的 GPIO 用法是**随开发进度逐渐发掘和补充**的。当前已确认表中所列；其余（麦克风、IMU 等）在源码历史注释中有规划（见下方"历史注释规划"），**尚未验证启用**。每次新增外设功能时，先核对本表避免冲突，确认用法后更新本表。

**当前已确认使用的 GPIO：**

| GPIO | 功能 | 说明 |
|---|---|---|
| 0 | B 键 | 按键接 GND，外部 10kΩ 上拉（`core/hardware/button_service.c`）。⚠ strapping 引脚：复位时若被按住会进入下载模式。RTC GPIO，deepsleep 唤醒源：伪关机后按下可"取消关机"并唤醒重启 |
| 1 | 电池电压 ADC | ADC1_CH0，10K+10K 分压（`battery.c`） |
| 2 | 触摸 SCL | I²C 时钟（`touch.h`） |
| 3 | 触摸 SDA | I²C 数据（`touch.h`） |
| 4 | 触摸 INT | 触摸中断（`touch.h`） |
| 5 | 触摸 RST | 触摸复位（`touch.h`） |
| 7 | LCD RST | 屏幕复位（`display.h`） |
| 8 | LCD CS | QSPI 片选（`display.h`） |
| 9 | LCD PCLK | QSPI 时钟（`display.h`） |
| 10~13 | LCD DATA0~3 | QSPI 数据线（`display.h`） |
| 14 | SD DAT1 | SDMMC 数据线 1，10k 上拉（`core/hardware/sd_card.c`） |
| 15 | SD DAT0 | SDMMC 数据线 0，10k 上拉（`core/hardware/sd_card.c`） |
| 16 | SD CLK | SDMMC 时钟，直连 MCU（`core/hardware/sd_card.c`） |
| 17 | SD CMD | SDMMC 命令线，10k 上拉（`core/hardware/sd_card.c`） |
| 18 | SD DAT3 | SDMMC 数据线 3，10k 上拉（`core/hardware/sd_card.c`） |
| 21 | SD DAT2 | SDMMC 数据线 2，10k 上拉（`core/hardware/sd_card.c`） |
| 38 | 音频 LRCK/WS | I2S 帧时钟，主机输出（`core/hardware/audio/audio.c`） |
| 39 | 音频 DIN | I2S 数据输出（`core/hardware/audio/audio.c`） |
| 40 | 音频 BCK | I2S 位时钟，主机输出（`core/hardware/audio/audio.c`） |
| 41 | CS43131 RESET# | DAC 复位，低有效，经 3.3→1.8V 电平转换（`core/hardware/audio/cs43131.c`） |
| 48 | A 键（电源键） | 按键接 GND，内部上拉（`core/hardware/button_service.c`）。⚠ 与电源侧**3 秒关机芯片复用同一按键**：长按满 **1.2s** 固件伪关机（`fake_shutdown_wait_power_off`，保存配置+停外设+定格提示+deepsleep；阈值 `CONFIG_BUTTON_LONG_PRESS_MS`），继续按住满 3s 由硬件直接断电/开机。非 RTC GPIO，无法用于 deepsleep 唤醒 |

**历史注释规划（源码 `main.c` 旧版注释，当前未启用、未验证）：**
- LCD 相关：`TFT_TE = 6`（撕裂效果同步）
- I²C 总线：`I2C_WIRE_SDA = 3`、`I2C_WIRE_SCL = 2`（与触摸共用）
- 音频（I2S + CS43131 DAC）：`MCK=-1, BCK=40, WS=38, DO=39, CS43131_RST=41`（**已确认启用**，见 `core/hardware/audio/`；MCLK 由板载 24.576MHz 晶振提供，不占用 GPIO）
- 按键/静音：~~普通版 `BTN_PIN=47`、`AUDIO_MUTE=48`；HIFI 版 `BTN_PIN=48`~~（**已确认**：A 键=GPIO48、B 键=GPIO0，见 `button_service.c`）
- 麦克风（PDM）：`MIC_PDM_DATA=46, MIC_PDM_SCK=45`
- IMU 中断：`IMU_INT1=47`（HIFI 版）

**已确认的 I²C 设备（开机扫描）：** 0x15、0x30（CS43131）、0x6b、0x7e。

### 使用技术（架构总览）

> 各模块的接口、内部实现与踩坑细节均在对应源码头文件/源文件注释中，读代码时可自然了解；本部分只保留跨模块的架构与通用约定。

**1. 分层架构**
- `core/` 放与应用无关的底层：`core/hardware/`（显示/触摸/电池驱动）、`core/ui/`（LVGL 移植层 + 触摸测试 + 虚拟列表）、`core/utils/`（性能监视/电源/深链）；`core/player`、`core/storage`、`core/music`、`core/image`、`core/fonts` 放播放引擎与数据层；`ui/` 放应用 UI（app、main_ui、boot_flow、notification_panel、settings_menu 等）
- `main.c` 只做初始化装配：`config_store_init → display_init → touch_init → battery_init → audio_init → audio_set_volume_level → EQ 恢复 → pldata_init → button_service_init → sd_card_init → lvgl_port_init → boot_flow_run(持锁首帧+点亮) → deep_worker_init → sleep_timer_init → key_handler_init → power_service_init → battery_logger_init → startPerformanceMonitor()`

**2. 任务核心布局（UI Core0 / 音频 Core1）**
- 动机：息屏时 UI 完全静止（挂起），把 **UI（LVGL）固定在 Core0**、**播放解码/封面解码固定在 Core1**，二者分核互不抢占。
- **深链执行器 deep_worker**（`core/utils/deep_worker.h/.c`：常驻任务，栈 8KB、prio 6、Core0，信号量驱动）：息屏/亮屏（esp_pm_configure + AMOLED 重初始化 + lv_refr_now 全屏重刷）与伪关机（停外设 + 提示渲染 + deepsleep）深调用链统一委托它串行执行；业务任务只需 `deep_worker_call(fn, arg)` 投递并等待完成。⚠ 约定：fn 内禁止再 deep_worker_call；调用方禁止持 LVGL 锁投递；勿回到"在小任务里直接跑深链"（历史爆栈记录）。
- Core0：`LVGL`（lvgl_port_task）、`button_svc`、`deep_worker`、`player_ctrl`、`cpu_coll0` 及系统 esp_timer 任务/ISR（sdkconfig 固定 CPU0）与全部驱动中断（app_main 于 Core0 初始化）。
- Core1：`mp3_play`/`mp3_out`（解码输出，播放时建/常驻）、`cover_load`（封面解码，注册为 lvgl_port 辅助任务息屏随 LVGL 挂起）、`sleep_timer`、`power_svc`、`batt_log`（电池电压日志，仅开发者选项开启时建）、`cpu_coll1`；`boot_scan`/`rescan`（仅重扫场景）。
- 不固定：`touch_init`（开机一次性自删）、`mem_monitor`（性能监视，CONFIG_PERFORMANCE_MONITOR 控制）。
- 任务栈大小已在各创建处按 mem_monitor 高水位实测收紧（LVGL 6KB、button_svc 4KB、power_svc 3KB、sleep_timer 4KB、deep_worker 8KB、cover_load 6KB、mp3_play 8K/out 4K、player_ctrl 4KB）。⚠ 调栈前先看 HWM 且**必须覆盖目标路径实测**（如自动息屏/亮屏后才打印的读数），勿只信亮屏态读数。
- 息屏（`power_service_screen_off`，经 deep_worker）：AMOLED SLEEP IN（DISPOFF 0x28 + SLEEP IN 0x10）→ `lvgl_port_suspend`（停 tick + 挂起 LVGL/封面任务）→ 固定 80MHz。亮屏（`power_service_screen_on`，经 deep_worker）：固定 240MHz → 持锁 `display_sleep_exit`（SLEEP OUT 0x11 + 120ms + `esp_lcd_panel_init` 重发完整 init + 全屏重刷）→ 恢复亮度 → `lvgl_port_resume`（须先于 LVGL resume，休眠中/刚唤醒面板不能接收刷屏数据）。
- **自动关机策略（power_service 守护任务，1s tick，触发即走既有伪关机链 `fake_shutdown_enter`）**：① 息屏且暂停超时（`CONFIG_PAUSE_AUTO_SHUTDOWN_MIN`，默认 10 分钟）；② 电池电压**持续**低于 **3.45V**（`CONFIG_LOW_BATTERY_SHUTDOWN_SEC`，默认 60 秒，0=关闭；阈值 `BATTERY_CRITICAL_LOW_MV` 与开机拒启共用，见 battery.h）。⚠ 暂无充电检测，充电态电压被抬高不会误触发。
- 🆕 🎯 **"息屏停掉 Core1"（esp_cpu_stall / `CONFIG_SCREEN_OFF_STOP_CORE1`）已从代码与 Kconfig 完全移除**：该方案曾实测失败（长期 esp_cpu_stall 与被停核的 PSRAM/XIP 取指形成总线竞态 → INT WDT 超时），代码不再保留该配置项。勿重新引入；真正的进一步省电方向：暂停/停止时 light sleep 或走既有伪关机。

### 关键技术要点与踩坑记录（务必遵守）

> 硬件/音频/UI 的具体技术细节与踩坑（开机防白屏、显示初始化耗时、触摸异步初始化、触摸数据只读一次、CS43131 采样极性/时钟策略、MP3 采样率限制、音频浮点路径+Clamp、JPEG 封面后台解码、虚拟列表依赖 LARGE_COORD、完整字体不运行时加载等）均在各模块源码注释与 `ideas_and_exps/` 设计文档中，读代码/文档时可自然了解；此处仅保留跨项目通用约定。

- **代码注释一律中文**；函数/变量使用 snake_case 命名，模块前缀（display_/touch_/lvgl_port_/notif_/player_/config_store_ 等）。
- **LVGL 线程安全**：非 LVGL 任务的线程操作 UI 必须先 `lvgl_port_lock(-1)`，用完 `lvgl_port_unlock()`；LVGL 任务回调（timer/event）内已持锁，无需重复加锁。config_store 的 set_xxx 会同步触发已注册回调，UI 注册方须保证回调运行在 LVGL 任务上下文（持锁）。
- **改完即烧录**：首次烧录时，若未指定串口号，则列出串口并询问用户，后续修改完成后直接构建并烧录到对应串口并验证。
- **新模块记得更新 `main/CMakeLists.txt`**：新增源码文件需纳入 `MAIN_SRCS`（部分模块受 Kconfig 条件编译控制，见 CMakeLists 注释）。

## 用户背景介绍

### 用户背景

- 用户对嵌入式开发有一定经验（此前完成过 ESP32-P4 时钟项目 OtoClock），代码主要由 AI 协作编写。
- 用户有多年 Java/C# 等高级语言开发经验，擅长设计美观易用的界面，能读懂并修改 C 代码，但不编写 CMake 文件。
- 项目是 DIY 开源项目，注重软硬件自由：固件源码开放，鼓励用户自行修改；不应对固件数据/配置设置不必要的限制。
- 用户会随着项目进度逐渐发掘各 GPIO 的用途（见 GPIO 表），新确认的分配请同步更新文档。

### 用户对代码的要求

- 遵循当前项目已建立的分层与命名习惯（`core/` 底层 vs `core/player`、`core/storage` 等数据层 vs `ui/` 应用、snake_case、模块前缀），新增文件需同时更新 `main/CMakeLists.txt`。
- 代码注释均使用中文，逻辑清晰，关键常量/魔法数字要有注释说明。
- UI 功能按模块实现、逐个测试验证，不要一次性堆砌所有功能。
- 使用 Kconfig（`main/Kconfig.projbuild`）控制可裁剪功能。但通常三套构建配置已经覆盖大部分开启或者关闭的场景。（例如调试版和发布版的差异）
- 所有的过程文档、组件文档会放在根目录下的 ideas_and_exps 文件夹中，新模块建议同步更新 ideas_and_exps/ 对应设计文档。

### 回复/行动说明

- 推理/思考及回复均使用中文。
- 若 IDF 服务器未运行（`idfc` 报连接错误），请提示用户手动启动 `StartIDFCServer.vbs`，不要自行尝试在受限权限下启动服务器。
