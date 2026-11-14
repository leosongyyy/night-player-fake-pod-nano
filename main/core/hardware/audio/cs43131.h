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

/* 电源控制寄存器（0x020000）各电源域 PDN 位（置 1 = 掉电）。
   仅 XTAL/ASP/HP 参与上电配置（cs43131_apply_full_config），其余域
   （XSP/DSDIF/PLL/CLKOUT）默认掉电且本项目不使用。 */
#define CS43131_PWR_ASP    (1U << 6)
#define CS43131_PWR_HP     (1U << 4)
#define CS43131_PWR_XTAL   (1U << 3)

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
 * @brief 优雅掉电（关机路径专用，避免耳机爆音）
 *
 * 按数据手册 **Example 5-6「PCM Pop-Free Power-Down Sequence（External
 * VCPFILT 模式）」** 执行，与上电用的 Example 5-5 / 本项目
 * cs43131_apply_full_config() 成对：
 *
 *   1. pop-free 掉电设置（0x010010=0x99、0x080046=0x21）；
 *   2. PCM 双声道静音（0x090003 |= 0x03，该寄存器配了 PCM_RAMP_DOWN
 *      软斜坡，音量是渐降而不是阶跃）；
 *   3. **等 150ms** 让软斜坡走完（手册要求；这期间 ASP/MCLK 时钟必须还在，
 *      所以调用方不要先停 I2S，见 audio_shutdown()）；
 *   4. 关耳机放大器（PDN_HP）并等其掉电；
 *   5. 关 ASP 数据路径（PDN_ASP）；
 *   6. 恢复手册要求的默认值（0x080046=0x20、0x010010=0x00，取消静音）；
 *   7. 关晶振（PDN_XTAL），最后把 RESET# 拉低 —— 芯片回到出厂
 *      "全部电源域掉电"态（0x020000=0xFE），PMU 随后切断整机电源时
 *      已经没有任何带偏置的模拟电路，是最安静的状态。
 *
 * ⚠ 手册第 4/6 步是"等 PDN_DONE 中断"，但寄存器级掉电（晶振仍开）时
 *   PDN_DONE 永不触发（见 cs43131_reset_for_sleep 注释），故改用固定延时兜底。
 * ⚠ 调用后 s_ready=false，音量/静音等接口一律返回 ESP_ERR_NOT_FOUND；
 *   若随后并没有真正断电（关机失败），必须重新 cs43131_init() 才能再用
 *   （audio 层封装成 audio_recover_after_failed_shutdown()）。
 * ⚠ 幂等：未探测到设备（audio 未初始化 / 初始化失败）时直接返回
 *   ESP_ERR_NOT_FOUND，不产生任何 I²C 流量。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 设备不可用（无需掉电）
 */
esp_err_t cs43131_power_off(void);

/**
 * @brief 深度休眠前复位 CS43131（RESET# 拉低硬件复位，供 deepsleep 使用）
 *
 * 直接拉低 GPIO41：芯片回到出厂默认态（0x020000=0xFE，全部电源域含晶振
 * 一起掉电），比寄存器级关机更彻底且状态完全确定——寄存器级关机时
 * PDN_DONE 掉电完成中断永不触发（晶振保持常开，芯片达不到"完全掉电"）。
 * 调用后 GPIO41 保持低电平直至进入 deepsleep（芯片持续复位，功耗最低）；
 * 唤醒后由 cs43131_init() 重新走完整复位+配置流程恢复，无需在此处理。
 *
 * @return ESP_OK 成功
 */
esp_err_t cs43131_reset_for_sleep(void);

/**
 * @brief 把本设备句柄从 I2C 总线上摘除（供 deepsleep 前释放总线用）
 *
 * IDF 6.1 的新 I2C 驱动是「总线句柄 + 每设备句柄」模型：CS43131 的设备句柄由
 * cs43131_init() 挂到触摸建立的总线上（touch_get_i2c_bus()）。既然触摸的
 * touch_power_off() 会调用 i2c_del_master_bus() 销毁整条总线，本函数就必须
 * **在它之前**调用，否则设备句柄悬垂，唤醒重启后的 cs43131_init() 会误判
 * “已挂载”而复用失效句柄。
 *
 * 幂等：未挂载时为空操作。
 */
void cs43131_bus_detach(void);

#ifdef __cplusplus
}
#endif
