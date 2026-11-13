#include "core/hardware/battery.h"

#include "esp_log.h"

static const char *TAG = "battery";

/* ------------------------------------------------------------------ */
/* S31 占位实现（AXP2101 尚未适配）                                      */
/*                                                                     */
/* S3 旧板：VBAT 经 10K+10K 分压到 GPIO1（ADC1_CH0），本文件用            */
/*          adc_oneshot + 校准读取真实电压。                              */
/* S31 新板：电池电压/充电状态由 AXP2101（I²C0，0x34）提供，没有分压 ADC   */
/*          引脚。2026-11 平台迁移阶段先保留 API、返回固定假值，等 AXP2101 */
/*          驱动完成后只替换本文件的实现即可，调用方不需要改。            */
/* ⚠ 当前 UI 已不显示电池电压，低电拒启/低电自动关机也已移除，所以这个    */
/*   假值不会被展示或触发任何动作；它只是给未来实现留的稳定接口。        */
/* ------------------------------------------------------------------ */

/* 占位电压（mV）：取锂电典型平台电压，仅用于"代码路径能跑通"，无物理意义 */
#define BATTERY_PLACEHOLDER_MV  3700.0f

void battery_init(void)
{
    ESP_LOGW(TAG, "battery ADC is not available on ESP32-S31; "
                  "using placeholder voltage (AXP2101 support TBD)");
}

float battery_read_voltage_mv(void)
{
    return BATTERY_PLACEHOLDER_MV;
}
