#include "ui/battery_indicator.h"

#include <stdio.h>

#include "core/hardware/battery.h"

/* 低电量配色（与 iOS 控制中心接近）：≤20% 黄、≤10% 红 */
#define BATT_COLOR_LOW    lv_color_hex(0xFFD60A)
#define BATT_COLOR_CRIT   lv_color_hex(0xFF453A)
#define BATT_COLOR_NONE   lv_color_hex(0x8E8E93)

bool battery_indicator_text(char *buf, size_t size, lv_color_t *color)
{
    if (buf == NULL || size == 0) {
        return false;
    }

    battery_info_t info;
    if (battery_read_info(&info) != ESP_OK || info.percent == 0xFF) {
        snprintf(buf, size, LV_SYMBOL_BATTERY_EMPTY " --%%");
        if (color != NULL) {
            *color = BATT_COLOR_NONE;
        }
        return false;
    }

    /* 图标：充电中优先闪电；否则按电量档位用内置电池图标 */
    const char *icon;
    if (info.charging) {
        icon = LV_SYMBOL_CHARGE;
    } else if (info.percent >= 90) {
        icon = LV_SYMBOL_BATTERY_FULL;
    } else if (info.percent >= 65) {
        icon = LV_SYMBOL_BATTERY_3;
    } else if (info.percent >= 40) {
        icon = LV_SYMBOL_BATTERY_2;
    } else if (info.percent >= 15) {
        icon = LV_SYMBOL_BATTERY_1;
    } else {
        icon = LV_SYMBOL_BATTERY_EMPTY;
    }

    snprintf(buf, size, "%s %u%%", icon, (unsigned)info.percent);

    if (color != NULL) {
        if (info.percent <= 10) {
            *color = BATT_COLOR_CRIT;
        } else if (info.percent <= 20) {
            *color = BATT_COLOR_LOW;
        } else {
            *color = lv_color_white();
        }
    }
    return true;
}
