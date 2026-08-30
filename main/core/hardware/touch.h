#pragma once

#include "driver/gpio.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸控制器硬件抽象（I2C 接口）
 *
 * 根据 display.h 中 TOUCH_IC_CONFIG 自动选择：
 *   CST816 / CST820 / CHSC6417
 */

#define EXAMPLE_USE_TOUCH               1

// ESP32S3_AMOLED_触摸
#define EXAMPLE_PIN_NUM_TOUCH_SCL         (GPIO_NUM_2)
#define EXAMPLE_PIN_NUM_TOUCH_SDA         (GPIO_NUM_3)
#define EXAMPLE_PIN_NUM_TOUCH_RST         (GPIO_NUM_5)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (GPIO_NUM_4)

/**
 * @brief 初始化 I2C 总线并创建触摸控制器
 *
 * @return esp_lcd_touch_handle_t 触摸句柄；失败时为 NULL
 */
esp_lcd_touch_handle_t touch_init(void);

/**
 * @brief 获取触摸句柄（供 UI 轮询使用）
 */
esp_lcd_touch_handle_t touch_get_handle(void);

/**
 * @brief 关闭触摸控制器并释放 I2C 总线驱动（供深度休眠前调用）
 *
 * 触摸 RST 拉低进入复位低功耗，随后释放 I2C_NUM_0 驱动（引脚回到高阻，
 * 外部上拉无电流路径）。须在 CS43131 关机后调用（总线已不再使用）；
 * 唤醒后 touch_init() 会重新安装总线与触摸控制器。
 */
void touch_power_off(void);

#ifdef __cplusplus
}
#endif
