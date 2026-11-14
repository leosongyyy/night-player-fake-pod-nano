# 组件源码（固定部分）
set(MAIN_SRCS
    "main.c"
    "core/hardware/display.c"
    "core/hardware/touch.c"
    "core/hardware/axp2101.c"
    "core/hardware/boot_reason.c"
    "core/hardware/rtc.c"
    "core/hardware/battery.c"
    "core/hardware/button_service.c"
    "core/hardware/sd_card.c"
    "core/hardware/pldata.c"
    "core/hardware/audio/audio.c"
    "core/storage/playlist_store.c"
    "core/storage/playlist_builder.c"
    "core/storage/playlist.c"
    "core/storage/config_store.c"
    "core/storage/config_slots.c"
    "core/hardware/audio/cs43131.c"
    "core/hardware/audio/mp3_player.c"
    "core/hardware/audio/eq.c"
    "core/network/wifi_time_sync.c"
    "core/player/player_controller.c"
    "core/player/shuffle_player.c"
    "core/ui/lvgl_port.c"
    "core/ui/lvgl_mem.c"
    "core/ui/lvgl_ppa_accel.c"
    "core/ui/touch_test.c"
    "core/ui/virtual_list.c"
    "core/ui/click_wheel.c"
    "core/ui/global_style.c"
    "core/utils/fake_shutdown.c"
    "core/utils/deep_worker.c"
    "core/utils/task_psram.c"
    "core/utils/power_service.c"
    "core/utils/sleep_timer.c"
    "core/fonts/system_smaller_font.c"
    "core/fonts/system_global_font_lite.c"
    "core/fonts/font_npf.c"
    "core/fonts/system_font.c"
    "core/image/default_cover.c"
    "core/image/cover_jpeg.c"
    "core/music/song_meta.c"
    "ui/app.c"
    "ui/battery_indicator.c"
    "ui/boot_flow.c"
    "ui/clock_display.c"
    "ui/clock_text.c"
    "ui/dir_list.c"
    "ui/key_handler.c"
    "ui/list_common.c"
    "ui/main_ui.c"
    "ui/now_playing.c"
    "ui/notification_panel.c"
    "ui/player_ticker.c"
    "ui/playlist_page.c"
    "ui/settings_menu.c"
    "ui/settings_page.c"
    "ui/eq_popup.c"
    "ui/config_slot_popup.c"
    "ui/time_date_popup.c"
    "ui/time_sync_ui.c"
    "ui/toast.c")

# 性能监视（内存 + 双核 CPU 使用率，串口每 2s 输出）仅当 CONFIG_PERFORMANCE_MONITOR
# 开启时编译；关闭可减小固件并省掉监视任务的内存开销（依赖
# CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS，见 sdkconfig.defaults）。
if(CONFIG_PERFORMANCE_MONITOR)
    list(APPEND MAIN_SRCS "core/utils/performance_monitor.c")
endif()

# 内存快照诊断（CONFIG_MEM_TRACE，release 关闭）：启动各阶段与封面加载打印
# 内部 RAM / DMA / PSRAM 占用，用于定位内存消耗者（见 IRAM_DMA_AUDIT.md）。
# 关闭时不编译本文件，且 mem_trace.h 会把所有调用点退化为空语句。
if(CONFIG_MEM_TRACE)
    list(APPEND MAIN_SRCS "core/utils/mem_trace.c")
endif()

# 系统字体：NPF1 全字库在 font 分区里（运行时 mmap，见 core/fonts/font_npf.c），
# 固件内只编译精简字库做兜底（system_global_font_lite）——两者都常驻 MAIN_SRCS。
# 20px 小字体（system_smaller_font）也常驻，供状态栏时间/两行文字图标使用。
# ⚠ 不再有 FreeType / TTF：字体是预烘焙的位图（bpp2），由 core/fonts/system_font.c
#   在 NPF 与内嵌精简字库之间二选一，详见 ideas_and_exps/NPF_FONT.md。

