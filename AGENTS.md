## FakePodNanoNightPlayer 项目概览

### 环境说明

**1. 硬件**

本项目是基于 ESP32-S3 成品播放器（开源项目 **FakePodNano** 的硬件）开发的第三方固件。设备通过 USB 串口（COM?，USB-Serial/JTAG 模式）连接，芯片参数：

- 芯片：ESP32-S3 (QFN56) rev v0.2，WiFi/BLE，240MHz 双核
- 存储：8MB Octal PSRAM（AP_3v3）、16MB Flash（QIO，80MHz）
- 屏幕：鱼鹰光电 AMOLED，**460x460**（型号 AM200Q460460LK，QSPI 接口），驱动 `esp_lcd_qspi_amoled`
- 触摸：CST820，驱动 `esp_lcd_touch_cst820`；
- 电源：电池电压经 10K+10K 分压到 GPIO1（ADC1_CH0）
- 耳放芯片：CS43131，I2C控制端口在0x30，通过I2S写入音频

**2. 开发环境**

- ESP-IDF **5.5.X**（安装于 `C:\esp\v5.5.X\esp-idf`），激活脚本 `C:\Espressif\tools\Microsoft.v5.5.X.PowerShell_profile.ps1`。不同的开发电脑上可能是5.5.4也可能是5.5.5。这里统一使用idfc.exe进行调用。
- LVGL **8.4.0**（managed component，`lvgl/lvgl`）
- 构建/烧录通过项目自带的 **`idfc.exe`**（客户端-服务器架构）完成，详见下方"idfc.exe 使用说明"。

**3. 参考资料**

- `components/`：本地组件（esp_lcd_qspi_amoled AMOLED 驱动、esp_lcd_touch_cst816/820/chsc6417 触摸驱动、i2c_bsp）
- `managed_components/`：ESP 组件管理器拉取的依赖（lvgl、esp_lcd_sh8601、esp_lcd_touch、cmake_utilities、esp-libhelix-mp3）
- `IDFC_USAGE.md`：idfc.exe 的完整使用说明（架构、启动服务器、常见问题排查）

**4. idfc.exe 使用说明（重要）**

`idfc.exe` 是 ESP-IDF 命令行的**服务器 + 客户端**封装：服务器一次性激活 IDF 环境并常驻监听 `http://localhost:19781/idf/`（HTTP + SSE 流式输出），客户端把 `idf.py` 命令发给服务器执行。

- **启动服务器**（需要 `danger-full-access` 权限，否则 HTTP.sys 端口绑定会失败）：
  ```powershell
  .\idfc.exe --server --idfenv "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"
  ```
  也可以让用户直接运行项目根目录的 `StartIDFCServer.vbs`。

- **执行命令**（客户端，在 `workspace-write` 权限下运行）：
  ```powershell
  .\idfc.exe build              # 构建
  .\idfc.exe -p COM5 flash      # 烧录（设备当前固定连接 COM5）
  .\idfc.exe -p COM5 monitor    # 串口监视
  ```
- **⚠ 若提示 `IDF server is not running at http://localhost:19781/idf.`**：说明服务器未运行或被回收，需要**提示用户手动启动服务器**（运行 `StartIDFCServer.vbs` 或上述 `--server` 命令），不要自行尝试在受限权限下重建服务器。服务器是常驻后台进程，会话结束后可能被回收，每次遇到连接错误都应先检查/提醒启动服务器。
- 服务器由 `idfc.exe` 内部派生的 `--server-child` 进程承载，用 `pwsh` 前台启动更稳定。

**5. 常用命令**

