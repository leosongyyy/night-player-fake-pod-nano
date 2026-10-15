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

set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE)