# 极速版默认封面（RGB565 真彩色，423KB）仅当 CONFIG_UI_COVER_FLASH 开启时编译；
# 关闭时用 2bit 索引色版 default_cover.c（52KB，省空间、渲染略慢）。
if(CONFIG_UI_COVER_FLASH)
    list(APPEND MAIN_SRCS "core/image/default_cover_flash.c")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "core" "ui")

# ============================================================================
# 构建期补丁：回写 patches/ 下针对**托管组件**的修改
# ----------------------------------------------------------------------------
# ⚠ 为什么需要"构建期"应用：托管组件位于 managed_components/，由
#   idf-component-manager 拉取；本机 3.1.2 **不支持**原生的 patches/ 机制
#   （已实测搜索，包内无任何 patch 相关代码），直接改那里的源文件会在下次
#   组件管理器更新时被覆盖。
#   做法：把**改好的整份文件**放在 patches/<补丁集>/ 下，目录结构与组件内
#   一致；每次 CMake 配置阶段逐文件比对内容，**不一致才回写**（幂等，不会
#   造成无谓重编）。日后上游合入了对应修复，比对结果相同 → 自动变成空操作，
#   届时可把对应条目删掉。
# ============================================================================

# ---- 补丁集 1：LVGL 的 IRAM_ATTR 段号冲突（IDF 6.1 + GCC 15） --------------
# 现象（2026-11 迁移到 IDF 6.1 时的第一个构建失败点）：
#   lvgl/src/misc/lv_color.c:317:1: error: ignoring attribute 'section (".iram1.4")'
#   because it conflicts with previous 'section (".iram1.1")' [-Werror=attributes]
#   （lv_color_op.c / lv_math.c 同样报，均为 "头文件里声明 + .c 里定义"）
#
# 根因（两处变更叠加，都不是 LVGL 的设计问题，而是**漏改**）：
#   · IDF 6.1 把 IRAM_ATTR 改成带 __COUNTER__ 的唯一子段
#     （esp_common/include/esp_attr.h）：
#         #define IRAM_ATTR _SECTION_ATTR_IMPL(".iram1", __COUNTER__)
#     于是同一个函数在**声明**处展开成 .iram1.1、在**定义**处展开成 .iram1.4，
#     两处段名不同。
#   · IDF 6.1 起 CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS 默认 n，GCC 15 的
#     -Wattributes 从警告升级为错误。
#
# 上游修法（本补丁就是它）：lvgl/lvgl PR #10737
#   "fix(attributes): remove LV_ATTRIBUTE_FAST_MEM from declarations"
#   —— 按 lvgl#4391 早已定下的约定（属性只留在**定义**和头文件里的
#   static inline 定义上，**不留在声明上**），把 3 处声明上的属性注释掉。
#   ⚠ 关键：**不动任何定义**，所以函数照样被放进 IRAM（实测 map/objdump：
#     lv_color_16_16_mix 落在 .iram1.1 → 最终 .iram0.text，地址 0x403793e4，
#     与补丁前逐字节一致），**零性能损失**，比"关掉 FAST_MEM"或"降级警告"干净。
#   证据：同一文件里 lv_math.h:53 的 lv_trigo_sin 本来就是注释形式
#     （int32_t /* LV_ATTRIBUTE_FAST_MEM */ lv_trigo_sin(...)），只有
#     lv_trigo_cos 漏了——印证这是漏改而非设计。
#   ⚠ 上游 9.6 仍未合入该 PR（2026-11 查证），故暂时无法靠升级解决。

