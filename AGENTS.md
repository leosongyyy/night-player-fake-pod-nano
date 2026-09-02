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

本项目使用多配置构建系统，以下是不同配置的命令速查表：

** debug（精简字体 + 调试全开） **

```powershell
# 增量构建
.\idfc.exe -B build/debug build

# 重新配置（改过 sdkconfig.defaults* 后，先删 build/debug/sdkconfig 再执行）
.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" reconfigure

# 烧录
.\idfc.exe -B build/debug -p COMn flash
```

** releasecandidate（完整字体 + 调试全开） **

```powershell
# 增量构建
.\idfc.exe -B build/releasecandidate build

# 重新配置
.\idfc.exe -B build/releasecandidate -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.releasecandidate" -D SDKCONFIG="build/releasecandidate/sdkconfig" reconfigure

# 烧录
.\idfc.exe -B build/releasecandidate -p COMn flash
```

** release（完整字体 + 调试全关） **

```powershell
# 增量构建
.\idfc.exe -B build/release build

# 重新配置
.\idfc.exe -B build/release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" -D SDKCONFIG="build/release/sdkconfig" reconfigure

# 烧录
.\idfc.exe -B build/release -p COMn flash
```

**备注**：只有首次或"改过 `sdkconfig.defaults.*`"才需要带 `-D` 重新配置；日常增量构建和烧录只带 `-B` 即可。烧录前记得确认串口号。
默认情况下，构建调试版的，除非用户指定。

完整的多配置构建说明见 ideas_and_exps/BUILD_CONFIG.md

