/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_caps.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // If this boot is a wake up from the deep sleep then go to the short way,
    // try to load the application which worked before deep sleep.
    // It skips a lot of checks due to it was done before (while first boot).
    bootloader_utility_load_boot_image_from_deep_sleep();
    // If it is not successful try to load an application as usual.
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 2.1 Load the TEE image
#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

/* ==================================================================== */
/* 用 PCF85063A 的 RAM_byte(0x03) 决定本次启动的分区（唯一来源）          */
/* ==================================================================== */
/*
 * 协议（bootloader 与 App 必须一致；App 侧常量在
 * main/core/system/fw_switch.c 里重复定义，改一处必须改两处）：
 *
 *   bit1:0  分区选择  0b00 = factory（表世界，默认）
 *                     0b01 = ota_0（里世界）
 *                     （0b10/0b11 保留未用，读到按无效处理 → factory）
 *   bit2    充电界面  1 = 本次启动进充电界面（表世界"插电关机"重启用），
 *                     **由 App 消费后立刻清 0，bootloader 不碰这一位**
 *   bit7:3  保留（当前恒 0，留给以后跨复位/跨固件的短消息）
 *
 * 为什么分区只用 2 bit：原来的 0xA5/0x5A 两个魔法值把整个字节占满了，
 * 现在要再挤一个"启动进充电界面"的一次性标志，所以改成位域。
 * ⚠ 旧固件写下的 0xA5 在新协议里 = 分区 0b01（里世界）+ bit2=1（充电界面）。
 *   实际不会同时生效：bootloader 先按分区启动 ota_0，位 2 留给里世界自己清掉。
 *
 * 读成功后只清自己消费的**分区位**（bit1:0），**保留 bit2 与保留位**，
 * 保证 App 在本次重启前写下的充电界面请求不被 bootloader 抹掉。
 * 读失败/超时/NACK -> 直接启动 factory（otadata 已不参与启动，见表下方）。
 *
 * ⚠ 这段代码在主核 IRAM 里跑（bootloader 的 main 组件），I²C 用 bit-bang，
 *   不依赖 FreeRTOS / i2c_master。RTC 由 PMU 的 RTCLDO 供电，电池常驻时
 *   这个字节跨 SoC 复位保持；SR(0x00=0x58) 或 RTC 掉电才会清。
 */
#define RTC_BOOT_SCL_GPIO       47
#define RTC_BOOT_SDA_GPIO       48
#define RTC_BOOT_I2C_ADDR       0x51   /* PCF85063A 7-bit 地址 */
#define RTC_BOOT_RAM_REG        0x03   /* RAM_byte */
#define RTC_BOOT_PART_MASK      0x03   /* bit1:0 分区选择 */
#define RTC_BOOT_PART_INNER     0x01   /* 0b01 = ota_0（里世界） */
#define RTC_BOOT_FLAG_CHARGE_UI 0x04   /* bit2 = 本次启动进充电界面（bootloader 不消费） */
#define RTC_BOOT_HALF_US        5      /* 半周期 5 µs ≈ 100 kHz */
#define RTC_BOOT_NO_REQUEST     INT32_MAX

static inline void rtc_half_delay(void)
{
    esp_rom_delay_us(RTC_BOOT_HALF_US);
}

