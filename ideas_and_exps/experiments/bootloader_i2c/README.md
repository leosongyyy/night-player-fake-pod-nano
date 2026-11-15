# bootloader_i2c（实验代码，默认不参与构建）

在 2nd stage bootloader 里用 bit-bang 读 AXP2101（I²C0，0x34）的 4 字节 DATA BUFFER
（0x04~0x07）。**只构建、未烧录**。

- 体积结果与完整分析：`../../BOOTLOADER_I2C_SIZE.md`
- 启用方式：把本目录整体复制到项目根的 `bootloader_components/boot_i2c/`，
  删掉 `build/<cell>/bootloader-prefix/src/bootloader-stamp/bootloader-configure`
  后 `idfc idfpy -B build\player.debug bootloader`。
- 依赖：`esp_rom` + `esp_hal_gpio`（都在 bootloader 构建里现成存在）；
  **不要**试图在这里 `REQUIRES esp_driver_i2c`（NON_OS 构建，FreeRTOS/heap 链不进来）。
- 引脚：SCL=GPIO47，SDA=GPIO48，开漏模拟，依赖板载外部上拉。
- 读到的值目前只放在 `volatile` 全局里，仅用于体积测量；没有接分区选择或 App 交接。
