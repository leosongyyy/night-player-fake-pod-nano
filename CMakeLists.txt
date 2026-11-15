# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# ESP-IDF 6.1 要求 cmake_minimum_required >= 3.16；旧值 3.5 在 CMake 4.x 下会
# 打 "Compatibility with CMake < 3.10 will be removed" 弃用警告。取 3.16 作为下限。
cmake_minimum_required(VERSION 3.16)
# 本项目自 2026-11 起固定面向 ESP32-S31（FakePodNano Pro V1）。
# ⚠ IDF 的构建目标由 CMake 变量 IDF_TARGET 决定，**不是** sdkconfig.defaults 里的
# CONFIG_IDF_TARGET（那一行只写进生成的 sdkconfig，不会反过来改构建目标）。
# 旧构建目录的 CMakeCache 里若残留 esp32s3，会继续用 xtensa 工具链与 S3 头文件，
# 表现为"GPIO_NUM_50+ 未定义"这类错误——必须在 include(project.cmake) 之前钉死目标。
set(IDF_TARGET "esp32s31")

# ============================================================================
# 组件剪裁（COMPONENTS 白名单）—— 2026-11 加入，动机：缩短**全量/冷编译**时间
# ----------------------------------------------------------------------------
# 不写 COMPONENTS 时 IDF 会把 COMPONENT_DIRS 里的**全部**组件都编一遍
# （本工程实测 155 个组件 / 1665 个编译单元），其中 wifi / bt / lwip /
# wpa_supplicant / esp_http_client / esp_https_ota / protocomm / openthread /
# unity / cmock 这些**本项目一行都不用**；mbedtls 更是个大件（153 个编译单元
# ≈ 9.2 %），却因为 bootloader_support / nvs_flash 的两处无条件依赖永远摘不掉
# （调研结论见 ideas_and_exps/MBEDTLS_BUILD_AUDIT.md，那里也解释了为什么
# "省 Flash"不是做这件事的理由——它只占镜像 2.5KB）。
#
# ⚠ 本列表是**编译集合**，不是依赖图：IDF 会自动补上所列组件自己的
#   REQUIRES/PRIV_REQUIRES 闭包，以及 "common requirements"（freertos / heap /
#   log / soc / esp_hw_support / esp_rom / newlib …），所以这里只需列
#   **本项目源码直接 #include 头文件的组件**。
# ⚠ 三个必须记住的坑（来自 MBEDTLS_BUILD_AUDIT.md §5-B）：
#   1) 不能只写 `main`（更不能只开 MINIMAL_BUILD）：`main` 的"没写 REQUIRES
#      就链接构建里全部组件"这条便利只在**构建内组件**范围内生效，一旦剪裁，
#      被剪掉组件的头文件立刻在 main 里找不到（nvs_flash / fatfs / sdmmc /
#      vfs / esp_partition … 都会缺）。所以下面逐个列出 main 用到的组件。
#   2) 本仓库自带的 4 个本地组件（AMOLED + 3 个触摸驱动）**必须显式列出**：
#      main 并不 REQUIRES 它们，今天是靠"全量构建 + main 链接全部组件"才编上。
#      另有 components/spi_flash（IDF 组件 fork，带 S31 分支预测补丁）：它是
#      esp_partition/nvs_flash 的依赖，由依赖图自动带入，不需要写进本列表；
#      但**不要删除该目录**（详见 ideas_and_exps/S31_FLASH_CACHE_RACE.md）。
#   3) 以后新增任何组件依赖，漏写进本列表就会**编译报错**（报错很明显，不算
#      隐蔽），按报缺失的头文件补一行即可。写完记得 `idfc build` 并做一次
#      全功能真机回归（SD/FATFS、NVS 配置、字体分区、音频、触摸、分区表）。
# ⚠ 必须写在 include(project.cmake) 之前（IDF 在 include 时就要算组件表）。
# ============================================================================
set(COMPONENTS
    main

    # ---- 本仓库的本地组件（见上方坑 2） ----
    esp_lcd_qspi_amoled            # core/hardware/display.c
    esp_lcd_touch_cst816           # core/hardware/touch.c（三型号运行时择优）
    esp_lcd_touch_cst820
    esp_lcd_touch_chsc6417
# ESP-DSP（2026-11-14 起改为**托管组件** espressif/esp-dsp 1.8.2，Apache-2.0，
    # 见 main/idf_component.yml；之前是 vendored 1.4.12 + CMake 裁剪）：
    # 蓝牙输出重采样用（core/hardware/audio/resampler.c 的 polyphase FIR
    # 点积调用 dsps_dotprod_f32）。⚠ 托管组件解包在 managed_components/
    # （gitignore），但**必须**列进本白名单，否则 main 里
    # #include "dsps_dotprod.h" 找不到头文件。
    # 实测 S31（RISC-V）上 1.8.2 可完整编译（174 个编译单元；S31 的 Kconfig 把
    # 实现选成 ANSI C，Xtensa 汇编在源文件内部被条件编译掉了）。
    esp-dsp
    tremor                         # OGG Vorbis 定点解码（audio_decoder.c），依赖 libogg 自动带入

    # ---- main 直接 include 头文件的 IDF 组件 ----
    esp_lcd                        # esp_lcd_panel_*.h / esp_lcd_panel_io.h
    esp_driver_gpio                # driver/gpio.h（按键、触摸 INT/RST、DAC RST）
    esp_driver_i2c                 # driver/i2c_master.h（触摸/CS43131）
    esp_driver_i2s                 # driver/i2s_std.h（音频输出）
    esp_driver_spi                 # driver/spi_master.h（QSPI 面板）
    esp_driver_sdmmc               # driver/sdmmc_host.h（SD 卡）
    esp_driver_ppa                 # driver/ppa.h（PPA 2D 加速，core/ui/lvgl_ppa_accel.c）
    esp_driver_jpeg                # driver/jpeg_decode.h（封面硬件 JPEG 解码，core/image/cover_jpeg.c）
    esp_mm                         # esp_cache.h（PPA 缓冲对齐/缓存同步）
    esp_partition                  # esp_partition.h（font 分区 mmap、pldata）
    esp_pm                         # esp_pm.h（亮屏 320MHz / 息屏 80MHz）
    esp_timer                      # esp_timer.h（各任务的周期定时）
    nvs_flash                      # nvs_flash.h / nvs.h（config_store）
    fatfs                          # esp_vfs_fat.h（SD 卡挂载，exFAT 需 IDF 本体打补丁）
    vfs                            # 文件系统注册（fatfs 依赖，直接包含亦稳妥）
    sdmmc                          # sdmmc_cmd.h / sd_protocol_defs.h（SD 卡）

    # ---- 网络对时（2026-11 新增 → 只给 sync 固件用） ----
    # 时间与日期 → WiFi对时：WiFi STA 连 AP + SNTP。
    # ⚠ **2026-11 改为双固件方案**（见 ideas_and_exps/INNER_WORLD.md）：
    #   WiFi 协议栈不再进播放器固件（它常驻约 27KB 内部 RAM + 65KB 运行时峰值，
    #   是蓝牙之后 UI 帧率的主要挤压来源），而是放进独立的「对时固件」（烧在
    #   ota_0，构建变体 `sync`）。播放器只在需要时用 esp_ota_set_boot_partition()
    #   切过去，对时固件写完 RTC 再切回 factory。
    #
    # ⚠ 为什么这 5 个组件**必须无条件**留在白名单里（不能按角色裁剪）：
    #   本 `COMPONENTS` 列表写在 `include(project.cmake)` 之前，那时 Kconfig
    #   还没加载，拿不到 CONFIG_FIRMWARE_ROLE_SYNC。而组件"被编进构建"与
    #   "被链接进镜像"是两件事：播放器固件没有任何代码引用 esp_wifi/lwip，
    #   静态库成员不会被拉进最终镜像 → **Flash 与内部 RAM 占用为 0**，
    #   代价只是播放器构建时多编译这些组件（一次性的全量编译时间）。
    #   实测：加回这 5 个组件后播放器镜像大小/静态 RAM 与下线时**逐字节一致**
    #   （见 INNER_WORLD.md §7 的验收记录）。
    esp_wifi                       # esp_wifi.h（STA 连接）
    esp_netif                      # esp_netif.h（默认 STA netif / DHCP）
    esp_event                      # esp_event.h（WiFi/IP 事件）
    lwip                           # esp_sntp.h / lwip/netdb.h（SNTP + DNS）
    wpa_supplicant                 # WPA2/WPA3 握手（esp_wifi 的实现依赖）
    # OTA 槽切换（双固件方案的核心）：esp_ota_set_boot_partition() 写 otadata。
    # 播放器用它切到里世界固件、里世界固件用它切回 factory（= 擦 otadata）。
    # 静态占一点 Flash（esp_ota_ops + esp_image_verify），RAM 可忽略。
    app_update                     # esp_ota_ops.h（esp_ota_set_boot_partition）

    # ---- 里世界固件的 USB 传歌（2026-11 新增 → 只给 sync 固件用） ----
    # USB Mass Storage：把 SD 卡原始块设备通过 TinyUSB MSC 暴露给电脑当 U 盘。
    # 托管组件 espressif/esp_tinyusb（见 main/idf_component.yml），提供
    # tinyusb.h / tinyusb_msc.h / tinyusb_default_config.h。
    # ⚠ 与上面 WiFi 那组同理**必须无条件**留在白名单里：本列表在
    #   include(project.cmake) 之前执行，拿不到 CONFIG_FIRMWARE_ROLE_SYNC；
    #   而"编进构建"不等于"链接进镜像"——播放器没有任何代码引用 tinyusb，
    #   静态库成员不会被拉进最终镜像（Flash/RAM 占用为 0）。
    esp_tinyusb                    # USB MSC（里世界固件的 USB 传歌）

    # ---- 经典蓝牙音频（2026-11 新增，core/hardware/bt/） ----
    # A2DP Source（SBC，内部编码器）+ AVRCP CT（绝对音量）。Classic 只能走
    # Bluedroid + BTDM 控制器（见 sdkconfig.defaults 的 BT 段）。
    # ⚠ 这里必须列 bt：main 直接 include esp_bt.h / esp_a2dp_api.h /
    #   esp_avrc_api.h / esp_gap_bt_api.h。
    bt                             # BlueDroid host + BTDM controller
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")

# 应用版本（写入 esp_app_desc_t，启动日志 / idf.py size / OTA 会读取）。
# 不设置时 ESP-IDF 用 `git describe --tags`，会得到 "0.9.0.0_alpha_5-25-gb8c87c4-dirty"
# 这类字符串，与设置页「关于」显示的 CONFIG_FIRMWARE_VERSION_STRING 不一致。
set(PROJECT_VER "2.0.0.0-alpha-1")

project(FakePodNanoNightPlayer)

# ============================================================================
# bt 组件的编译选项（2026-11，经典蓝牙音频接入）
# ----------------------------------------------------------------------------
# IDF 6.1 的 Bluedroid 里有若干 `BCM_STRNCPY_S(..., BD_NAME_LEN)`（把 248 字节
# 名字写进 248 字节缓冲）；GCC 15 的 -Wstringop-truncation 会报"可能截断"，
# 而 IDF 默认 -Werror 把它当错误。这不是本项目的代码，也不影响运行
# （BT 名字本来就允许不以 '\0' 结尾，Bluedroid 内部按长度处理）。
# 只对 bt 组件关掉这一条警告，不动全局的 CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS。
# ⚠ 必须判断 CONFIG_BT_ENABLED：对时固件（构建变体 sync）把它关掉后，bt 组件会
#   注册成 **INTERFACE 库**（没有任何源文件），而 INTERFACE 目标只允许设置
#   INTERFACE 属性——直接 target_compile_options(... PRIVATE ...) 会在配置阶段报
#   "target_compile_options may only set INTERFACE properties on INTERFACE targets"。
# ============================================================================
if(CONFIG_BT_ENABLED)
    idf_component_get_property(_bt_lib bt COMPONENT_LIB)
    if(TARGET ${_bt_lib})
        target_compile_options(${_bt_lib} PRIVATE -Wno-stringop-truncation)
    endif()
endif()
