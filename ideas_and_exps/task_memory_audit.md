# RTOS 任务 + 内存分配全量审计（ESP32-S3, releasecandidate 构建）

> 审计对象：`build/releasecandidate` 产物（ESP-IDF 5.5.5, FreeRTOS-SMP, xtensa-esp32s3, StackType_t = **uint8_t**，`xTaskCreatePinnedToCore` 的栈参数即**字节数**，无需 ×4）。
> 范围：main/ 下全部 16 处 `xTaskCreate`/`xTaskCreatePinnedToCore` 调用点；静态内存按 `.map`/`.elf` 实测；动态大头按代码确认。
> 单位：表内所有栈/内存数值均为**字节**；地址前缀 0x3f…= 内部 DRAM（变量/静态），0x4002…/0x4037… = 指令侧（代码），0x3c…/0x4200… = flash/PSRAM 映射只读区（不占 SRAM）。

---

## 一、RTOS 任务清单

### 1.1 应用任务（main/ 源码创建，按创建顺序大致排列）

| 任务名 | 栈大小字节（宏定义） | 优先级 | 固定核心 | 创建位置 文件:行 | 创建条件 / 生命周期 | 是否自删 | 深调用链评估 |
|---|---|---|---|---|---|---|---|
| LVGL | 8192（`EXAMPLE_LVGL_TASK_STACK_SIZE (8*1024)`，lvgl_port.c:32） | 2 | **0** | core/ui/lvgl_port.c:271（`lvgl_port_init`） | 开机即建（app_main→lvgl_port_init），常驻；息屏时 `vTaskSuspend`、亮屏 `vTaskResume` | 否 | 深：`lv_timer_handler` 内执行全部 LVGL 回调 + 刷屏 `esp_lcd_panel_draw_bitmap`（QSPI DMA 链）；持锁期间还承担 `display_set_brightness`/`lv_refr_now` 等与 SPI 总线冲突的调用 |
| touch_init | 4096（字面量 4096） | 5 | 不固定（`xTaskCreate`） | core/hardware/touch.c:119（`touch_init`） | 开机即建，一次性：完成 `esp_lcd_touch_new_i2c_*`（含约 200ms×2 复位等待）后即退出 | 是（函数尾 `vTaskDelete(NULL)`，touch.c:71） | 中深：触摸控制器组件初始化链（I2C 复位/读 ID），约 400ms+ 即结束 |
| button_svc | 4096（`BUTTON_TASK_STACK (4*1024)`，button_service.c:51） | 4 | **0** | core/hardware/button_service.c:239（`button_service_init`） | 开机即建，常驻；每轮 poll 去抖 + 事件回调分发（key_handler：音量/息屏/伪关机） | 否 | 浅：只做浅投递；关机/息屏重活经 `deep_worker_call` 委托。历史曾在 3KB 栈直接跑关机链爆栈（"A stack overflow in task button_svc"），故现在委托 + 4KB |
| deep_worker | 8192（`DEEP_WORKER_TASK_STACK (8*1024)`，deep_worker.c:21） | 6 | **0** | core/utils/deep_worker.c:67（`deep_worker_init`） | 开机即建，常驻（全项目深链唯一执行器，信号量驱动） | 否 | **极深（设计上专扛）**：`esp_pm_configure`、`display_sleep_exit`（含面板重初始化 `esp_lcd_panel_init` 重链 + 120ms 等待）、`lv_refr_now` 全屏同步重刷、伪关机停外设（cs43131 复位/touch_power_off/sd_card_deinit）+ `esp_deep_sleep_start`。注释：8KB 为历史实测安全值 |
| sleep_timer | 4096（`SLEEP_TASK_STACK 4096`，sleep_timer.c:20） | 4 | **1** | core/utils/sleep_timer.c:130（`sleep_timer_init`） | 开机即建，常驻；500ms 轮询，到点 `fake_shutdown_enter`（浅投递） | 否 | 中：音量渐降直调 `audio_set_volume_level`（走 I2C 驱动，注释称互斥+中断路径栈开销大，2048B 曾爆栈） |
| screen_sleep | 4096（`SCREEN_SLEEP_TASK_STACK (4*1024)`，screen_sleep.c:31） | 1 | **1** | core/utils/screen_sleep.c:175（`screen_sleep_init`） | 开机即建，常驻；每秒超时检测，触发息屏/暂停超时关机（均浅投递 deep_worker） | 否 | 浅（重活全委托 deep_worker；注释实测栈峰值约 2KB，历史 2KB 直接内跑 `esp_pm_configure` 曾爆栈） |
| mem_monitor | 4096（`MEM_MONITOR_TASK_STACK 4096`，performance_monitor.c:11） | 2 | 不固定（`xTaskCreate`） | core/utils/performance_monitor.c:155（`startPerformanceMonitor`） | 开机即建（`CONFIG_PERFORMANCE_MONITOR=y`），常驻；每 2s 打印 heap/iram，每 20s 打印任务栈 HWM | 否 | 浅（heap_caps 查询 + ESP_LOG） |
| cpu_coll0 / cpu_coll1 | 各 1024（`CPU_COLLECTOR_STACK 1024`，performance_monitor.c:14） | 1 | 0 / 1 | core/utils/performance_monitor.c:97 与 :100（mem_monitor_task 内） | mem_monitor 创建时各建一个，常驻；每核一个采集空闲计数器 | 否 | 浅 |
| player_ctrl | 4096（`PLAYER_TASK_STACK 4096`，player_controller.c:24） | 5 | **0** | core/player/player_controller.c:574（`player_controller_init`） | **仅当开机走"播放列表数据有效→正常界面"路径才建**（boot_flow.c:281 调用）；常驻；rescan 时 `vTaskSuspend` 挂起（不删除），随后重启 | 否 | 中浅：命令队列消费，调 `mp3_player_play/stop`、NVS 读写（config_store） |
| mp3_play | 8192（`MP3_PLAY_TASK_STACK 8192`，mp3_player.c:31） | 8 | **1** | core/hardware/audio/mp3_player.c:604（`mp3_player_play`） | **播放时才建（每曲一个）**；播完/停止后自删 | 是（`vTaskDelete(NULL)`，mp3_player.c:541 及错误分支） | 深：Helix `MP3Decode` 解码循环 + SD fread + mainbuf memmove；读缓冲/输出缓冲在静态区（不占本栈），栈主要扛解码/库调用链 |
| mp3_out | 4096（`MP3_OUT_TASK_STACK 4096`，mp3_player.c:37） | 9 | **1** | core/hardware/audio/mp3_player.c:330（`ensure_output_init`） | **首次播放时懒建一次**，之后常驻不删；停止只置标志等通知 | 否 | 中：ringbuf→`audio_pcm_write`→I2S DMA 写（转换缓冲 pcm_buf 为静态 4KB） |
| audio_tone | 4096（`AUDIO_TONE_TASK_STACK 4096`，audio.c:23） | 8 | **1** | core/hardware/audio/audio.c:250（`audio_play_tone`） | 仅调用 `audio_play_tone` 才建；**当前 main/ 下无任何调用方 → 正常运行不创建** | 是（停止后 `vTaskDelete(NULL)`，audio.c:113） | 浅中：正弦查表 + `i2s_channel_write` 阻塞循环 |
| boot_scan | 8192（`BOOT_SCAN_STACK_WORDS (32768u/4u)`，boot_flow.c:38）⚠ 见下注 | 5（字面量） | **1** | ui/boot_flow.c:242（`boot_show_scanning`） | **仅当播放列表数据无效且用户点"开始扫描"才建**；扫描完自删 | 是（`vTaskDelete(NULL)`，boot_flow.c:327） | 深：`playlist_build_from_sd` 全盘扫描 + 组装 + 写 pldata 分区（大构建缓冲在 PSRAM heap；栈上仅 full_path 等中小数组）。⚠ 宏名/注释称"32KB 内部 RAM 栈"，但按本 port 字节语义实传 **8192B**——需复核（见结论建议 1） |
| rescan | 8192（`RESCAN_TASK_STACK (8*1024)`，settings_menu.c:328） | 6 | **1** | ui/settings_menu.c:373（`rescan_confirm_cb`） | 设置页"重新扫描"确认后临时创建；流程以 `esp_restart` 结束（其后 `vTaskDelete` 不可达） | 否（重启） | 深：`lvgl_port_suspend` + `player_controller_deinit`（含停播等待≤1s）+ `playlist_store_invalidate`（擦分区）+ `esp_restart` |
| cover_load | 8192（`COVER_TASK_STACK 8192`，now_playing.c:86） | 1 | **1** | ui/now_playing.c:645（`now_playing_create`） | **进入正常主界面即建一次**（main_ui_create→now_playing_create），常驻不删；任务栈上另有 `char path[512]` | 否 | 深：`song_meta_load` 读 .npd + `cover_jpeg_decode`（tjpgd JPEG 解码，460×460 帧缓冲落 PSRAM）+ 持锁换图 |