static void rtc_i2c_pin_init(uint32_t pin)
{
    /* 开漏模拟：IO_MUX 选 GPIO；输入常开；输出路由到 GPIO_OUT 寄存器
       （SIG_GPIO_OUT_IDX），输出使能由 GPIO_ENABLE_REG 控制（oen_sel=1）；
       初始释放（输出关、电平 1），外部上拉拉高。 */
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_set_output_signal_matrix_source(&GPIO, pin, SIG_GPIO_OUT_IDX, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_set_level(&GPIO, pin, 1);
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_pullup_dis(&GPIO, pin);
    gpio_ll_pulldown_dis(&GPIO, pin);
}

static inline void rtc_scl_low(void)
{
    gpio_ll_set_level(&GPIO, RTC_BOOT_SCL_GPIO, 0);
    gpio_ll_output_enable(&GPIO, RTC_BOOT_SCL_GPIO);
}

static inline void rtc_scl_release(void)
{
    gpio_ll_output_disable(&GPIO, RTC_BOOT_SCL_GPIO);
}

static inline void rtc_sda_low(void)
{
    gpio_ll_set_level(&GPIO, RTC_BOOT_SDA_GPIO, 0);
    gpio_ll_output_enable(&GPIO, RTC_BOOT_SDA_GPIO);
}

static inline void rtc_sda_release(void)
{
    gpio_ll_output_disable(&GPIO, RTC_BOOT_SDA_GPIO);
}

static inline int rtc_sda_level(void)
{
    return gpio_ll_get_level(&GPIO, RTC_BOOT_SDA_GPIO);
}

/* 等 SCL 变高；上限 100×5 µs = 500 µs，防止 RTC 异常时卡死启动 */
static bool rtc_scl_wait_high(void)
{
    for (int i = 0; i < 100; i++) {
        if (gpio_ll_get_level(&GPIO, RTC_BOOT_SCL_GPIO)) {
            return true;
        }
        rtc_half_delay();
    }
    return false;
}

static void rtc_i2c_stop(void)
{
    rtc_scl_low();
    rtc_half_delay();
    rtc_sda_low();
    rtc_half_delay();
    rtc_scl_release();
    rtc_half_delay();
    rtc_sda_release();
    rtc_half_delay();
}

/* 9 个 SCL 把从机从"卡住 SDA 低"里踢出来，再补 STOP */
static void rtc_i2c_bus_recover(void)
{
    rtc_sda_release();
    rtc_scl_release();
    for (int i = 0; i < 9 && rtc_sda_level() == 0; i++) {
        rtc_scl_low();
        rtc_half_delay();
        rtc_scl_release();
        rtc_half_delay();
    }
    rtc_i2c_stop();
}

static bool rtc_i2c_start(void)
{
    rtc_sda_release();
    rtc_scl_release();
    rtc_half_delay();
    if (!rtc_scl_wait_high() || rtc_sda_level() == 0) {
        return false;
    }
    rtc_sda_low();
    rtc_half_delay();
    rtc_scl_low();
    rtc_half_delay();
    return true;
}

static bool rtc_i2c_restart(void)
{
    rtc_sda_release();
    rtc_half_delay();
    rtc_scl_release();
    if (!rtc_scl_wait_high()) {
        return false;
    }
    rtc_half_delay();
    rtc_sda_low();
    rtc_half_delay();
    rtc_scl_low();
    rtc_half_delay();
    return true;
}

static bool rtc_i2c_write_bit(bool bit)
{
    if (bit) {
        rtc_sda_release();
    } else {
        rtc_sda_low();
    }
    rtc_half_delay();
    rtc_scl_release();
    if (!rtc_scl_wait_high()) {
        return false;
    }
    rtc_half_delay();
    rtc_scl_low();
    rtc_half_delay();
    return true;
}

static int rtc_i2c_read_bit(void)
{
    rtc_sda_release();
    rtc_half_delay();
    rtc_scl_release();
    if (!rtc_scl_wait_high()) {
        return -1;
    }
    rtc_half_delay();
    int bit = rtc_sda_level();
    rtc_scl_low();
    rtc_half_delay();
    return bit;
}

static bool rtc_i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (!rtc_i2c_write_bit(((byte >> i) & 1u) != 0)) {
            return false;
        }
    }
    rtc_sda_release();
    rtc_half_delay();
    rtc_scl_release();
    if (!rtc_scl_wait_high()) {
        return false;
    }
    rtc_half_delay();
    bool ack = (rtc_sda_level() == 0);
    rtc_scl_low();
    rtc_half_delay();
    return ack;
}

static int rtc_i2c_read_byte(bool ack)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        int bit = rtc_i2c_read_bit();
        if (bit < 0) {
            return -1;
        }
        byte = (uint8_t)((byte << 1) | bit);
    }
    if (ack) {
        rtc_sda_low();
    } else {
        rtc_sda_release();
    }
    rtc_half_delay();
    rtc_scl_release();
    if (!rtc_scl_wait_high()) {
        return -1;
    }
    rtc_half_delay();
    rtc_scl_low();
    rtc_half_delay();
    rtc_sda_release();
    return byte;
}

/* 读 PCF85063A RAM_byte(0x03)；任何一步失败返回 false */
static bool rtc_read_ram(uint8_t *value)
{
    if (!rtc_i2c_start()) {
        return false;
    }
    if (!rtc_i2c_write_byte((uint8_t)(RTC_BOOT_I2C_ADDR << 1))) {
        rtc_i2c_stop();
        return false;
    }
    if (!rtc_i2c_write_byte(RTC_BOOT_RAM_REG)) {
        rtc_i2c_stop();
        return false;
    }
    if (!rtc_i2c_restart()) {
        return false;
    }
    if (!rtc_i2c_write_byte((uint8_t)((RTC_BOOT_I2C_ADDR << 1) | 1u))) {
        rtc_i2c_stop();
        return false;
    }
    int v = rtc_i2c_read_byte(false);   /* 最后一个字节 NACK */
    if (v < 0) {
        rtc_i2c_stop();
        return false;
    }
    rtc_i2c_stop();
    *value = (uint8_t)v;
    return true;
}

/* 写 RAM_byte(0x03)，用于"消费"请求；失败只记日志，不影响本次启动 */
static bool rtc_write_ram(uint8_t value)
{
    if (!rtc_i2c_start()) {
        return false;
    }
    if (!rtc_i2c_write_byte((uint8_t)(RTC_BOOT_I2C_ADDR << 1))) {
        rtc_i2c_stop();
        return false;
    }
    if (!rtc_i2c_write_byte(RTC_BOOT_RAM_REG)) {
        rtc_i2c_stop();
        return false;
    }
    if (!rtc_i2c_write_byte(value)) {
        rtc_i2c_stop();
        return false;
    }
    rtc_i2c_stop();
    return true;
}