# ---- 补丁集 2：esp_lcd_touch 的组件依赖 ------------------------------------
# IDF 6.0 起旧 `driver` 组件不再**公共依赖** esp_driver_gpio 等 18 个子组件，
# 而 esp_lcd_touch 一边 include "driver/gpio.h"（连公开头文件里都用 gpio_num_t）、
# 一边大量调用 gpio_install_isr_service / gpio_intr_enable / gpio_isr_handler_add /
# gpio_isr_handler_remove / gpio_intr_disable，CMakeLists 里却只写
# `REQUIRES "driver" "esp_lcd"`。当前能编过纯粹是 esp_lcd 转发依赖的结果，
# 上游 esp_lcd 一旦收窄依赖即编译失败。补成显式 PRIV_REQUIRES esp_driver_gpio。
# （本项目本地那 3 个触摸驱动 + esp_lcd_qspi_amoled 有同样的隐患，已经直接改在
#   仓库内它们的 CMakeLists 上——那些组件本身就在版本控制里，无需补丁。）

# ⚠ 内层文件清单用**逗号**分隔，不能用分号：CMake 的 list 语义里分号就是分隔符，
#   无论怎么转义，外层 foreach 展开时都会被拆开（这里踩过一次：文件清单被拆成
#   独立元素，list(GET ... 2) 取不到东西）。故此处用 "|" 分层、"," 分文件。
set(_patch_sets
    "lvgl_fast_mem_decl|managed_components/lvgl__lvgl|src/misc/lv_color.h,src/misc/lv_color_op.h,src/misc/lv_math.h"
    "esp_lcd_touch_deps|managed_components/espressif__esp_lcd_touch|CMakeLists.txt")

foreach(_set ${_patch_sets})
    string(REPLACE "|" ";" _set_parts "${_set}")
    list(GET _set_parts 0 _set_name)
    list(GET _set_parts 1 _set_component)
    list(GET _set_parts 2 _set_files)
    string(REPLACE "," ";" _set_files "${_set_files}")
    set(_root "${CMAKE_CURRENT_SOURCE_DIR}/../patches/${_set_name}")
    set(_patched 0)
    foreach(_rel ${_set_files})
        set(_src "${_root}/${_rel}")
        set(_dst "${CMAKE_CURRENT_SOURCE_DIR}/../${_set_component}/${_rel}")
        if(NOT EXISTS "${_src}")
            message(WARNING "补丁源文件缺失，跳过：${_src}")
        elseif(NOT EXISTS "${_dst}")
            message(WARNING "补丁目标不存在，跳过（组件版本可能已变）：${_dst}")
        else()
            # ⚠ 换行符归一化后再比较：本仓库 core.autocrlf=true，patches/ 下的
            #   补丁源（受 git 管理）与 managed_components/ 下的目标（gitignore、
            #   由组件管理器按 LF 解包）**换行符可能不一致**。CMake 的 file(READ)
            #   当前会归一化换行，所以裸比较也不会误判；但那样就依赖了未文档化的
            #   行为与各人的 git 配置——一旦某台机器上不归一化，就会**每次配置都
            #   回写**（表现为 lvgl 每次全量重编）。故显式去掉 \r 再比、也写归一化
            #   内容，让"是否相同"只取决于代码本身。
            file(READ "${_src}" _src_content)
            file(READ "${_dst}" _dst_content)
            string(REPLACE "\r" "" _src_norm "${_src_content}")
            string(REPLACE "\r" "" _dst_norm "${_dst_content}")
            if(NOT _src_norm STREQUAL _dst_norm)
                file(WRITE "${_dst}" "${_src_norm}")
                math(EXPR _patched "${_patched} + 1")
            endif()
        endif()
    endforeach()
    if(_patched GREATER 0)
        message(STATUS "构建期补丁：已回写 ${_patched} 个文件（${_set_name}）")
    else()
        message(STATUS "构建期补丁：已是最新，无需回写（${_set_name}）")
    endif()
endforeach()
unset(_patch_sets)
unset(_set_parts)
unset(_set_name)
unset(_set_component)
unset(_set_files)
unset(_root)
unset(_src)
unset(_dst)
unset(_src_content)
unset(_dst_content)
unset(_src_norm)
unset(_dst_norm)
unset(_patched)
unset(_rel)

