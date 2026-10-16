# esp_driver_gpio 为显式依赖（2026-11 迁移 IDF 6.1 时补，由本项目
# patches/esp_lcd_touch_deps/ 在 CMake 配置阶段自动回写，见
# main/CMakeLists.txt 的"构建期补丁"一节）：
# 本组件直接调用 gpio_install_isr_service / gpio_intr_enable /
# gpio_isr_handler_add / gpio_isr_handler_remove / gpio_intr_disable，
# 并在公开头文件 esp_lcd_touch.h 里 include "driver/gpio.h"。
# IDF 6.0 起旧 `driver` 组件不再公共依赖 esp_driver_gpio 等 18 个子组件，
# 此前只是靠 esp_lcd 转发才拿到头文件；一旦上游 esp_lcd 收窄依赖即编译失败。
idf_component_register(SRCS "esp_lcd_touch.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES "esp_driver_gpio"
                       REQUIRES "driver" "esp_lcd")