**注（核心布局与文档差异）**：本代码的实际核心绑定为——**Core0：LVGL / button_svc / deep_worker / player_ctrl / cpu_coll0；Core1：mp3_play / mp3_out / audio_tone / cover_load / sleep_timer / screen_sleep / boot_scan / rescan / cpu_coll1**；touch_init、mem_monitor 不固定。这与 AGENTS.md §6 描述的布局（UI 在 Core1、播放在 Core0）相反，**以当前代码为准**（audio.c:25 注释亦写"LVGL 固定在 Core0，音频放 Core1"）。

**应用任务栈合计（字节）**：
- 常驻 11 个（LVGL/button/deep_worker/sleep_timer/screen_sleep/mem_monitor/cpu_coll×2/player_ctrl/cover_load/mp3_out）= **51,200**（约 50KB，正常开机即有卡路径）。
- 播放中再加 mp3_play 8,192 → 峰值约 **59,392**（约 58KB）。
- 临时/一次性：touch_init 4,096（开机初期 ~0.4s）、boot_scan 8,192（仅重扫场景）、rescan 8,192（仅手动重扫→重启）、audio_tone 4,096（无调用方，一般不出现）。

### 1.2 ESP-IDF / FreeRTOS 系统任务（sdkconfig 或 IDF 默认，标注"系统默认"）

