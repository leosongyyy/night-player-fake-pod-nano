# 实验性组件：在 2nd stage bootloader 里用 bit-bang 读 AXP2101（I²C0，0x34）
#
# ⚠ 仅用于体积/可行性实验，默认不参与正常构建：
#   需要时把本目录复制到 <project>/bootloader_components/boot_i2c/ 再 build。
#
# 依赖：esp_rom（ROM 的 GPIO / delay 函数）+ esp_hal_gpio（gpio_ll.h 头文件）。
# 不依赖 esp_driver_i2c / FreeRTOS ——  bootloader 是非 OS 构建，i2c_master 驱动
# 需要 FreeRTOS 信号量，无法直接链进来。
idf_component_register(SRCS "boot_i2c.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_rom esp_hal_gpio)
