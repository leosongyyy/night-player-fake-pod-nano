#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电池电压采集模块（当前为 **S31 占位实现**）
 *
 * S3 时代：VBAT 经 10K+10K 分压到 GPIO1（ADC1_CH0），ADC 测得的电压 =
 * VBAT / 2。S31（FakePodNano Pro V1）把电池电压/充电状态交给 **AXP2101**
 * 电源管理芯片（I²C0，地址 0x34），不再有分压 ADC 引脚。
 *
 * ⚠ 2026-11 平台迁移阶段 AXP2101 尚未接入：本模块**只保留 API 占位**，
 *   battery_read_voltage_mv() 返回固定的假电压（见 battery.c），
 *   供后续用 AXP2101 电量计替换实现。当前固件已不显示电池电压、
 *   也不做低电拒启/低电自动关机，因此这个假值不会被用户看到或触发动作。
 *   适配 AXP2101 时只需替换 battery.c 的实现，调用方接口不变。
 */

// 电量百分比估算区间（锂电池，单位 mV）
#define BAT_EMPTY_MV      3000
#define BAT_FULL_MV       4200

// 临界低电压（mV）：低于此值视为"电池电压过低"。当前 S31 固件不使用
// （AXP2101 适配后再接入低电策略），保留常量供旧代码/未来实现引用。
#define BATTERY_CRITICAL_LOW_MV 3450

/**
 * @brief 初始化电池电压采集（当前为占位实现：只打印一条说明）
 */
void battery_init(void);

/**
 * @brief 读取电池电压
 *
 * @return 电压值（mV）；S31 占位实现返回固定假值；读取失败返回 -1.0f
 */
float battery_read_voltage_mv(void);

#ifdef __cplusplus
}
#endif
