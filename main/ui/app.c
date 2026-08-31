#include "ui/app.h"
#include "ui/main_ui.h"
#include "ui/notification_panel.h"

#include "core/hardware/display.h"
#include "core/fonts/system_font.h"

#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app";

#define STATUS_BAR_H  46   /* 顶部状态栏高度（同时也是通知栏下拉触发区高度） */

static lv_obj_t *time_label = NULL;

/* 周期刷新顶部状态栏时间（显示系统当前时间，需外部设置系统时间，
   否则未校时前显示 1970 年，见 time_timer_cb 注释） */
static void time_timer_cb(lv_timer_t *timer)
{
    if (time_label == NULL) return;

    char buf[24];
    time_t now = time(NULL);          /* 获取系统时间（POSIX 时间戳） */
    struct tm t;
    localtime_r(&now, &t);            /* 转本地时间（含时区/夏令时，配置在 IDF 的 tz） */
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(time_label, buf);
}

/* 独立顶部状态栏：固定显示时间，返回状态栏句柄供通知栏挂载拖拽事件 */
static lv_obj_t *create_status_bar(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_set_size(status, EXAMPLE_LCD_H_RES, STATUS_BAR_H);
    lv_obj_set_pos(status, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_border_side(status, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(status, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_border_width(status, 1, 0);
    lv_obj_set_style_pad_all(status, 0, 0);

    time_label = lv_label_create(status);
    lv_label_set_text(time_label, "--:--:--");
    lv_obj_set_style_text_font(time_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);

    lv_timer_create(time_timer_cb, 1000, NULL);

    return status;
}

void app_ui_create(void)
{
    ESP_LOGI(TAG, "Create main UI");
    main_ui_create();

    ESP_LOGI(TAG, "Create status bar");
    lv_obj_t *status_bar = create_status_bar();

    ESP_LOGI(TAG, "Create notification panel");
    notification_panel_create(status_bar);
}
