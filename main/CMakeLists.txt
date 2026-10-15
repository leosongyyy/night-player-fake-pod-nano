# 组件源码（固定部分）
set(MAIN_SRCS
    "main.c"
    "core/hardware/display.c"
    "core/hardware/touch.c"
    "core/hardware/battery.c"
    "core/hardware/button_service.c"
    "core/hardware/sd_card.c"
    "core/hardware/pldata.c"
    "core/hardware/audio/audio.c"
    "core/storage/playlist_store.c"
    "core/storage/playlist_builder.c"
    "core/storage/playlist.c"
    "core/storage/config_store.c"
    "core/hardware/audio/cs43131.c"
    "core/hardware/audio/mp3_player.c"
    "core/hardware/audio/eq.c"
    "core/player/player_controller.c"
    "core/player/shuffle_player.c"
    "core/ui/lvgl_port.c"
    "core/ui/lvgl_mem.c"
    "core/ui/touch_test.c"
    "core/ui/virtual_list.c"
    "core/ui/click_wheel.c"
    "core/ui/global_style.c"
    "core/utils/fake_shutdown.c"
    "core/utils/deep_worker.c"
    "core/utils/task_psram.c"
    "core/utils/power_service.c"
    "core/utils/battery_logger.c"
    "core/utils/sleep_timer.c"
    "core/fonts/system_smaller_font.c"
    "core/fonts/system_global_font_lite.c"
    "core/fonts/font_ttf.c"
    "core/image/default_cover.c"
    "core/image/cover_jpeg.c"
    "core/music/song_meta.c"
    "ui/app.c"
    "ui/boot_flow.c"
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

# 完整版系统字体体积很大（固件约 4.1MB、烧录慢），仅当 CONFIG_UI_SYSTEM_FONT_FULL
# 开启时才编译；关闭时改用精简版 system_global_font_lite，便于调试快速烧录。
# ⚠ 2026-10 起界面字体默认走 TTF + FreeType（CONFIG_UI_FONT_TTF=y），此时本项被
#    Kconfig 的 depends on !UI_FONT_TTF 屏蔽，完整版字库根本不参与构建；
#    system_global_font_lite 则**始终编译**——它是 TTF 打不开时的回退字体。
# ⚠ 曾尝试 SPIFFS 运行时加载（font_store），实测解析 ~8 秒开机太慢已回退。
if(CONFIG_UI_SYSTEM_FONT_FULL)
    list(APPEND MAIN_SRCS "core/fonts/system_global_font.c")
endif()

# 极速版默认封面（RGB565 真彩色，423KB）仅当 CONFIG_UI_COVER_FLASH 开启时编译；
# 关闭时用 2bit 索引色版 default_cover.c（52KB，省空间、渲染略慢）。
if(CONFIG_UI_COVER_FLASH)
    list(APPEND MAIN_SRCS "core/image/default_cover_flash.c")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "core" "ui")

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
# FreeType（TTF 字体）接线
# ----------------------------------------------------------------------------
# LVGL 的 env_support/cmake/esp.cmake 在 LV_USE_FREETYPE=y 时只把
# src/libs/freetype/*.c 加进编译，**不加任何依赖**，而 lv_freetype_private.h 里是
#     #include "ft2build.h"
#     #include FT_FREETYPE_H / FT_GLYPH_H / FT_CACHE_H …
# 所以必须由外部把 espressif__freetype **链接给 lvgl 组件**（不只是链接给 main）：
# 只有挂在 lvgl 目标上，它的 INTERFACE include 目录才在编译 lv_freetype.c 时生效，
# 同时顺带满足链接期的 FT_* 符号（main 链接 lvgl 时会传递过去）。
# ⚠ 2026-10 迁移时参考了 OtoClock（tempdir/oto-clock-master）：那边是
#    esppressif/esp_lvgl_adapter 的 CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE 在做这件事
#    （target_link_libraries(lvgl PUBLIC idf::espressif__freetype)）；本项目用的是自己的
#    lvgl_port，没有适配器，所以在这里自己做一次。
# ============================================================================
if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    if(TARGET idf::espressif__freetype)
        target_link_libraries(${lvgl_lib} PUBLIC idf::espressif__freetype)
    else()
        idf_component_get_property(freetype_lib espressif__freetype COMPONENT_LIB)
        target_link_libraries(${lvgl_lib} PUBLIC ${freetype_lib})
    endif()

    # ⚠ 还要补一个宏：LVGL 的 src/libs/freetype/lv_ftsystem.c 用了 FreeType 的
    # **内部**接口（FT_THROW / FT_ERR_PREFIX，见 freetype/internal/ftdebug.h），
    # 而 FreeType 的 fterrors.h 末尾是
    #       #ifndef FT2_BUILD_LIBRARY
    #       #undef FT_ERR_PREFIX
    #       #endif
    # 上游 freetype 目标只把 FT2_BUILD_LIBRARY 设成 PRIVATE
    # （freetype/CMakeLists.txt: target_compile_definitions(freetype PRIVATE FT2_BUILD_LIBRARY)），
    # 所以外部使用者看不到它。不给 lvgl 补上的报错是：
    #   error: 'FT_ERR_PREFIXCannot_Open_Resource' undeclared
    #          (did you mean 'FT_Err_Cannot_Open_Resource'?)   ← 宏被展开成了空
    # 这正是 2026-10 字体迁移时第一个构建失败点。
    target_compile_definitions(${lvgl_lib} PRIVATE FT2_BUILD_LIBRARY)

    # ⚠⚠ 还要把 FreeType 光栅化器的**渲染池**从 16KB 压到 4KB（第三个坑）：
    # smooth/ftgrays.c 把渲染池当栈上局部变量用
    #     TCell buffer[FT_MAX_GRAY_POOL];  FT_MAX_GRAY_POOL = FT_RENDER_POOL_SIZE/sizeof(TCell)
    # 默认 FT_RENDER_POOL_SIZE = 16384 → **单次 FT_Render_Glyph 就要 16KB 栈**。
    # 本板内部 RAM 紧张，不可能给每个"可能渲染字形"的任务都加 16KB 栈：
    # 实测主任务栈只有 8KB（CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192），开机在
    # app_main 里持锁渲染首帧时栈溢出 12.6KB 跌进 .bss，把 esp_pm 的
    # s_rtos_lock_handle 与 task_wdt 静态量一起写坏（表现为
    # esp_pm_lock_acquire 里 LoadProhibited + Core1 idle 报 task not found）。
    # 缩小池子安全：池子不够时 ftgrays.c 会自动分带渲染，结果位图不变、只是复杂
    # 字形稍慢（每个字形只光栅化一次，之后走 LVGL 字形缓存）。
    # FreeType 官方口子：ftheader.h 里 `#ifndef FT_CONFIG_OPTIONS_H` →
    # 用 FT_CONFIG_OPTIONS_H 指向我们那份"先 include 原配置再改池子"的头。
    # ⚠ 宏的替换文本带尖括号，所以整个 -D 要写成 "NAME=<绝对路径>"。
    if(TARGET freetype)
        target_compile_definitions(freetype PRIVATE
            "FT_CONFIG_OPTIONS_H=<${CMAKE_CURRENT_SOURCE_DIR}/core/fonts/ftoption_lvgl.h>")
    endif()

    # ⚠⚠ 还要让 **LVGL 的 lv_ftsystem.c 赢得链接**（这是字体迁移的第二个坑）：
    # FreeType 自带的 src/base/ftsystem.c 也定义了
    #     FT_Stream_Open / FT_New_Memory / FT_Done_Memory
    # （默认实现是 stdio 的 fopen/fseek/fread + newlib malloc），而且它就在
    # libfreetype.a 内部 —— 链接器扫到这个库时顺手就把它拉进来了。后果是：
    #   · 文件访问没走 lv_fs：lv_freetype 明明用 lv_fs 打开了 "F:/system.ttf"，
    #     FreeType 底下却拿路径去 stdio fopen() → FT_New_Face 报
    #     Cannot_Open_Resource(0x1)，界面回退位图字体；
    #   · 内存分配没走 lv_malloc：FreeType 的内部结构（cmap/loca 等，本字体
    #     cmap 22KB、loca 119KB）会落到 newlib malloc = 内部 RAM，
    #     而本板开机后内部 RAM 只剩几十 KB —— 等于 CONFIG_LV_FREETYPE_USE_LVGL_PORT
    #     完全失效。
    # 实测证据：map 里 `FT_Stream_Open ... libfreetype.a(ftsystem.c.obj)`，
    #           而 lv_ftsystem.c.obj 根本没进链接。
    # 修法沿用上面 lv_malloc_core 的同款思路：在链接开始前把这三个符号标成未定义。
    # 实测链接顺序是 libmain.a → liblvgl__lvgl.a → libfreetype.a，所以扫到 liblvgl.a
    # 时就会先拉入 lv_ftsystem.o；等扫到 libfreetype.a 时符号已满足，
    # ftsystem.o 不会被拉入（否则会 multiple definition）。
    target_link_options(${lvgl_lib} INTERFACE
        "-Wl,-u,FT_New_Memory" "-Wl,-u,FT_Done_Memory" "-Wl,-u,FT_Stream_Open")
