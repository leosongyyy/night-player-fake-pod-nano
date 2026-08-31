#include "ui/app.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/notification_panel.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app";

/* 顶部状态栏高度（同时也是通知栏下拉触发区高度），统一见 UI_STATUS_BAR_H */
#define STATUS_BAR_H  UI_STATUS_BAR_H

/* 状态栏电量/音量刷新周期（ms）：电压 0.01V 精度，每 5 秒更新一次 */
#define STATUS_BATTERY_REFRESH_MS  5000

/* 当前页面名称（与 main_ui 的四页滑动 tab 下标一一对应） */
static const char *const PAGE_NAMES[] = {
    "目录 / 歌单",   /* tab 0 */
    "播放列表",      /* tab 1 */
    "正在播放",      /* tab 2 */
    "播放设置",      /* tab 3 */
};

static lv_obj_t *s_status_bar     = NULL;  /* 状态栏容器（透明隐藏时保留事件） */
static lv_obj_t *page_title_label = NULL;   /* 状态栏中央：当前页面名称 */
static lv_obj_t *vol_label        = NULL;   /* 状态栏左侧：音量图标+数值（单 label） */
static lv_obj_t *bat_label        = NULL;   /* 状态栏右侧：电池图标+电压（单 label） */

/* 状态栏图标与数值字体：统一用 LVGL 内置 Montserrat 16。
   - 图标：LVGL 内置字体含 LV_SYMBOL 图标字形（系统精简字体不含 PUA 区图标）；
   - 数值：音量/电压只有数字、字母、小数点（非中文），无需系统字体，
     用内置字体即可（比 24px 系统字体更小、更紧凑）。
   状态栏中央的页面名称（中文）仍用 UI_SYSTEM_FONT。 */
#define STATUS_SMALL_FONT  (&lv_font_montserrat_16)

/* 电量图标按百分比分级（对应 LV_SYMBOL_BATTERY_* 五档） */
static const char *status_battery_icon(float pct)
{
    if (pct >= 90.0f) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60.0f) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40.0f) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15.0f) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* 音量图标按音量等级分级（暂无低音量专用图标，低档用音符代替） */
static const char *status_volume_icon(uint8_t vol)
{
    if (vol >= 70) return LV_SYMBOL_VOLUME_MAX;
    if (vol >= 30) return LV_SYMBOL_VOLUME_MID;
    return LV_SYMBOL_AUDIO;
}

/* 状态栏周期刷新：电量（电池图标按百分比分级 + 电压 0.01V）+ 音量（实时跟随）。
   图标与数值拼接为单个 label（图标 + 空格 + 文本）。
   创建后立即手动调用一次填充初始值。 */
static void status_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 电量：电压精确到 0.01V；图标按电量百分比分级 */
    float vbat_mv = battery_read_voltage_mv();
    if (vbat_mv < 0) {
        lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_EMPTY " --.-- V");
    } else {
        float pct = (vbat_mv - BAT_EMPTY_MV) / (float)(BAT_FULL_MV - BAT_EMPTY_MV)
                    * 100.0f;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %.2f V",
                 status_battery_icon(pct), vbat_mv / 1000.0f);
        lv_label_set_text(bat_label, buf);
    }

    /* 音量：当前音量等级（0~100） */
    uint8_t vol = audio_get_volume();
    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), "%s %u",
             status_volume_icon(vol), (unsigned)vol);
    lv_label_set_text(vol_label, vol_buf);
}

/* 独立顶部状态栏：左=音量（图标+等级）、中=当前页面名称、右=电量（图标+电压）。
   返回状态栏句柄供通知栏挂载拖拽事件。 */
static lv_obj_t *create_status_bar(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_set_size(status, EXAMPLE_LCD_H_RES, STATUS_BAR_H);
    lv_obj_set_pos(status, 0, 0);
    s_status_bar = status;
    /* 半透明黑背景（50%）：下面主界面内容隐约透出，为沉浸式封面做准备；
       无底部边框线 */
    lv_obj_set_style_bg_color(status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_50, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);

    /* 左侧：音量（图标 + 空格 + 数值，单 label） */
    vol_label = lv_label_create(status);
    lv_label_set_text(vol_label, LV_SYMBOL_VOLUME_MAX " --");
    lv_obj_set_style_text_font(vol_label, STATUS_SMALL_FONT, 0);
    lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
    lv_obj_align(vol_label, LV_ALIGN_LEFT_MID, 12, 0);

    /* 中间：当前页面名称（默认第 1 页"目录 / 歌单"，随 tab 切换更新） */
    page_title_label = lv_label_create(status);
    lv_label_set_text(page_title_label, PAGE_NAMES[0]);
    lv_obj_set_style_text_font(page_title_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(page_title_label, lv_color_white(), 0);
    lv_obj_align(page_title_label, LV_ALIGN_CENTER, 0, 0);

    /* 右侧：电量（电池图标 + 空格 + 电压，单 label，右对齐） */
    bat_label = lv_label_create(status);
    lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_FULL " --.-- V");
    lv_obj_set_style_text_font(bat_label, STATUS_SMALL_FONT, 0);
    lv_obj_set_style_text_color(bat_label, lv_color_white(), 0);
    lv_obj_align(bat_label, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 周期刷新（每 5 秒）+ 立即填充一次初始值 */
    lv_timer_create(status_refresh_timer_cb, STATUS_BATTERY_REFRESH_MS, NULL);
    status_refresh_timer_cb(NULL);

    /* 状态栏创建晚于主界面：开机可能已切到非第 0 页（如默认停在正在播放页），
       需同步一次当前页名称（此前 main_ui_switch_tab 时的同步因状态栏未建被忽略） */
    app_status_set_page(main_ui_get_current_tab());

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

void app_status_set_page(uint8_t tab_index)
{
    /* 状态栏由 main_ui 之后创建，tab 事件可能早于状态栏触发，判空保护 */
    if (page_title_label == NULL) {
        return;
    }
    uint8_t count = (uint8_t)(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]));
    if (tab_index >= count) {
        return;
    }
    lv_label_set_text(page_title_label, PAGE_NAMES[tab_index]);
}

void app_status_set_ui_visible(bool visible)
{
    if (s_status_bar == NULL) {
        return;
    }
    if (visible) {
        /* 恢复：半透明黑背景 + 显示三个内容 label */
        lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_50, 0);
        lv_obj_clear_flag(page_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(vol_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bat_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 隐藏：背景完全透明 + 内容 label 隐藏。
           ⚠ 不删除状态栏对象、不清 CLICKABLE：通知栏下拉事件挂载在
           状态栏上，需保留事件接收（供全屏封面时下拉通知栏） */
        lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(page_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(vol_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bat_label, LV_OBJ_FLAG_HIDDEN);
    }
}
