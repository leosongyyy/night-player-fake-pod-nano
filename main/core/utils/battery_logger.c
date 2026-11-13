#include "core/utils/battery_logger.h"

#include "esp_log.h"

static const char *TAG = "batt_log";

/* ------------------------------------------------------------------ */
/* S31 占位实现（AXP2101 尚未适配）                                      */
/*                                                                     */
/* S3 旧板：开发者选项开启时建后台任务，按间隔采样 battery_read_voltage_mv() */
/*         并把"运行时长 + 平均电压"写入 /sdcard/battery_log.txt。         */
/* S31 新板：电池信息由 AXP2101 提供（电量计/充电状态），battery_read_    */
/*          voltage_mv() 当前本身也是占位假值，记录假数据没有意义。        */
/* 因此本函数暂时只打印说明、不建任务；AXP2101 驱动完成后在这里恢复采样     */
/* 任务即可（config_store 的 battery_log / battery_log_min 开关仍然保留）。*/
/* ------------------------------------------------------------------ */

void battery_logger_init(void)
{
    ESP_LOGW(TAG, "battery logger disabled on ESP32-S31 "
                  "(AXP2101 support TBD)");
}
