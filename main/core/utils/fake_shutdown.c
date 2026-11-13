#include "core/utils/fake_shutdown.h"

#include "esp_log.h"

static const char *TAG = "fake_shutdown";

/* ------------------------------------------------------------------ */
/* S31 占位实现（AXP2101 尚未适配）                                      */
/*                                                                     */
/* S3 旧板：伪关机 = 停外设 + esp_deep_sleep_start()，B 键 GPIO0 唤醒；   */
/*          A 键长按由电源侧 3 秒关机芯片断电。                          */
/* S31 新板：关机/开机由 AXP2101（I²C0，0x34）控制，主控没有独立的电源键  */
/*          GPIO，唤醒源也不同。AXP2101 驱动完成前，本模块保留两个入口   */
/*          作为占位：只打印日志、不做硬件动作，保证上层代码结构不变。    */
/*                                                                     */
/* ⚠ 当前没有调用方（睡眠定时器改为停播+息屏，低电/电源键关机已移除）；   */
/*   适配 AXP2101 时在这里实现真正的关机流程即可。                       */
/* ------------------------------------------------------------------ */

void fake_shutdown_enter(void)
{
    ESP_LOGW(TAG, "fake shutdown is a placeholder on ESP32-S31 "
                  "(AXP2101 power-off support TBD), ignored");
}

void fake_shutdown_wait_power_off(void)
{
    ESP_LOGW(TAG, "power-key shutdown is a placeholder on ESP32-S31 "
                  "(AXP2101 support TBD), ignored");
}
