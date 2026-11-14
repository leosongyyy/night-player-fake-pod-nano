#include "ui/clock_text.h"

#include "core/hardware/rtc.h"
#include "core/storage/config_store.h"

#include <stdio.h>

void ui_clock_text(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }

    rtc_time_t t;
    bool ok = rtc_is_ready() && (rtc_get_time(&t) == ESP_OK)
           && rtc_time_is_sane(&t);
    if (!ok) {
        snprintf(buf, size, "--:--");
        return;
    }

    if (config_store_get_time_24h()) {
        snprintf(buf, size, "%02u:%02u", (unsigned)t.hour, (unsigned)t.minute);
        return;
    }

    /* 12 小时制：0 点显示 12，13 点显示 01；不显示上下午 */
    unsigned hour12 = (unsigned)t.hour % 12u;
    if (hour12 == 0) {
        hour12 = 12;
    }
    snprintf(buf, size, "%02u:%02u", hour12, (unsigned)t.minute);
}
