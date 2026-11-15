/*
 * 实验性：2nd stage bootloader 里用 bit-bang 读 AXP2101（I²C0，地址 0x34）
 *
 * 目的：量出"在 bootloader 里加 I²C 读取"到底要多吃多少字节，能不能装进
 *       bootloader 分区（0x2000 ~ 0x8000，上限 24,576 B）。
 *
 * 为什么不用 esp_driver_i2c：bootloader 是 NON_OS 构建（没有 FreeRTOS 信号量，
 * 也没有 esp_timer / heap 的那套初始化），i2c_master 驱动链不进来。这里直接
 * 用 gpio_ll 模拟开漏 I²C，只依赖 esp_rom + esp_hal_gpio 的头文件。
 *
 * 硬件前提：I²C0 的 SCL=GPIO47 / SDA=GPIO48，板上有外部上拉（PMU 文档要求
 * 2.2 kΩ，实际板载已有），所以这里不打开内部上/下拉。
 *
 * ⚠ 本文件只做体积实验，没有接任何"用这个字节决定启动分区"的逻辑；
 *   读到的 4 字节放在 volatile 全局里，方便在 map 里看到它确实没被优化掉。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

#define BOOT_I2C_SCL_GPIO   47
#define BOOT_I2C_SDA_GPIO   48
#define BOOT_I2C_ADDR       0x34   /* AXP2101 7-bit 地址 */
#define BOOT_I2C_HALF_US    5      /* 半周期 5 µs ≈ 100 kHz */

/* AXP2101 的 4 字节 DATA BUFFER：0x04~0x07（RW，POR 默认 0x00） */
#define BOOT_PMU_DATA_REG   0x04
#define BOOT_PMU_DATA_LEN   4

/* 读到的结果。volatile 保证编译器不会把"只写不读"的赋值删掉；
 * 真要用的话，App 侧需要一个约定的 RTC 保留内存/共享地址来取。 */
volatile uint8_t  g_boot_pmu_data[BOOT_PMU_DATA_LEN];
volatile uint32_t g_boot_pmu_status;

static inline void i2c_half_delay(void)
{
    esp_rom_delay_us(BOOT_I2C_HALF_US);
}

/* ------------------------------ GPIO 开漏 ------------------------------ */

static void i2c_pin_init(uint32_t pin)
{
    /* IO_MUX 选 GPIO 功能；输入常开（读 SDA / 读 SCL 做时钟拉伸检测）；
       输出路由到 GPIO_OUT 寄存器（SIG_GPIO_OUT_IDX），输出使能由
       GPIO_ENABLE_REG 控制（oen_sel=1）；初始释放（输出关、电平 1）。 */
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_set_output_signal_matrix_source(&GPIO, pin, SIG_GPIO_OUT_IDX, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, pin, false, false);
    gpio_ll_set_level(&GPIO, pin, 1);
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_pullup_dis(&GPIO, pin);
    gpio_ll_pulldown_dis(&GPIO, pin);
}

static inline void scl_low(void)
{
    gpio_ll_set_level(&GPIO, BOOT_I2C_SCL_GPIO, 0);
    gpio_ll_output_enable(&GPIO, BOOT_I2C_SCL_GPIO);
}

static inline void scl_release(void)
{
    gpio_ll_output_disable(&GPIO, BOOT_I2C_SCL_GPIO);
}

static inline void sda_low(void)
{
    gpio_ll_set_level(&GPIO, BOOT_I2C_SDA_GPIO, 0);
    gpio_ll_output_enable(&GPIO, BOOT_I2C_SDA_GPIO);
}

static inline void sda_release(void)
{
    gpio_ll_output_disable(&GPIO, BOOT_I2C_SDA_GPIO);
}

static inline int sda_level(void)
{
    return gpio_ll_get_level(&GPIO, BOOT_I2C_SDA_GPIO);
}

/* 等 SCL 真的变高（防从机时钟拉伸；AXP2101 不拉伸，但卡死时要有上限） */
static bool scl_wait_high(void)
{
    for (int i = 0; i < 100; i++) {
        if (gpio_ll_get_level(&GPIO, BOOT_I2C_SCL_GPIO)) {
            return true;
        }
        i2c_half_delay();
    }
    return false;
}

/* ------------------------------ I²C 基本时序 --------------------------- */

static void i2c_stop(void)
{
    scl_low();
    i2c_half_delay();
    sda_low();
    i2c_half_delay();
    scl_release();
    i2c_half_delay();
    sda_release();
    i2c_half_delay();
}