| 任务 | 栈大小（来源） | 优先级 | 核心 | 说明 |
|---|---|---|---|---|
| idle task ×2（每核一个） | 1,536/个（`CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536`） | 0 | 各核一个 | 调度器自动创建，系统默认 |
| esp_timer | 3,584（`CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584`） | 系统默认（较高，esp_timer 内部任务） | CPU0（`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y`） | 系统默认；LVGL tick 等全部 esp_timer 经它回调 |
| main（app_main） | 8,192（`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`） | 1（系统默认） | CPU0（AFFINITY=0x0） | 引导期任务；app_main 返回后删除 |
| sys_evt（默认事件循环） | 2,304（`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`） | 系统默认 | 任意 | 仅当 `esp_event_loop_create_default` 被调用才建；本项目 main/ 未见显式使用（无 WiFi/netif），通常不创建 |
| IPC 任务（ipc0/ipc1） | 1,280/个（`CONFIG_ESP_IPC_TASK_STACK_SIZE=1280`） | 系统默认 | 各核一个 | IDF5.x 已改为 ISR 型 IPC（`CONFIG_ESP_IPC_ISR_ENABLE=y`），任务一般不创建（配置为历史兼容） |
| Tmr Svc（FreeRTOS 软件定时器服务） | 2,048（`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`） | 1（`CONFIG_FREERTOS_TIMER_TASK_PRIORITY=1`） | 任意（NO_AFFINITY） | 内核首个软件定时器创建时才启动；本项目未见 xTimerCreate 使用点，可能不创建 |
| pthread（如使用） | 3,072（`CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072`） | 5 | 任意 | 系统默认，仅当调用 pthread 接口 |