/*
 * 返回：
 *   0                  -> ota_0（里世界）
 *   FACTORY_INDEX(-1)  -> factory（表世界）
 *   RTC_BOOT_NO_REQUEST-> RTC 读不到（由调用方决定降级策略）
 *
 * 2026-11-16 起**全面迁移到 RTC 字节**：otadata 不再参与启动决策。
 *   bit1:0 = 0b01 → ota_0；其它（0b00 / 保留值 / 无效）→ factory。
 * 这样"回表世界"不需要擦 otadata（旧实现要擦 8KB，非 RTC 主路径时还会每次擦）。
 * 代价：RTC 读失败时不再回退 otadata，而是直接走 factory（表世界是主固件，
 * 这个降级方向更安全；里世界只在显式请求时才进）。
 */
static int rtc_boot_select(const bootloader_state_t *bs)
{
    uint8_t req = 0;

    rtc_i2c_pin_init(RTC_BOOT_SCL_GPIO);
    rtc_i2c_pin_init(RTC_BOOT_SDA_GPIO);
    rtc_i2c_bus_recover();

    if (!rtc_read_ram(&req)) {
        /* 一次重试：瞬态干扰/总线没释放干净时很常见 */
        rtc_i2c_bus_recover();
        if (!rtc_read_ram(&req)) {
            esp_rom_printf("[boot] rtc03 read failed -> factory (no otadata any more)\n");
            return RTC_BOOT_NO_REQUEST;
        }
    }

    const uint8_t part = (uint8_t)(req & RTC_BOOT_PART_MASK);
    const bool want_inner = (part == RTC_BOOT_PART_INNER);
    int index;

    if (want_inner && bs->ota[0].offset != 0) {
        index = 0;
    } else {
        /* 0b00 / 保留值 / 请求了但 ota_0 不存在 → factory */
        index = FACTORY_INDEX;
        if (want_inner) {
            esp_rom_printf("[boot] rtc03=0x%02x wants ota_0 but slot is missing -> factory\n",
                           req);
        }
    }

    /* 消费分区位（只清 bit1:0）：bit2（充电界面请求）与保留位必须原样留给 App。
       这样里世界崩溃/复位后不会因为分区位还在又进里世界。 */
    if (part != 0) {
        if (!rtc_write_ram((uint8_t)(req & (uint8_t)~RTC_BOOT_PART_MASK))) {
            esp_rom_printf("[boot] rtc03 clear failed (request consumed anyway)\n");
        }
    }
    esp_rom_printf("[boot] rtc03=0x%02x -> %s (flags kept 0x%02x)\n", req,
                   index == 0 ? "ota_0 (inner)" : "factory (player)",
                   (unsigned)(req & (uint8_t)~RTC_BOOT_PART_MASK));

    return index;
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return selected_boot_partition(bs);
}

/*
 * Selects a boot partition.
 * The conditions for switching to another firmware are checked.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        // Factory firmware.
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
        bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
        reset_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, reset_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a condition of the factory reset");
            bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
            ota_data_erase = true;
#endif
            const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
            ESP_LOGI(TAG, "Data partitions to erase: %s", list_erase);
            if (bootloader_common_erase_part_type_data(list_erase, ota_data_erase) == false) {
                ESP_LOGE(TAG, "Not all partitions were erased");
            }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
            bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
            return bootloader_utility_get_selected_boot_partition(bs);
        }
#endif // CONFIG_BOOTLOADER_FACTORY_RESET
        // TEST firmware.
#ifdef CONFIG_BOOTLOADER_APP_TEST
        bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
        app_test_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_APP_TEST, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, app_test_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a boot condition of the test firmware");
            if (bs->test.offset != 0) {
                boot_index = TEST_APP_INDEX;
                return boot_index;
            } else {
                ESP_LOGE(TAG, "Test firmware is not found in partition table");
                return INVALID_INDEX;
            }
        }
#endif // CONFIG_BOOTLOADER_APP_TEST
        // Customer implementation: PCF85063A RAM_byte 启动请求。
        // 放在 factory reset / test 之后：那两个调试入口优先级更高。
        // ⚠ 2026-11-16 起**这是唯一的分区来源**：otadata 不再参与决策，
        //   "回表世界"因此不需要任何 flash 擦写。
        int rtc_index = rtc_boot_select(bs);
        if (rtc_index != RTC_BOOT_NO_REQUEST) {
            return rtc_index;
        }

        /* RTC 读不到（芯片不在/总线故障）：直接走 factory。
           表世界是主固件，这个降级方向比"按陈旧 otadata 启动"安全。 */
        if (bs->factory.offset != 0) {
            esp_rom_printf("[boot] rtc unavailable -> factory (player)\n");
            return FACTORY_INDEX;
        }
    }

    /* 深睡眠唤醒 / factory 分区不存在：交回 IDF 默认选择（最后的兜底，
       只在上面两条都没给出结论时才会用到） */
    return bootloader_utility_get_selected_boot_partition(bs);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
