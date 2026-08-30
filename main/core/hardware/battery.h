#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电池电压采集模块
 *
 * VBAT 通过两个 10K 电阻串联到 GND，GPIO1 接在两电阻中间，
 * ADC 测得的电压 = VBAT / 2，因此 VBAT = ADC电压 * 2
 */

// 电量百分比估算区间（锂电池，单位 mV）
#define BAT_EMPTY_MV      3000
#define BAT_FULL_MV       4200

/**
 * @brief 初始化电池电压 ADC（单次采样 + 校准）
 */
void battery_init(void);

/**
 * @brief 读取真实电池电压
 *
 * @return 电压值（mV）；读取失败返回 -1.0f
 */
float battery_read_voltage_mv(void);

#ifdef __cplusplus
}
#endif
