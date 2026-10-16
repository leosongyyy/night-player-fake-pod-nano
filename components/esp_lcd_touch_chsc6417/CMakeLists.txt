# esp_driver_gpio 为显式依赖（2026-11 迁移 IDF 6.1 时补）：本驱动直接调用
# gpio_config / gpio_reset_pin / gpio_set_level / gpio_isr_handler_remove 配置
# RST 与 INT 线。IDF 6.0 起旧 `driver` 组件不再公共依赖 esp_driver_gpio 等
# 18 个子组件，此前只是靠 esp_lcd_touch → esp_lcd 间接转发才拿到头文件。
idf_component_register(SRCS "esp_lcd_touch_chsc6417.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES "esp_driver_gpio"
                       REQUIRES "esp_lcd_touch")
