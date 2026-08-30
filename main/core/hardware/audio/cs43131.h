#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CS43131 DAC 驱动（I2C 控制 + 复位，依据官方数据手册 DS1155 编写）
 *
 * 硬件连接（FakePodNano）：
 *  - I2C 总线与触摸共用 I2C_NUM_0（SCL=GPIO2, SDA=GPIO3），实测设备地址 0x30
 *  - RESET# 经 3.3V→1.8V 电平转换接 GPIO41（低有效复位）
 *  - XTI/XTO 接 24.576MHz 无源晶振，直接作为内部 MCLK（Direct XTAL 模式）
 *  - ASP 为从机（Slave），BCK/LRCK/DIN 由 ESP32 I2S 主机提供
 *
 * 时钟策略（数据手册 7.1.7）：除 384kHz 主模式外，内部 MCLK 既可为 22.5792MHz
 * 也可为 24.576MHz，因此全程固定使用 24.576MHz 直通（MCLK_SRC_SEL=00, MCLK_INT=0），
 * 切换采样率只改 ASP 采样率寄存器，完全不触碰 PLL 与 MCLK 源，
 * 从根本上避免 MCLK 源切换导致的芯片失响应（卡死）。
 */

/* 电源控制寄存器（0x020000）各电源域 PDN 位（置 1 = 掉电） */
#define CS43131_PWR_XSP    (1U << 7)
#define CS43131_PWR_ASP    (1U << 6)
#define CS43131_PWR_DSDIF  (1U << 5)
#define CS43131_PWR_HP     (1U << 4)
#define CS43131_PWR_XTAL   (1U << 3)
#define CS43131_PWR_PLL    (1U << 2)
#define CS43131_PWR_CLKOUT (1U << 1)

/**
 * @brief 初始化 CS43131（复位 → 晶振 → 直通 MCLK → ASP → HP → PCM 路径）
 *
 * 前置条件：I2C_NUM_0 总线已由 touch_init() 安装。
 * 默认按 48kHz 采样率配置，音量 0dB。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 未找到设备；其它为 I2C 错误
 */
esp_err_t cs43131_init(void);

/**
 * @brief 设置 ASP 采样率（仅写 0x01000B，不切换 MCLK）
 *
 * @param sample_rate 44100 或 48000
 * @return ESP_OK 成功
 */
esp_err_t cs43131_set_sample_rate(uint32_t sample_rate);

/**
 * @brief 设置 PCM 数字音量（双通道）
 *
 * @param vol 0x00 = 0dB（最大），0xFF = 数字静音
 * @return ESP_OK 成功
 */
esp_err_t cs43131_set_volume(uint8_t vol);

/**
 * @brief 设置 PCM 静音
 *
 * @param mute true = 静音，false = 取消静音
 * @return ESP_OK 成功
 */
esp_err_t cs43131_set_mute(bool mute);

/**
 * @brief 获取当前配置的采样率
 * @return 44100 或 48000
 */
uint32_t cs43131_get_sample_rate(void);

/**
 * @brief 查询 CS43131 当前是否处于关机状态（耳机输出路径掉电，仅保留 I2C 控制）
 *
 * 读 0x020000 电源控制寄存器判断：读不到（I2C 异常）或 HP 与 ASP 均已掉电
 * （PDN_HP、PDN_ASP 均为 1）视为已关机。关机后 I2C 接口仍存活，可随时安全调用。
 *
 * @return true = 已关机；false = 处于开机状态
 */
bool cs43131_is_powered_down(void);

/**
 * @brief 关机（关闭耳机输出路径，仅保留 I2C 控制接口，供深度休眠等低功耗场景）
 *
 * 按数据手册 §5.6.1（Example 5-3）顺序：解除 PDN_DONE 中断掩码 → 关 HP
 * （PDN_HP=1）→ 轮询等掉电完成中断（PDN_DONE_INT，100ms 超时）→ 关 ASP
 * （PDN_ASP=1）。晶振（PDN_XTAL）按手册保持常开（Direct XTAL 直通 MCLK
 * 依赖它且功耗极低），PLL/XSP/DSDIF/CLKOUT 本就掉电，不参与操作。
 * 寄存器内容不受影响，开机后无需重新探测。
 * 注意：调用前应确保无播放进行中（UI 层负责先暂停）。
 *
 * @return ESP_OK 成功
 */
esp_err_t cs43131_power_down(void);

/**
 * @brief 开机（恢复耳机输出路径，供深度休眠等低功耗场景恢复）
 *
 * 按数据手册 §5.7.1（Example 5-5）顺序：去 pop 设置（0x010010=0x99、
 * 0x080032=0x20）→ 开 ASP（清 PDN_ASP）→ 开 HP（清 PDN_HP）→ 等 12ms →
 * 恢复默认设置。其余寄存器（采样率/音量/路径等）在关机时保持不变，无需重写。
 * 内部含 12ms 等待，属阻塞调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t cs43131_power_up(void);

#ifdef __cplusplus
}
#endif