- `idfc build`：构建项目（初次/配置变更后较久，建议放宽超时）。
- `idfc -p COM5 flash`：烧录到设备。**通常烧录前先征得用户确认。COM口不一定固定，（除非用户已明确要求烧录，并指定COM口）**。
- `idfc -p COM5 monitor`：查看串口日志（退出 Ctrl+C或者直接结束任务）。
- 已开放 `C:\esp\` 与 `C:\Espressif\` 的读取权限，可查阅 IDF 源码。

**6. 构建配置**

- `main/Kconfig.projbuild`：应用配置项（menuconfig）：
  - `CONFIG_TOUCH_TEST`（触摸区域测试，默认开）：显示 20px 白点跟随手指，抬起后 1s 渐变消失，用于触摸区域调试
- `sdkconfig.defaults`：esp32s3、QIO 16MB Flash、自定义分区表、8MB Octal PSRAM（含 XIP 指令/只读数据）、240MHz、64B 数据缓存线、FreeRTOS 1000Hz、LVGL indev 轮询 15ms、开启 FreeRTOS 运行时统计（`GENERATE_RUN_TIME_STATS`，performance_monitor 依赖）、LVGL 字体（12/16/20/28/48）与演示。
- `sdkconfig`（生成结果，已纳入 git）：由构建自动合并 defaults 生成；`fullclean` 只清空 `build/`，不会删除 sdkconfig，仅在 sdkconfig 不存在时从 defaults 重新生成。
- `partitions.csv`：`nvs`(0x9000) / `phy_init`(0xf000) / `factory`(6M，0x10000 起)。

### 项目内容

**FakePodNanoNightPlayer —— FakePodNano 播放器的第三方固件**

> 本项目目标：为成品开源播放器 **FakePodNano**（ESP32-S3 + AMOLED + CS43131 + 触摸）开发**更好用、专注于音乐播放**的第三方固件，替代/改进原厂固件的使用体验。

**愿景（尚未实现的部分）：**
- 完整的音乐播放能力（解码、播放控制、进度、播放列表）
- 专注音乐播放的简洁 UI（区别于通用演示界面）
- 通过顶部的状态栏/下拉通知栏提供系统级信息（时间、电池、亮度）——下拉通知栏架构已就绪

**当前实现状态：**
- 硬件层：AMOLED 显示（QSPI，支持 6 种屏幕尺寸配置切换）、触摸（CST816/820/CHSC6417 按屏幕自动选择）、电池电压 ADC、音频（I2S 主机 + CS43131 DAC + MP3 解码播放）、SD 卡（SDMMC 4-bit + FAT，挂载 `/sdcard`）
- LVGL 移植层：显示驱动、2ms tick、专用 LVGL 任务（固定 Core0）、互斥锁、触摸 indev（15ms，Y 坐标 ×23/24 校准）、触摸事件订阅回调
- UI：主界面（Hello World 占位）、顶部状态栏（运行时长时钟）、下拉通知栏（电池显示 + 亮度滑块 + 底部上拉把手，**纯 LVGL 事件驱动**拖拽）
- 工具：双核 CPU/内存性能监视（串口每 2s 输出）、触摸区域测试（Kconfig 控制）

### 代码结构

```
main/
  main.c                    # app_main：硬件初始化 → LVGL 移植层 → UI 组装 → 性能监视
  CMakeLists.txt
  Kconfig.projbuild         # 应用配置项（CONFIG_TOUCH_TEST）
  core/
    hardware/               # 与应用无关的底层硬件驱动
      display.h/.c          # AMOLED LCD（QSPI 总线/面板）+ 屏幕尺寸配置 + 亮度控制
      touch.h/.c            # 触摸控制器（I2C + CST816/820/CHSC6417 按需选择）
      battery.h/.c          # 电池电压 ADC（分压换算 + 校准）
      button_service.h/.c   # A/B 双键服务（GPIO48/GPIO0，去抖 + 单击/双按事件）
      sd_card.h/.c          # SD 卡驱动（SDMMC 4-bit + FAT 挂载 /sdcard）
      audio/
        audio.h/.c          # 音频播放 API（I2S 主机 + 正弦波 + PCM 流写 I2S）
        cs43131.h/.c        # CS43131 DAC 驱动（I2C 控制、24.576MHz 直通 MCLK、ASP 从机 I2S、音量）
        mp3_player.h/.c     # MP3 解码播放（Helix + 跳 ID3v2 + 写 I2S，Core1 后台任务）
    ui/                     # 通用 UI 基础设施（与应用无关）
      lvgl_port.h/.c        # LVGL 移植层：显示驱动、tick、任务(Core0)、互斥锁、
                            #   触摸 indev、触摸事件订阅（lvgl_port_register_touch_cb）
      touch_test.h/.c       # 触摸区域测试（白点跟随 + 淡出，CONFIG_TOUCH_TEST）
    utils/
      performance_monitor.h/.c  # 双核 CPU/内存监视（从 OtoClock 移植）
  ui/                       # 应用 UI
    app.h/.c                # UI 组装入口 + 顶部状态栏（时间）
    main_ui.h/.c            # 主界面（正弦波播放控制：采样率×频率按钮 + 停止 + 音量滑块）
    notification_panel.h/.c # 下拉通知栏（电池/亮度/拖拽，LVGL 事件驱动）
components/
  esp_lcd_qspi_amoled/      # AMOLED 面板驱动（QSPI，含亮度 0x51 命令）
  esp_lcd_touch_cst816/     # CST816 触摸
  esp_lcd_touch_cst820/     # CST820 触摸
  esp_lcd_touch_chsc6417/   # CHSC6417 触摸
  i2c_bsp/                  # I2C 总线 BSP