/* 9 个 SCL 把从机从"卡住 SDA 低"的状态里踢出来，再补一个 STOP */
static void i2c_bus_recover(void)
{
    sda_release();
    scl_release();
    for (int i = 0; i < 9 && sda_level() == 0; i++) {
        scl_low();
        i2c_half_delay();
        scl_release();
        i2c_half_delay();
    }
    i2c_stop();
}

static bool i2c_start(void)
{
    sda_release();
    scl_release();
    i2c_half_delay();
    if (!scl_wait_high() || sda_level() == 0) {
        return false;   /* 总线不是空闲态 */
    }
    sda_low();
    i2c_half_delay();
    scl_low();
    i2c_half_delay();
    return true;
}

/* 重复起始：SCL 低时先释放 SDA，再释放 SCL，然后重新拉低 SDA */
static bool i2c_restart(void)
{
    sda_release();
    i2c_half_delay();
    scl_release();
    if (!scl_wait_high()) {
        return false;
    }
    i2c_half_delay();
    sda_low();
    i2c_half_delay();
    scl_low();
    i2c_half_delay();
    return true;
}

static bool i2c_write_bit(bool bit)
{
    if (bit) {
        sda_release();
    } else {
        sda_low();
    }
    i2c_half_delay();
    scl_release();
    if (!scl_wait_high()) {
        return false;
    }
    i2c_half_delay();
    scl_low();
    i2c_half_delay();
    return true;
}

/* 返回 0/1；-1 = SCL 等不到高 */
static int i2c_read_bit(void)
{
    sda_release();
    i2c_half_delay();
    scl_release();
    if (!scl_wait_high()) {
        return -1;
    }
    i2c_half_delay();
    int bit = sda_level();
    scl_low();
    i2c_half_delay();
    return bit;
}

/* 写 1 字节并读 ACK：true = 从机应答 */
static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (!i2c_write_bit(((byte >> i) & 1u) != 0)) {
            return false;
        }
    }
    sda_release();
    i2c_half_delay();
    scl_release();
    if (!scl_wait_high()) {
        return false;
    }
    i2c_half_delay();
    bool ack = (sda_level() == 0);
    scl_low();
    i2c_half_delay();
    return ack;
}

/* 读 1 字节；ack=true 时给从机 ACK，最后一个字节给 NACK。返回 -1 = 失败 */
static int i2c_read_byte(bool ack)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        int bit = i2c_read_bit();
        if (bit < 0) {
            return -1;
        }
        byte = (uint8_t)((byte << 1) | bit);
    }
    if (ack) {
        sda_low();
    } else {
        sda_release();
    }
    i2c_half_delay();
    scl_release();
    if (!scl_wait_high()) {
        return -1;
    }
    i2c_half_delay();
    scl_low();
    i2c_half_delay();
    sda_release();
    return byte;
}

/* 从 AXP2101 连续读 len 字节（寄存器地址自增） */
static bool pmu_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (!i2c_start()) {
        return false;
    }
    if (!i2c_write_byte((uint8_t)(BOOT_I2C_ADDR << 1))) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return false;
    }
    if (!i2c_restart()) {
        return false;
    }
    if (!i2c_write_byte((uint8_t)((BOOT_I2C_ADDR << 1) | 1u))) {
        i2c_stop();
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        int v = i2c_read_byte((uint8_t)(i + 1) < len);
        if (v < 0) {
            i2c_stop();
            return false;
        }
        buf[i] = (uint8_t)v;
    }
    i2c_stop();
    return true;
}

/* ------------------------------ 钩子 ---------------------------------- */

/* 这个空函数是让链接器把本组件从静态库里拉进来的"锚点"（IDF 的约定） */
void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    uint8_t buf[BOOT_PMU_DATA_LEN] = { 0 };

    i2c_pin_init(BOOT_I2C_SCL_GPIO);
    i2c_pin_init(BOOT_I2C_SDA_GPIO);
    i2c_bus_recover();

    if (pmu_read(BOOT_PMU_DATA_REG, buf, BOOT_PMU_DATA_LEN)) {
        for (int i = 0; i < BOOT_PMU_DATA_LEN; i++) {
            g_boot_pmu_data[i] = buf[i];
        }
        g_boot_pmu_status = 0xB0070000u | buf[0];
    } else {
        g_boot_pmu_status = 0xDEAD0000u;
    }
}
