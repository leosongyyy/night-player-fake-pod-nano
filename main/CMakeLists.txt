idf_component_register(
    SRCS "main.c"
         "core/hardware/display.c"
         "core/hardware/touch.c"
         "core/hardware/battery.c"
         "core/hardware/button_service.c"
         "core/hardware/sd_card.c"
         "core/hardware/audio/audio.c"
         "core/hardware/audio/cs43131.c"
         "core/hardware/audio/mp3_player.c"
         "core/ui/lvgl_port.c"
         "core/ui/touch_test.c"
         "core/utils/performance_monitor.c"
         "ui/app.c"
         "ui/main_ui.c"
         "ui/notification_panel.c"
    INCLUDE_DIRS "." "core" "ui")

set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE)