---

## 二、内存占用清单

### 2.1 静态链接占用（esp_idf_size --archives + size -A + nm 实测）

**esp_idf_size 官方汇总**（build/releasecandidate/FakePodNanoNightPlayer.map）：
```
Used static IRAM:   16384 bytes (  0 remain, 100.0% used)   ← 工具归并口径，见下方说明
      .text: 15356 / .vectors: 1028
Used stat D/IRAM:  157635 bytes (184125 remain, 46.1% used)  ← 含 .data+.bss+.text 归并（含链接对齐保留区口径）
      .data: 16508 / .bss: 49592 / .text: 91535
Used Flash size: 4035368 bytes（.text 483832 + .rodata 3551280）
```

**size -A 分段实测（更可信口径）**：

| 段 | 大小 | 地址 | 性质 |
|---|---|---|---|
| .iram0.vectors + .iram0.text | 1,028 + 106,891 ≈ 107,919 | 0x4037_4xxx | 内部 SRAM 指令侧（代码/向量），**独立于堆**，非变量 |
| .dram0.data | 16,508 | 0x3fca_0xxx | 内部 DRAM 已初始化变量 |
| .dram0.bss | 49,592 | 0x3fcb_0xxx | 内部 DRAM 零初始化变量 |
| .flash.text | 483,832 | 0x4200_xxxx | flash XIP 代码，不占 SRAM |
| .flash.rodata | 3,551,280 | 0x3c… | flash 映射只读数据（字体点阵/默认封面/JPEG 表等），**不算 SRAM**（总量约 3.39MB） |

> 说明：esp_idf_size 的"IRAM 16,384B / 100% used"与 "D/IRAM .text 91,535B" 属其归档/归并口径（与 --archives 的 IRAM0 .text 列同源），与 size -A 的分段数字不一致，**勿据此判断 IRAM 告警**；变量静态账以 size -A + nm 为准：**内部 RAM 静态（.data+.bss）合计 = 66,100B ≈ 64.6KB**。

**按库的 .data+.bss Top 10（内部 RAM 静态大头，esp_idf_size --archives 列 .data/.bss 列求和）**：

| 排名 | 库 | .data (B) | .bss (B) | .data+.bss 合计 (B) | 主要来源 |
|---|---|---|---|---|---|
| 1 | libmain.a | 133 | 45,347 | **45,480** | 音频静态缓冲（见 2.2 符号表，约 43.3KB）+ 各模块小变量 |
| 2 | libhal.a | 5,931 | 4 | 5,935 | 驱动 HAL 静态表 |
| 3 | libfreertos.a | 3,108 | 776 | 3,884 | 内核任务列表/队列静态 |
| 4 | libspi_flash.a | 2,219 | 28 | 2,247 | SPI flash 驱动 |
| 5 | liblvgl__lvgl.a | 146 | 1,163 | 1,309 | LVGL 全局（对象内存在堆，不在此） |
| 6 | libxtensa.a | 1,060 | 0 | 1,060 | CPU 移植层 |
| 7 | libesp_hw_support.a | 671 | 286 | 957 | 时钟/电源/中断支持 |
| 8 | libesp_system.a | 509 | 325 | 834 | 系统服务 |
| 9 | libc.a | 360 | 356 | 716 | stdio/环境 |
| 10 | libnewlib.a | 172 | 216 | 388 | newlib 运行时 |

（其余各库均 < 350B。flash 侧 .rodata 大头为 libmain.a 约 2.95MB——全量中文字库点阵等，属 flash 不计 SRAM。）

### 2.2 符号级：内部 DRAM（地址 0x3f…）最大的 .bss/.data 符号 Top 15（nm -S --size-sort）