# ============================================================================
# LVGL 9 自定义分配器（core/ui/lvgl_mem.c）的链接顺序修正
# ----------------------------------------------------------------------------
# Kconfig 选中 CONFIG_LV_USE_CUSTOM_MALLOC 后，LVGL 的 src/stdlib/lv_mem.c 会
# 引用外部符号 lv_malloc_core / lv_free_core / lv_realloc_core / lv_mem_init，
# 这些由本组件（main）的 core/ui/lvgl_mem.c 提供。
#
# 但项目的最终链接顺序是 `liblvgl__lvgl.a → libmain.a → liblvgl__lvgl.a`
# （IDF 的依赖排序：main 依赖 lvgl，于是把 lvgl 再重复一次）：
#   · 第一遍扫 liblvgl 时，没有任何"当前未定义"的符号需要它，所以一个 .o
#     都不会被拉进来；
#   · 接着扫 libmain.a：此时未定义集合来自 main 自己的目标文件（lv_init 等），
#     **不含 lv_malloc_core**（main 不直接调它），因此提供这些符号的 lvgl_mem.o
#     不会被拉进来；
#   · 第二遍扫 liblvgl 时才拉入 lv_mem.o，它这才产生 lv_malloc_core 等未定义
#     符号 —— 而 libmain.a 已经扫过，不会再回看，于是报 undefined reference。
# 实测报错：`undefined reference to 'lv_malloc_core' / 'lv_free_core' /
# 'lv_realloc_core' / 'lv_mem_init'`。
#
# 用 `-u`（--undefined）在链接开始前就把 lv_malloc_core 标为未定义，强制
# 链接器在扫 libmain.a 时就拉入 lvgl_mem.o（一个 .o 里的其它符号随之全进来）。
# 该选项在改回 CLIB/BUILTIN 分配器时也无害：那时符号由 lvgl 自己的
# stdlib/*/lv_mem_core_*.c 提供，同样能正常解析。
# ============================================================================
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")

# ============================================================================
# 系统字体（NPF1 位图全字库）——**不需要任何组件接线**
# ----------------------------------------------------------------------------
# 2026-11 从 TTF/FreeType 改回位图字体：字形位图与 glyph_dsc 都是离线预烘焙的
# 二进制（spiffs/system.npf → font 分区），运行时只做 esp_partition_mmap + 填
# lv_font_fmt_txt_dsc_t（见 core/fonts/font_npf.c）。因此这里**没有** FreeType
# 链接、没有 FT_CONFIG_OPTIONS_H、也没有 lv_ftsystem.c 的链接顺序问题——那一整段
# 历史接线（以及 ftoption_lvgl.h）已随 TTF 一起删除。
# ============================================================================

