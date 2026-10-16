# ⚠ 显式声明 esp_driver_gpio（2026-11 迁移 IDF 6.1 时补）：本驱动直接调用
# gpio_config / gpio_reset_pin / gpio_set_level（见 .c 里 RST 线的处理）。
# IDF 6.0 起旧 `driver` 组件**不再公共依赖** 18 个 esp_driver_* 子组件
# （esp_driver_gpio 就在其中），所以 `PRIV_REQUIRES driver` 已经拿不到 gpio 头；
# 目前能编过纯粹是因为 `REQUIRES esp_lcd` 转发了 esp_driver_gpio/i2c/spi——
# 一旦上游 esp_lcd 收窄依赖，这里就会连带编译失败。故补成显式依赖。
idf_component_register(SRCS "esp_lcd_qspi_amoled.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES "driver" "esp_driver_gpio"
                       REQUIRES "esp_lcd")

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})