```

### GPIO 分配（随项目进度持续发掘）

> ⚠ **重要**：本项目的 GPIO 用法是**随开发进度逐渐发掘和补充**的。当前仅确认了屏幕/触摸/电池部分；其它 GPIO（SD 卡、音频、麦克风、按键、IMU 等）在源码历史注释中有规划（见下方"历史注释规划"），但**尚未验证启用**。每次新增外设功能时，先核对本表避免冲突，确认用法后更新本表。

**当前已确认使用的 GPIO：**

| GPIO | 功能 | 说明 |
|---|---|---|
| 0 | B 键 | 按键接 GND，外部 10kΩ 上拉（`core/hardware/button_service.c`）。⚠ strapping 引脚：复位时若被按住会进入下载模式 |
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

**历史注释规划（源码 `main.c` 旧版注释，当前未启用、未验证）：**
- LCD 相关：`TFT_TE = 6`（撕裂效果同步）
- I²C 总线：`I2C_WIRE_SDA = 3`、`I2C_WIRE_SCL = 2`（与触摸共用）
- 音频（I2S + CS43131 DAC）：`MCK=-1, BCK=40, WS=38, DO=39, CS43131_RST=41`（**已确认启用**，见 `core/hardware/audio/`；MCLK 由板载 24.576MHz 晶振提供，不占用 GPIO）
- 按键/静音：~~普通版 `BTN_PIN=47`、`AUDIO_MUTE=48`；HIFI 版 `BTN_PIN=48`~~（**已确认**：A 键=GPIO48、B 键=GPIO0，见 `button_service.c`）
- 麦克风（PDM）：`MIC_PDM_DATA=46, MIC_PDM_SCK=45`
- IMU 中断：`IMU_INT1=47`（HIFI 版）

**已确认的 I²C 设备（开机扫描）：** 0x15、0x30（CS43131）、0x6b、0x7e。

### 使用技术（已完成的系统模块）

**1. 分层架构**（已完成）
- `core/` 放与应用无关的底层：`core/hardware/`（显示/触摸/电池驱动）、`core/ui/`（LVGL 移植层 + 触摸测试）、`core/utils/`（性能监视）
- `ui/` 放应用 UI：app（组装 + 状态栏）、main_ui（主界面）、notification_panel（通知栏）
- `main.c` 只做初始化装配：`display_init() → touch_init() → battery_init() → audio_init() → lvgl_port_init(panel, tp) → app_ui_create() → startPerformanceMonitor()`

**2. LVGL 移植层**（`core/ui/lvgl_port.c`，已完成）
- 显示驱动注册（flush/rounder 回调、DMA draw buffer、2ms tick）、LVGL 任务固定 **Core0**（`xTaskCreatePinnedToCore`）
- 互斥锁 `lvgl_port_lock/unlock`：LVGL API 非线程安全，所有外部 UI 操作必须先持锁
- 触摸输入设备：`lvgl_touchpad_read_cb` 统一读取触摸（**触摸数据全项目只读一次**），并支持事件订阅 `lvgl_port_register_touch_cb(pressed, x, y, ctx)`，避免各功能重复轮询
- **触摸 Y 坐标校准：`y = y * 23 / 24`**，在 `lvgl_touchpad_read_cb` 中统一应用，LVGL 与订阅回调共用（触摸面板纵向比例修正，经实测最符合直觉）

**3. 下拉通知栏**（`ui/notification_panel.c`，已完成）
- 全屏面板 + 顶部状态栏（时间）+ 电池显示 + 亮度滑块 + 底部上拉把手
- **拖拽纯 LVGL 事件驱动**（`LV_EVENT_PRESSED/PRESSING/RELEASED`），通过 `lv_event_get_indev()` + `lv_indev_get_point()` 取坐标，**无独立轮询任务**
- 手势区：顶部 46px（下拉/上拉）或底部 46px（上拉收起）；滑块等可点击控件消费自身事件（不冒泡），不与拖拽冲突
- 松手按速度矢量决策展开/收起（`VEL_THRESH=400px/s`，200ms 缓出动画）

**4. 性能监视**（`core/utils/performance_monitor.c`，已完成，从 OtoClock 移植）
- 双核 CPU：每个核心上跑 `cpu_collector_task`，用 `ulTaskGetIdleRunTimeCounter()` + `esp_timer` 计算利用率
- 内存：每 2s 打印 `heap`（默认堆）/`iram`（内部 RAM）及增量、cpu0/cpu1 百分比
- 依赖 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`（已在 defaults 开启）

**5. 触摸区域测试**（`core/ui/touch_test.c`，`CONFIG_TOUCH_TEST` 控制）
- 20px 白色圆点跟随手指，抬起后 1s 内 `ease_in` 渐变消失
- 复用 `lvgl_port_register_touch_cb` 触摸数据（无独立 timer）；圆点 `HIT_TEST` 穿透，不拦截控件
- 在 `lvgl_port_init()` 末尾初始化（Kconfig 开关）