# ============================================================================
# PPA（ESP32-S31 的 2D 像素加速器）接线 —— LVGL 9.4 自带 PPA 绘制单元
# ----------------------------------------------------------------------------
# `CONFIG_LV_USE_PPA=y` 时 LVGL 的 src/draw/espressif/ppa/* 会：
#   1) #include "driver/ppa.h" / "esp_cache.h" / "hal/color_hal.h"；
#   2) 在 lv_draw_ppa_private.h 里做编译期对齐检查：
#        CONFIG_LV_ATTRIBUTE_MEM_ALIGN_SIZE == CONFIG_CACHE_L1_CACHE_LINE_SIZE
#   3) 在 lv_init() 里调用 lv_draw_ppa_init()（无需应用侧再初始化）。
#
# 但托管组件 lvgl 的 env_support/cmake/esp.cmake 虽然写了
#   PRIV_REQUIRES esp_driver_ppa esp_mm ...
# 在本工程（IDF 6.1 + 组件管理器）实测**没有把 include 目录传进 lvgl 的编译**
# （表现为 fatal error: driver/ppa.h: No such file or directory）。
# 所以这里像 FreeType 一样，手动把 PPA 组件链接给 lvgl 目标。
#
# ⚠ 第二个坑：LVGL 头里写的是 `CONFIG_CACHE_L1_CACHE_LINE_SIZE`（ESP32-P4 的
#   符号名），而 ESP32-S31 的 Kconfig 里叫 `CONFIG_CACHE_L1_DCACHE_LINE_SIZE`
#   （=64）。不补这个宏就会命中 #error "For using PPA buffers need to be
#   aligned to 64-byte boundary!"，即使两个 LVGL 对齐配置都已经设成 64。
# ============================================================================
if(CONFIG_LV_USE_PPA)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    idf_component_get_property(ppa_lib esp_driver_ppa COMPONENT_LIB)
    idf_component_get_property(mm_lib esp_mm COMPONENT_LIB)
    if(TARGET ${ppa_lib} AND TARGET ${mm_lib})
        target_link_libraries(${lvgl_lib} PRIVATE ${ppa_lib} ${mm_lib})
    else()
        message(WARNING "PPA 组件目标缺失：LV_USE_PPA 可能编译失败")
    endif()
    target_compile_definitions(${lvgl_lib} PRIVATE
        "CONFIG_CACHE_L1_CACHE_LINE_SIZE=${CONFIG_CACHE_L1_DCACHE_LINE_SIZE}")
endif()

# ============================================================================
# font 分区（partitions.csv 的 font @ 0x600000，6MiB）：字体**不由构建系统烧录**
# ----------------------------------------------------------------------------
# 2026-10-25 改版：这里原本用
#     esptool_py_flash_target(font-flash ...) + esptool_py_flash_to_partition(...)
# 注册一条 `idfc idfpy font-flash` 目标（"平时 idfc flash / 换字体才 font-flash"），
# 现已**整段移除**。原因两条：
#   · 字体是"装一次就不动"的数据，为它单独维护一条 ninja 目标太重；
#   · 那条目标还自带一串坑：`-p COMx` 传不进去 —— esptool 的端口是**烧录那一刻**
#     才读环境变量 `ESPPORT`（components/esptool_py/get_port_args.cmake 刻意不在配置期
#     展开），而 ninja 是 idfc 服务器拉起的，客户端设的 ESPPORT 同样传不过去，于是
#     esptool 只能逐个串口试（实测记录见 FONT_FLASH.md §7）。
# 现在：**分区表只保留位置**，字体由装机/换字体的人用手边的 esptool 手动刷一条命令
# （装固件的人本来就有 esptool，构建系统这边不再需要任何自定义目标）：
#     esptool.exe --chip esp32s31 --port COMx write_flash 0x600000 spiffs/system.npf
# 完整说明（何时必须刷、怎么校验刷成功、分区 6MiB 上限、IDF 环境下的等价命令、
# 历史坑）见 **ideas_and_exps/FONT_FLASH.md**。
# 运行期不受影响：font 分区仍是裸二进制、无文件系统，由 core/fonts/font_npf.c
# 做一次 esp_partition_mmap 后直接按指针读（不再有 lv_fs 驱动）。
# ⚠ 2026-11 分区重排：font 从 4MiB@0x800000 改成 6MiB@0x600000（装 4.44MiB 的
#   NPF1 全字库），factory 相应缩到 5.9375MB；**起始偏移变了 → 必须重刷字体**。
# ⚠ font 分区的内容不由构建系统烧录，本段只做提示。
# ============================================================================
# 只提示一次：字体不在固件里，"重刷分区表 / 换新机器 / 换字体"都要手动刷字体分区，
# 否则界面会静默回退内嵌位图字体（能开机，但生僻字、日文会缺字变方框）。
message(STATUS "提示：font 分区（0x600000）的内容需手动刷入 —— esptool write_flash 0x600000 spiffs/system.npf（见 ideas_and_exps/FONT_FLASH.md）")

set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE)