| 地址 | 大小(B) | 段 | 符号 | 来源 |
|---|---|---|---|---|
| 3fca99cc | 16,384 | b | read_buf | mp3_player.c `MP3_READBUF_SIZE(16KB)` MP3 读缓冲 |
| 3fca3ef4 | 11,520 | b | silence_flush | audio.c I2S DMA 静音冲刷缓冲（6×240×2×32bit） |
| 3fca87cc | 4,608 | b | out_buf | mp3_player.c Helix 一帧解码输出（short[2304]） |
| 3fca75cc | 4,608 | b | stereo_buf | mp3_player.c 单声道→双声道转换缓冲 |
| 3fca2ef4 | 4,096 | b | pcm_buf | audio.c int16→int32 PCM 转换缓冲（512帧×2ch） |
| 3fc9f900 | 3,072 | D | port_IntStack | xtensa 中断栈（系统，非应用可优化） |
| 3fca6bf4 | 2,048 | b | sine_table | audio.c 正弦查表（仅 audio_play_tone 使用，当前无调用方） |
| 3fcadaec | 512 | b | s_path_buf | 播放路径快照缓冲 |
| 3fca2a84 | 500 | b | pxReadyTasksLists | FreeRTOS 内核 |
| 3fcae6d4 | 312 | B | __sf | newlib stdio |
| 3fcae348 | 256 | b | opa_table$0 | 触摸/渐变查表 |
| 3fc9ed18 | 256 | D | pow43_14 | (系统) |
| 3fcadcec | 256 | b | s_song_name | 歌曲名缓冲 |
| 3fca27e8 | 248 | b | s_log_cache | 日志缓存 |
| 3fc9f55c | 240 | D | _impure_data | newlib |

→ 音频路径静态缓冲合计约 **43.3KB**（read_buf 16K + silence_flush 11.5K + out_buf 4.5K + stereo_buf 4.5K + pcm_buf 4K + sine_table 2K + 小件），全部在内部 RAM .bss。

### 2.3 运行时已知动态大头

| 项目 | 内部 RAM 占用 (B) | 依据/说明 |
|---|---|---|
| **LVGL draw buffer ×2**（已知不可动） | **84,640**（2×42,320） | lvgl_port.c:228/230：`heap_caps_malloc(H_RES 460 × BUF_HEIGHT(=V_RES/10=46) × sizeof(lv_color_t))`，`MALLOC_CAP_DMA` → 只能落内部 RAM；LV_COLOR_DEPTH=16 → lv_color_t=2B → 每块 460×46×2 = 42,320B |
| 应用常驻任务栈（见 1.1 合计） | ≈51,200（播放态 +8,192） | 从内部堆动态分配 |
| 系统任务栈（esp_timer 3,584 + idle×2 3,072 + 按需项） | ≈6,656~13,000 | 内部堆 |
| 播放路径动态：Helix 解码器各结构块（`MP3InitDecoder` 内多个独立 malloc，单块 <16KB，按 SPIRAM_USE_MALLOC 规则优先吃内部堆；播放结束释放）、FATFS 缓冲 | **未实测**（估 20~40KB 波动） | esp-libhelix-mp3 用 libc malloc（real/buffers.c）；建议播放中抓 mem_monitor 的 iram 曲线校准 |
| LVGL 对象/样式/动画内存 | 未实测（估 10~30KB） | `CONFIG_LV_MEM_CUSTOM=y` → 走 stdlib malloc（默认内部优先，>16KB 自动转 PSRAM） |
| 驱动：I2S/SPI DMA 描述符与缓冲、UART、NVS 等 | 估几 KB~十几 KB | 内部堆 |

**PSRAM（不占内部 RAM 的动态大头，备查）**：PCM ringbuf 75KB（`MALLOC_CAP_SPIRAM`）、封面解码帧缓冲 ~423KB（460×460×2，cover_jpeg.c 显式 SPIRAM）、.npd 元数据/JPEG 数据缓冲（song_meta.c 显式 SPIRAM）、SD 扫描构建缓冲 ~3.7MB（playlist_builder.c 显式 SPIRAM）。

