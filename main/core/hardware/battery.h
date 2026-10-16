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

// 临界低电压（mV）：低于此值视为"电池电压过低"——开机拒绝启动
// （ui/boot_flow.c）与运行中持续过低自动伪关机（core/utils/power_service.c）
// 共用同一阈值。与 BAT_EMPTY_MV（0%）之间留 450mV 余量，避免显示层
// 在临界点附近抖动。
// 取值说明（2026-09 Bug 21 实测后定为 3450）：锂电放电曲线在 ~3.5V 之后
// 进入拐点、电压骤降极快——曾用 3200 实测 3.1V 仍继续播放（拐点后掉电
// 速度超过 60 秒计时窗口，尚未触发就跌出采样）。3.45V 位于拐点前的
// 平台段，持续低电计时可靠，且给 0%（BAT_EMPTY_MV=3.0V）留出 450mV 余量。
// ✅ 2026-11 低电拒启路径已实测验证：临时把本值抬到 4200 后，实测电压
//    4190mV 被正确判定为过低并走 boot_flow 的拒启分支（日志
//    "battery too low (4190 mV < 4200 mV), refuse to boot"，停在低电警告屏），
//    验证完已改回 3450。要再测就临时改高、测完记得改回来。
#define BATTERY_CRITICAL_LOW_MV 3450

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