endif()

# ============================================================================
# font 分区：把系统字体烧到分区偏移处（裸二进制，无文件系统）
# ----------------------------------------------------------------------------
# 分区定义见根目录 partitions.csv（font @ 0x800000，4MiB，subtype 0x41；
# 2026-10-25 由 8MiB 缩小 —— 换用 3.55MiB 的 Droid 后 4MiB 足够，省下的 4MiB 给 factory）。
# ⚠ **故意不挂到 `flash` 目标上**：字体 3,725,920 B，每轮 `idfc flash` 都重烧一遍
#    要多花 33 秒（实测；MiSans 时代是 75 秒），而字体文件几乎从不变。所以单独建一个
#    `font-flash` 目标：
#      · 平时：idfc flash          → 只烧 bootloader + 分区表 + app（约 20 秒）
#      · 换字体：idfc idfpy font-flash   → 只烧 font 分区
#    （`font-flash` 由 esptool_py_flash_target 注册成 CMake 目标，idf.py 直接透传。）
# ⚠ 换分区表后先看 font 分区的**起始偏移**变没变：
#    变了（例如从无到有）→ 必须至少跑一次 font-flash，否则分区是空的：界面仍能启动，
#      但会打印 "font 分区里不是 TTF/OTF（头 4 字节 = 0xFFFFFFFF）" 并回退内嵌位图字体；
#    没变、只是变小了（8MiB→4MiB，起点都是 0x800000）→ 字体数据原样有效，**不用重烧**。
# ⚠ 分区里没有文件系统，运行期由 core/fonts/font_ttf.c 的 lv_fs 驱动按绝对偏移读。
# ⚠ **端口坑**：`font-flash` 是 CMake 生成的自定义目标，`idf.py` 只是把它当 ninja
#    目标跑；而 IDF 的 components/esptool_py/get_port_args.cmake **在"烧录那一刻"
#    才读环境变量 `ESPPORT`**（刻意不在配置期展开，好让端口不用重配就能改）。
#    所以 `idfc idfpy -p COM12 font-flash` 里的 -p **传不进来**，esptool 会逐个
#    串口试（最终能烧成功，只是慢几秒、日志里多几条 could not open port）。
#    要明确端口：让**拉起 ninja 的那个进程**（= idfc 服务器，它会把自身环境整套
#    复制给 idf.py 子进程）带上 ESPPORT —— 先 `$env:ESPPORT="COM12"` 再
#    `idfc start-server`。完整实测与 idfc 侧的建议改法见 FontSystem.md §6。
# ============================================================================
if(CONFIG_UI_FONT_TTF)
    idf_component_get_property(_esptool_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(_esptool_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(font-flash "${_esptool_main_args}" "${_esptool_sub_args}")
    # 字体文件**写死**为 spiffs/system.ttf —— 换字体 = 用新字体文件**改名覆盖**它，
    # 然后 idfc build + idfc idfpy font-flash（固件代码与运行期路径都不用动：font 分区
    # 是裸二进制、只放一个文件，固件永远按 F:/system.ttf 打开）。
    # 当前内容：Droid Sans Fallback（3.55MiB，50951 字形；许可见 spiffs/ 里那份 txt）。
    esptool_py_flash_to_partition(font-flash "font"
                                  "${CMAKE_CURRENT_SOURCE_DIR}/../spiffs/system.ttf")
endif()

set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE)
