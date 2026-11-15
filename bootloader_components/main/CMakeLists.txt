# 覆盖 2nd stage bootloader 的 main 组件：
# 在分区选择前用 bit-bang 读 PCF85063A 的 RAM_byte(0x03)，按协议选 factory / ota_0。
#
# ⚠ 实验性：正常构建会带上它。移走本目录（或加 BOOTLOADER_IGNORE_EXTRA_COMPONENT=main）
#   即恢复 IDF 默认 bootloader。
idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support esp_hal_gpio esp_rom)

# 使用 IDF 默认的 bootloader linker script（必须保留）
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