**main/ 下其它 heap_caps 显式分配（无其它 MALLOC_CAP_INTERNAL/DMA/8BIT 大块）**：均为 `MALLOC_CAP_SPIRAM`（见上）或默认 malloc；内部 DMA/8BIT 大分配仅 LVGL draw buffer 一处。

---

## 三、结论：内部 RAM（约 288KB 级动态堆池）主要被谁吃掉（估算）

> 口径：动态堆池 ≈288KB（perf monitor 打印参考值）；静态 .data+.bss 66.1KB 独立于堆。估算 ±20%，**以运行时 mem_monitor 的 iram 打印为准**。

| 去向 | 估算占用 (KB) | 备注 |
|---|---|---|
| LVGL draw buffer ×2 | ~84.6 | 已知不可动（MALLOC_CAP_DMA 必须内部） |
| 应用常驻任务栈 | ~50（播放态 ~58） | 常驻 11 任务；播放 +8.2KB |
| 系统任务栈（esp_timer/idle/main 引导期/按需项） | ~7~13 | 系统默认 |
| LVGL 对象内存 | ~10~30 | 未实测，随 UI 复杂度 |
| 播放路径动态（Helix 解码/FATFS） | ~20~40（仅播放时） | 未实测；播完释放 |
| 驱动/杂项 | ~5~15 | DMA 描述符、NVS、log 等 |
| **估算剩余空闲** | **~50~110（亮屏待机）／~30~90（播放中）** | ⚠ 需 mem_monitor 实测校准 |

**前 5 个值得关注的可优化点**（LVGL draw buffer 与音频 PCM 静态缓冲两项已定案不可动，不在建议内）：

1. **boot_scan 栈口径疑点（最优先核实）**：boot_flow.c:38 `BOOT_SCAN_STACK_WORDS (32768u/4u)` 注释称"内部 RAM 32KB"，但按本 port（StackType_t=uint8_t）实传 **8,192B**。若真意图 32KB 应传 32768。扫描历史能跑通说明 8KB 可能够用——请核对注释/宏名并统一（要么扩栈求稳，要么改注释并信任 8KB）。
2. **常驻 8KB 大栈按 HWM 实测缩栈**：LVGL / deep_worker / cover_load 三个 8KB 常驻栈，配合已有的 `mem_report_stack_hwm`（每 20s 打印），实测后若余量 ≥4KB 可各降到 6KB → 最多省 ~6KB 内部 RAM；deep_worker 注释已表明"实测栈高水位后可按需下调"。
3. **为播放态动态内存建立实测基线，必要时给 Helix 显式指定内存**：Helix 解码器多个 <16KB malloc 默认优先吃内部堆（播放期内部堆波动主力之一），先抓 mem_monitor 数据；若紧张可给解码器结构显式 `MALLOC_CAP_SPIRAM`（需实测性能影响）。
4. **release 配置关闭 `CONFIG_PERFORMANCE_MONITOR`**：可省 mem_monitor 4KB + cpu_coll×2 各 1KB（共 ~6.1KB 常驻栈）及 500ms ISR 频率采样定时器；debug/releasecandidate 保留。
5. **删除无调用方的 audio_play_tone 通路**：main/ 下无任何调用方，删掉可省 tone 任务定义 4KB 栈规格 + `sine_table` 2KB .bss + `gen_sine_table()` 开机计算，同时减小维护面（若日后要做按键提示音再恢复）。

**附注（系统级，无需动作）**：esp_idf_size 汇总里 "IRAM 16,384B/100% used" 与 size -A 的 .iram0.text 106,891B 不一致属工具归并口径差异；内部指令 RAM 代码（~107.9KB，0x4037…）是代码不是堆，不影响内存预算。flash .rodata ~3.39MB（中文字库为主）经 flash 缓存访问，不占 SRAM。