- `idfc -p COMx monitor`：查看串口日志（退出 Ctrl+C或者直接结束任务）。
- 已开放 `C:\esp\` 与 `C:\Espressif\` 的读取权限，可查阅 IDF 源码。

**6. 构建配置**

- `main/Kconfig.projbuild`：应用配置项（menuconfig）：
  - `CONFIG_TOUCH_TEST`（触摸区域测试，默认开）：显示 20px 白点跟随手指，抬起后 1s 渐变消失，用于触摸区域调试
  - `CONFIG_SCREEN_OFF_STOP_CORE1`（息屏停 Core1，**默认关**，实验失败方案）：esp_cpu_stall 停核与 PSRAM/XIP 总线竞态致崩溃，勿开启，见第 6 节
- `sdkconfig.defaults`（+ `sdkconfig.defaults.debug` / `.releasecandidate` / `.release`）：esp32s3、QIO 16MB Flash、自定义分区表、8MB Octal PSRAM（含 XIP 指令/只读数据）、240MHz、64B 数据缓存线、FreeRTOS 1000Hz、LVGL 刷新 33ms / indev 轮询 33ms、开启 FreeRTOS 运行时统计（`GENERATE_RUN_TIME_STATS`，performance_monitor 依赖）、LVGL 字体（12/16/20/24/28/32/48，默认 18）
- **版本管理：配置事实源只有 `sdkconfig.defaults*` 与 `main/Kconfig.projbuild`（均纳入 git）**；生成的根 `sdkconfig` 与各 `build/<cfg>/sdkconfig` **已被 `.gitignore` 忽略，不入库**。多配置构建下每个 build 目录持有自己的 sdkconfig（`-D SDKCONFIG=build/<cfg>/sdkconfig` 指定）；`fullclean` 只清 `build/`，不会删 sdkconfig。改配置的正确姿势：改 `sdkconfig.defaults*` 或 `Kconfig.projbuild` 后，按第 5 节命令删对应 `build/<cfg>/sdkconfig` 再带 `-D` reconfigure（IDF 也会在 defaults 变化时自动重新合并，删了重建最干净）。
- `partitions.csv`：`nvs`(0x9000) / `phy_init`(0xf000) / `factory`(6M，0x10000 起)。

### 项目内容

**FakePodNanoNightPlayer —— FakePodNano 播放器的第三方固件**

> 本项目目标：为成品开源播放器 **FakePodNano**（ESP32-S3 + AMOLED + CS43131 + 触摸）开发**更好用、专注于音乐播放**的第三方固件，替代/改进原厂固件的使用体验。

**愿景（尚未实现的部分）：**
- 完整的音乐播放能力（解码、播放控制、进度、播放列表）
- 专注音乐播放的简洁 UI（区别于通用演示界面）
- 通过顶部的状态栏/下拉通知栏提供系统级信息（时间、电池、亮度）——下拉通知栏架构已就绪

**当前实现状态：**
- 硬件层：AMOLED 显示（QSPI，支持 6 种屏幕尺寸配置切换，**全黑启动防开机白屏闪烁**）、触摸（CST816/820/CHSC6417 按屏幕自动选择，**异步初始化不阻塞开机**）、电池电压 ADC、音频（I2S 主机 + CS43131 DAC + MP3 解码播放）、SD 卡（SDMMC 4-bit + FAT，挂载 `/sdcard`）
- **任务核心布局：UI（LVGL）固定 Core0、播放解码（MP3/封面）固定 Core1，二者分核互不抢占（见第 6 节；早期文档"UI Core1/播放 Core0"是停核实验分支遗留，已按当前代码修正；"息屏停 Core1"实验已失败并默认关闭）**
- LVGL 移植层：显示驱动、2ms tick、专用 LVGL 任务（固定 Core0）、互斥锁、触摸 indev（33ms，x/y 坐标 ×29/30 校准）、触摸事件订阅回调、辅助 UI 任务注册（随 LVGL 挂起/恢复）；息屏/伪关机的深调用链统一委托 deep_worker 大栈任务（见第 6 节）
- UI：主界面（四页滑动 tab：目录/播放列表/正在播放/设置）、顶部状态栏（运行时长时钟）、下拉通知栏（电池显示 + 亮度滑块 + 底部上拉把手，**纯 LVGL 事件驱动**拖拽）
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
      button_service.h/.c   # A/B 双键服务（GPIO48/GPIO0，去抖 + 单击/双按/长按事件）
      sd_card.h/.c          # SD 卡驱动（SDMMC 4-bit + FAT 挂载 /sdcard）
      audio/
        audio.h/.c          # 音频播放 API（I2S 主机 + CS43131 DAC + PCM 流写 I2S）
        cs43131.h/.c        # CS43131 DAC 驱动（I2C 控制、24.576MHz 直通 MCLK、ASP 从机 I2S、音量）
        mp3_player.h/.c     # MP3 解码播放（Helix + 跳 ID3v2 + 写 I2S，Core1 后台任务）
    ui/                     # 通用 UI 基础设施（与应用无关）
      lvgl_port.h/.c        # LVGL 移植层：显示驱动、tick、任务(Core0)、互斥锁、
                            #   触摸 indev、触摸事件订阅（lvgl_port_register_touch_cb）
      touch_test.h/.c       # 触摸区域测试（白点跟随 + 淡出，CONFIG_TOUCH_TEST）
    utils/
      performance_monitor.h/.c  # 双核 CPU/内存监视（从 OtoClock 移植，含 ISR 采样 CPU 频率）
      fake_shutdown.h/.c        # 伪关机（睡眠定时器到点；A 键长按"等待断电"，深链委托 deep_worker）
      sleep_timer.h/.c          # 睡眠定时器（到点触发普通伪关机）
      screen_sleep.h/.c         # 息屏管理（AMOLED SLEEP IN + 挂起 LVGL + esp_pm 降频 + 无操作超时自动息屏）
      deep_worker.h/.c          # 深路径执行器（大栈任务）：息屏/亮屏/伪关机深调用链唯一执行者
  ui/                       # 应用 UI
    app.h/.c                # UI 组装入口 + 顶部状态栏（时间/音量/电量）
    main_ui.h/.c            # 主界面（四页滑动 tab：目录/播放列表/正在播放/设置）
    notification_panel.h/.c # 下拉通知栏（电池/亮度/拖拽，LVGL 事件驱动）
    key_handler.h/.c        # 按键分发（亮屏：音量±/伪关机/息屏；息屏：播放控制/亮屏）
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
| 48 | A 键（电源键） | 按键接 GND，内部上拉（`core/hardware/button_service.c`）。⚠ 与电源侧**3 秒关机芯片复用同一按键**：长按满 2s 固件伪关机（`fake_shutdown_wait_power_off`，保存配置+停外设+定格提示+deepsleep），继续按住满 3s 由硬件直接断电/开机。非 RTC GPIO，无法用于 deepsleep 唤醒 |

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
- `main.c` 只做初始化装配：`config_store_init → display_init → touch_init → battery_init → audio_init → pldata_init → button_service_init → sd_card_init → lvgl_port_init → boot_flow_run(持锁首帧+点亮) → deep_worker_init → sleep_timer_init → key_handler_init → screen_sleep_init → startPerformanceMonitor()`

**2. LVGL 移植层**（`core/ui/lvgl_port.c`，已完成）
- 显示驱动注册（flush/rounder 回调、DMA draw buffer、2ms tick）、LVGL 任务固定 **Core0**（`xTaskCreatePinnedToCore`；UI 与播放解码/封面分核互不抢占，见第 6 节）
- 互斥锁 `lvgl_port_lock/unlock`：LVGL API 非线程安全，所有外部 UI 操作必须先持锁
- 辅助 UI 任务注册 `lvgl_port_register_aux_task`：会触碰 LVGL 的后台任务（封面加载）注册后随 `lvgl_port_suspend/resume` 一起挂起/恢复（先挂起/先恢复，避免冻结在持锁中途）
- 触摸输入设备：`lvgl_touchpad_read_cb` 统一读取触摸（**触摸数据全项目只读一次**），并支持事件订阅 `lvgl_port_register_touch_cb(pressed, x, y, ctx)`，避免各功能重复轮询
- **触摸异步初始化**：`touch_init()` 只同步做 I2C 总线/扫描/面板 IO，控制器完整初始化（`esp_lcd_touch_new_i2c_xxx`，含约 400ms 复位等待）放后台任务，就绪后经 `touch_register_ready_cb` 回调把句柄交给 LVGL（`lvgl_port_set_touch_handle`）；就绪前 `read_cb` 判空返回"未按下"不给数据
- **触摸坐标校准：`x = x * 29 / 30`、`y = y * 29 / 30`**（x/y 等比缩放），在 `lvgl_touchpad_read_cb` 中统一应用，LVGL 与订阅回调共用（触摸面板比例修正，经实测最符合直觉）

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

**6. 任务核心布局（UI Core0 / 音频 Core1）**
- 布局动机：息屏时 UI 完全静止（挂起），把 **UI（LVGL）固定在 Core0**、**播放解码/封面解码固定在 Core1**，二者分核互不抢占；息屏挂起 LVGL 后 Core0 空闲（tick 停止），Core1 继续 80MHz 播放。
  ⚠ 早期文档"UI Core1/播放 Core0"是停核实验分支遗留描述，当前代码实际为 UI Core0/音频 Core1（audio.c 与各任务创建处注释一致），本文档已按代码修正。
- **深链执行器 deep_worker**：息屏/亮屏（esp_pm_configure + AMOLED 重初始化 + lv_refr_now 全屏重刷）与伪关机（停外设 + 提示渲染 + deepsleep）调用链深，历史导致 button/screen_sleep/伪关机等多个任务各自扛 8KB 大栈。现统一委托 `deep_worker`（`core/utils/deep_worker.h/.c`：常驻任务，栈 8KB、prio 6、Core0，信号量驱动）串行执行；业务任务只需 `deep_worker_call(fn, arg)` 投递并等待完成（浅调用，实测各调用任务栈峰值 0.7~2.4KB）。⚠ 约定：fn 内禁止再 deep_worker_call；调用方禁止持 LVGL 锁投递。
- Core0：`LVGL`（lvgl_port_task）、`button_svc`、`deep_worker`、`player_ctrl`、`cpu_coll0` 及系统 esp_timer 任务/ISR（sdkconfig 固定 CPU0）与全部驱动中断（app_main 于 Core0 初始化）
- Core1：`mp3_play`/`mp3_out`（解码输出，播放时建/常驻）、`cover_load`（封面解码，注册为 lvgl_port 辅助任务息屏随 LVGL 挂起）、`sleep_timer`、`screen_sleep`、`cpu_coll1`；`boot_scan`/`rescan`（仅重扫场景）
- 不固定：`touch_init`（开机一次性自删）、`mem_monitor`（性能监视，CONFIG_PERFORMANCE_MONITOR 控制）
- **任务栈现状（2026-09 按 mem_monitor 栈高水位实测收紧；perf 日志每 20s 一行 `stk: 任务名=剩余字节`，HWM 为自启动以来最小剩余）**：LVGL 6KB（峰值~3.1K）、button_svc 4KB（峰值~2.4K）、screen_sleep 3KB（自动息屏投递链峰值 ~1.9K；曾缩 2K 实测仅剩 116B 过紧，故定 3K）、sleep_timer 4KB（渐降 I2C 链需 4K，勿缩）、deep_worker 8KB（伪关机链 deepsleep 后 HWM 清零无法实测，保持 8K）、cover_load 6KB（解码峰值~3.8K，余~2.2K）、mp3_play/out 8K/4K、player_ctrl 4KB（峰值 2.6K 偏紧勿缩）。⚠ 调栈前先看 HWM 且**必须覆盖目标路径实测**（如自动息屏/亮屏后才打印的读数），勿只信亮屏态读数；勿回到"在小任务里直接跑深链"（历史 2~3KB 爆栈记录）。release 配置另关 CONFIG_PERFORMANCE_MONITOR（省 mem_monitor 4K + cpu_coll×2 2K）。
- 息屏（`screen_sleep_enter`，状态翻转后经 deep_worker 执行）：**AMOLED SLEEP IN**（DISPOFF 0x28 + SLEEP IN 0x10，`display_sleep_enter`：驱动 IC 停 DC/DC/振荡器/扫描，显示 RAM 保留——真正的低功耗态，仅亮度 0 时 IC 仍满负荷）→ `lvgl_port_suspend`（停 tick + 挂起 LVGL/封面任务）→ 固定 80MHz（详见 SCREEN_SLEEP_AMOLED_OPTIMIZATION.md）
- 亮屏（`screen_sleep_exit`，经 deep_worker）：固定 240MHz → **持锁 `display_sleep_exit`**（SLEEP OUT 0x11 + 120ms 恢复等待 + `esp_lcd_panel_init` 重发完整 init——该面板 SLEEP OUT 后必须重初始化，仅 0x11+DISPON 不亮屏——+ 全屏 `lv_obj_invalidate`/`lv_refr_now` 黑屏中重刷）→ 恢复用户亮度 → `lvgl_port_resume`。须先于 LVGL resume（休眠中/刚唤醒面板不能接收刷屏数据）
- ⚠ **"息屏停掉 Core1"（esp_cpu_stall，`CONFIG_SCREEN_OFF_STOP_CORE1`）已实测失败，默认关闭**：ESP32-S3 上长期 esp_cpu_stall 与被停核的 PSRAM/XIP 取指形成总线竞态——被停核若恰在 PSRAM 总线事务上被冻结，另一核访问 PSRAM 永久等待 → INT WDT 超时（Guru Meditation "Interrupt wdt timeout on CPUx"，PC 停在解码等访问 PSRAM 的代码）。esp_cpu_stall 只适合瞬时跨核操作。代码保留供参考，**勿再开启**；真正的进一步省电方向：暂停/停止时 light sleep 或走既有伪关机

### 关键技术要点与踩坑记录（务必遵守）

- **开机防白屏（全黑启动）**：AMOLED 面板 RAM 上电未初始化，若以默认亮度点亮会闪白屏。所有屏幕的 init 序列中亮度命令 `0x51` **必须**位于 `0x29`（DISPON 打开显示器）**之前**且值置 `0x00`（全黑）；`display_init()` 末尾同步亮度状态为 0，UI 首帧渲染完成（`app_main` 中 `lv_refr_now(NULL)` 后）再调用 `display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT)` 恢复。**⚠ `display_set_brightness()` 必须在持有 `lvgl_port_lock` 时调用**：亮度命令与 LVGL 刷屏共用 SPI 总线，锁外并发会冲突（曾致画面定格/花屏）；`app_main` 已把点亮放在 `lv_refr_now(NULL)` 的同一持锁区间内。默认亮度宏 `DISPLAY_BRIGHTNESS_DEFAULT`（0xFF）定义在 `display.h`。
- **显示初始化耗时优化**：display_init 的 ~360ms 主要来自组件 `panel_qspi_amoled_reset()` 的 RST 释放后 150ms 等待（模板保守值）与 init 序列延时。已压缩为 RST 释放后 30ms（组件 `esp_lcd_qspi_amoled.c`，配合 `0x11` Sleep Out 的 120ms 总恢复时间充足）+ 序列中 `0x29`（DISPON）后延时 60→0ms；`0x11` 的 120ms 是规格书要求的恢复时间**不可省**。另外序列中的 `0x3A`/`0x36` 与组件内部 COLMOD/MADCTL 重复，删掉可消除 "3Ah command overwritten" 警告。若切其它屏幕，需实测确认 30ms 复位等待足够。
- **触摸异步初始化**：`esp_lcd_touch_new_i2c_xxx` 内部 `reset()` 含 200ms×2 复位等待（约 400ms），若在开机关键路径同步调用会拖慢启动。`touch_init()` 只同步做 I2C 总线/扫描/面板 IO，控制器初始化放后台任务（优先级 5，栈 4KB），完成回调（`touch_register_ready_cb` → `lvgl_port_set_touch_handle`）交给 LVGL；就绪前 `lvgl_touchpad_read_cb` 判空返回"未按下"（不给数据）。`touch_power_off()` 需先等初始化任务完成，避免与 `i2c_driver_delete` 冲突。
- **触摸只读一次**：全项目触摸数据统一由 `lvgl_port` 的 indev 回调读取，需要触摸的功能一律通过 `lvgl_port_register_touch_cb` 订阅，**禁止**再自行调用 `esp_lcd_touch_read_data`/轮询。
- **LVGL 线程安全**：非 LVGL 任务的线程操作 UI 必须先 `lvgl_port_lock(-1)`，用完 `lvgl_port_unlock()`；LVGL 任务回调（timer/event）内已持锁，无需重复加锁。
- **事件驱动优先**：控件交互（拖拽、点击）优先用 LVGL 事件回调，避免自建轮询 timer。可点击控件默认不冒泡事件，天然隔离交互区域。
- **不拦截事件的视觉层**：纯视觉对象（触摸白点、底部把手）需 `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE)` 并挂 `LV_EVENT_HIT_TEST` 回调置 `res=false`，否则会挡住下层控件。
- **容器滚动条**：`lv_obj_create` 容器默认带 padding 且可滚动，全屏内容容器需 `pad_all=0` + 清除 `SCROLLABLE` + `LV_SCROLLBAR_MODE_OFF`，否则子对象超宽会出现横向滚动条。
- **CS43131 采样极性**：ASP 从机模式下 `0x040018` 必须写 `0x0C`（`SCPOL_IN=1`，SCLK 下降沿采样）。写 `0x00`（`SCPOL_IN=0`，上升沿采样）会导致左声道（LRCK 下降沿后第一个 slot）逐位错位、输出锯齿/方波，而右声道因时序余量大碰巧正常，极具迷惑性（曾在此错误方向绕了很久）。
- **CS43131 时钟策略**：全程固定 24.576MHz 直通（`0x010006=0x00`，MCLK_SRC_SEL=00 + MCLK_INT=0），**禁止**在播放时动态切换 PLL/MCLK 源——手册 §4.6 明确：切换 MCLK 源时若目标时钟未就绪，芯片会进入**不可恢复的失响应状态**（卡死，只能复位）。切换采样率只写 `0x01000B`（ASP_SPRATE）。
- **MP3 采样率限制**：CS43131 的 `ASP_SPRATE` 只支持 32k/44.1k/48k/88.2k/96k/176.4k/192k/352.8k/384k 档位，当前 MP3 播放（`mp3_player.c`）仅支持 32k/44.1k/48k 三档；22.05k/24k 等其它采样率需软件重采样（尚未实现），遇到会报 `unsupported sample rate` 停播。
- **代码注释一律中文**；函数/变量使用 snake_case 命名，模块前缀（display_/touch_/lvgl_port_/notif_ 等）。
- **改完即烧录**：首次烧录时，若未指定串口号，则列出串口并询问用户，后续修改完成后直接构建并烧录到对应串口并验证。

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
- 所有的过程文档、组件文档会放在根目录下的 ideas_and_exps 文件夹中。

### 回复/行动说明

- 推理/思考及回复均使用中文。
- 若 IDF 服务器未运行（`idfc` 报连接错误），请提示用户手动启动 `StartIDFCServer.vbs`，不要自行尝试在受限权限下启动服务器。