### 关键技术要点与踩坑记录（务必遵守）

- **触摸只读一次**：全项目触摸数据统一由 `lvgl_port` 的 indev 回调读取，需要触摸的功能一律通过 `lvgl_port_register_touch_cb` 订阅，**禁止**再自行调用 `esp_lcd_touch_read_data`/轮询。
- **LVGL 线程安全**：非 LVGL 任务的线程操作 UI 必须先 `lvgl_port_lock(-1)`，用完 `lvgl_port_unlock()`；LVGL 任务回调（timer/event）内已持锁，无需重复加锁。
- **事件驱动优先**：控件交互（拖拽、点击）优先用 LVGL 事件回调，避免自建轮询 timer。可点击控件默认不冒泡事件，天然隔离交互区域。
- **不拦截事件的视觉层**：纯视觉对象（触摸白点、底部把手）需 `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE)` 并挂 `LV_EVENT_HIT_TEST` 回调置 `res=false`，否则会挡住下层控件。
- **容器滚动条**：`lv_obj_create` 容器默认带 padding 且可滚动，全屏内容容器需 `pad_all=0` + 清除 `SCROLLABLE` + `LV_SCROLLBAR_MODE_OFF`，否则子对象超宽会出现横向滚动条。
- **CS43131 采样极性**：ASP 从机模式下 `0x040018` 必须写 `0x0C`（`SCPOL_IN=1`，SCLK 下降沿采样）。写 `0x00`（`SCPOL_IN=0`，上升沿采样）会导致左声道（LRCK 下降沿后第一个 slot）逐位错位、输出锯齿/方波，而右声道因时序余量大碰巧正常，极具迷惑性（曾在此错误方向绕了很久）。
- **CS43131 时钟策略**：全程固定 24.576MHz 直通（`0x010006=0x00`，MCLK_SRC_SEL=00 + MCLK_INT=0），**禁止**在播放时动态切换 PLL/MCLK 源——手册 §4.6 明确：切换 MCLK 源时若目标时钟未就绪，芯片会进入**不可恢复的失响应状态**（卡死，只能复位）。切换采样率只写 `0x01000B`（ASP_SPRATE）。
- **MP3 采样率限制**：CS43131 的 `ASP_SPRATE` 只支持 32k/44.1k/48k/88.2k/96k/176.4k/192k/352.8k/384k 档位，当前 MP3 播放（`mp3_player.c`）仅支持 32k/44.1k/48k 三档；22.05k/24k 等其它采样率需软件重采样（尚未实现），遇到会报 `unsupported sample rate` 停播。
- **代码注释一律中文**；函数/变量使用 snake_case 命名，模块前缀（display_/touch_/lvgl_port_/notif_ 等）。
- **改完即烧录**：用户偏好修改完成后直接构建并烧录到 COM5 验证（除非用户另有指示）。

## 用户背景介绍

### 用户背景

- 用户对嵌入式开发有一定经验（此前完成过 ESP32-P4 时钟项目 OtoClock），代码主要由 AI 协作编写。
- 用户有多年 Java/C# 等高级语言开发经验，擅长设计美观易用的界面，能读懂并修改 C 代码，但不编写 CMake 文件。
- 项目是 DIY 开源项目，注重软硬件自由：固件源码开放，鼓励用户自行修改；不应对固件数据/配置设置不必要的限制。
- 用户会随着项目进度逐渐发掘各 GPIO 的用途（见 GPIO 表），新确认的分配请同步更新文档。

### 用户对代码的要求

- 遵循当前项目已建立的分层与命名习惯（`core/` 底层 vs `ui/` 应用、snake_case、模块前缀），新增文件需同时更新 `main/CMakeLists.txt`。
- 代码注释均使用中文，逻辑清晰，关键常量/魔法数字要有注释说明。
- UI 功能按模块实现、逐个测试验证，不要一次性堆砌所有功能。
- 使用 Kconfig（`main/Kconfig.projbuild`）控制可裁剪功能（如 `CONFIG_TOUCH_TEST`），参考 `..\OtoClock\main\Kconfig.projbuild` 的写法。

### 回复/行动说明

- 推理/思考及回复均使用中文。
- 修改代码后默认构建并烧录到 COM5（用户已明确"后续一律改完后烧录"），烧录前无需重复确认。
- 若 IDF 服务器未运行（`idfc` 报连接错误），请提示用户手动启动 `StartIDFCServer.vbs`，不要自行尝试在受限权限下启动服务器。
