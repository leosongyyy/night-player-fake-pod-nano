#include "ui/time_date_popup.h"

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "ui/toast.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "time_date";

/* ------------------------------------------------------------------ */
/* 布局与配色（与 settings_menu / eq_popup 的暗色主题一致）              */
/* ------------------------------------------------------------------ */

#define TD_SCREEN_W   EXAMPLE_LCD_H_RES
#define TD_SCREEN_H   EXAMPLE_LCD_V_RES

#define TD_PANEL_W    400
#define TD_PANEL_PAD  16
#define TD_TITLE_H    32
#define TD_ROLLER_H   150
#define TD_ROLLER_COL_H (TD_ROLLER_H + 40)   /* 小标题 + 间距 + 滚轮 */
#define TD_BTN_H      48
#define TD_GAP        12

#define TD_BG         lv_color_hex(0x141414)
#define TD_ROW_BG     lv_color_hex(0x1E1E1E)
#define TD_SEL_BG     lv_color_hex(0x333333)
#define TD_TEXT       lv_color_white()
#define TD_DIM        lv_palette_main(LV_PALETTE_GREY)
#define TD_ACCENT     lv_color_hex(0xFFC000)

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_date_root = NULL;
static lv_obj_t *s_date_year = NULL;
static lv_obj_t *s_date_month = NULL;
static lv_obj_t *s_date_day = NULL;
static int       s_date_day_count = 0;

static lv_obj_t *s_time_root = NULL;
static lv_obj_t *s_time_hour = NULL;
static lv_obj_t *s_time_min = NULL;
static lv_obj_t *s_time_sec = NULL;

static lv_obj_t *s_tz_root = NULL;
static lv_obj_t *s_tz_roller = NULL;

/* 小时区间弹窗（滚动选 开始/结束 小时，两者不可相同） */
static lv_obj_t *s_range_root = NULL;
static lv_obj_t *s_range_start = NULL;
static lv_obj_t *s_range_end = NULL;
static lv_obj_t *s_range_title_lbl = NULL;
static time_date_hour_range_cb_t s_range_cb = NULL;
static char s_hour_opts[24 * 6];   /* "00:00".."23:00"：每项 5 字符 + 换行 */

static void (*s_refresh_cb)(void) = NULL;

/* 年/月/日滚轮的选项字符串（静态存储：roller 不复制字符串） */
static char s_year_opts[100 * 5];
static char s_month_opts[12 * 3];
static char s_day_opts[31 * 3];
static bool s_opts_ready = false;

/* 时区档位表（常用偏移：整点 + 半小时 + 三刻钟） */
typedef struct {
    int minutes;
    const char *label;
} tz_entry_t;

static const tz_entry_t TZ_TABLE[] = {
    { -720, "UTC-12" },
    { -660, "UTC-11" },
    { -600, "UTC-10" },
    { -570, "UTC-9:30" },
    { -540, "UTC-9" },
    { -480, "UTC-8" },
    { -420, "UTC-7" },
    { -360, "UTC-6" },
    { -300, "UTC-5 纽约" },
    { -240, "UTC-4" },
    { -210, "UTC-3:30" },
    { -180, "UTC-3" },
    { -120, "UTC-2" },
    {  -60, "UTC-1" },
    {    0, "UTC+0 伦敦" },
    {   60, "UTC+1 巴黎" },
    {  120, "UTC+2" },
    {  180, "UTC+3" },
    {  210, "UTC+3:30" },
    {  240, "UTC+4" },
    {  270, "UTC+4:30" },
    {  300, "UTC+5" },
    {  330, "UTC+5:30 印度" },
    {  345, "UTC+5:45" },
    {  360, "UTC+6" },
    {  390, "UTC+6:30" },
    {  420, "UTC+7 曼谷" },
    {  480, "UTC+8 北京时间" },
    {  525, "UTC+8:45" },
    {  540, "UTC+9 东京" },
    {  570, "UTC+9:30" },
    {  600, "UTC+10" },
    {  630, "UTC+10:30" },
    {  660, "UTC+11" },
    {  720, "UTC+12" },
    {  765, "UTC+12:45" },
    {  780, "UTC+13" },
    {  840, "UTC+14" },
};
#define TZ_COUNT  (sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]))
static char s_tz_opts[TZ_COUNT][28];

/* ------------------------------------------------------------------ */
/* 格式化辅助                                                          */
/* ------------------------------------------------------------------ */

void time_date_format_date(const rtc_time_t *t, char *buf, size_t size)
{
    if (t == NULL || buf == NULL || size == 0) {
        return;
    }
    snprintf(buf, size, "%04u-%02u-%02u", t->year, t->month, t->day);
}

void time_date_format_time(const rtc_time_t *t, char *buf, size_t size)
{
    if (t == NULL || buf == NULL || size == 0) {
        return;
    }
    snprintf(buf, size, "%02u:%02u:%02u", t->hour, t->minute, t->second);
}

void time_date_format_tz(int minutes, char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }
    int abs_min = (minutes < 0) ? -minutes : minutes;
    snprintf(buf, size, "UTC%c%d:%02d", (minutes < 0) ? '-' : '+',
             abs_min / 60, abs_min % 60);
}

/* ------------------------------------------------------------------ */
/* 选项字符串初始化                                                     */
/* ------------------------------------------------------------------ */

static int days_in_month(int year, int month)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2) {
        bool leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

static void options_init(void)
{
    if (s_opts_ready) {
        return;
    }

    s_year_opts[0] = '\0';
    for (int y = 2000; y <= 2099; y++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", y);
        strcat(s_year_opts, tmp);
        if (y < 2099) {
            strcat(s_year_opts, "\n");
        }
    }
    s_month_opts[0] = '\0';
    for (int m = 1; m <= 12; m++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", m);
        strcat(s_month_opts, tmp);
        if (m < 12) {
            strcat(s_month_opts, "\n");
        }
    }
    s_day_opts[0] = '\0';
    for (int d = 1; d <= 31; d++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", d);
        strcat(s_day_opts, tmp);
        if (d < 31) {
            strcat(s_day_opts, "\n");
        }
    }

    for (size_t i = 0; i < TZ_COUNT; i++) {
        snprintf(s_tz_opts[i], sizeof(s_tz_opts[i]), "%s", TZ_TABLE[i].label);
    }

    /* 小时档位（00:00 ~ 23:00）：小时区间弹窗的两个滚轮共用一份字符串 */
    s_hour_opts[0] = '\0';
    for (int h = 0; h < 24; h++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%02d:00", h);
        strcat(s_hour_opts, tmp);
        if (h < 23) {
            strcat(s_hour_opts, "\n");
        }
    }

    s_opts_ready = true;
}

/* ------------------------------------------------------------------ */
/* 通用控件                                                             */
/* ------------------------------------------------------------------ */

/* 全屏遮罩（点击空白 = 关闭）。返回遮罩对象，调用方再往里放面板 */
static lv_obj_t *popup_root_create(void)
{
    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root, TD_SCREEN_W, TD_SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_60, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

/* 遮罩点击：只有点到遮罩自身（不是面板/控件冒泡）才关闭 */
static void popup_bg_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *root = lv_event_get_user_data(e);
    if (target != root) {
        return;
    }
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

/* 中央面板：固定宽度、纵向 flex */
static lv_obj_t *panel_create(lv_obj_t *root, const char *title, int height)
{
    lv_obj_t *panel = lv_obj_create(root);
    lv_obj_set_size(panel, TD_PANEL_W, height);
    lv_obj_set_pos(panel, (TD_SCREEN_W - TD_PANEL_W) / 2,
                   (TD_SCREEN_H - height) / 2);
    lv_obj_set_style_bg_color(panel, TD_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(panel, TD_PANEL_PAD, 0);
    lv_obj_set_style_pad_row(panel, TD_GAP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_lbl = lv_label_create(panel);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title_lbl, TD_TEXT, 0);

    return panel;
}

/* 滚轮样式：深灰底 + 选中行浅灰底 + 选中文字白色 */
static void roller_style_apply(lv_obj_t *roller)
{
    lv_obj_set_style_text_font(roller, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(roller, TD_DIM, 0);
    lv_obj_set_style_text_color(roller, TD_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, TD_ROW_BG, 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(roller, TD_SEL_BG, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_border_width(roller, 1, 0);
    lv_obj_set_style_border_color(roller, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(roller, 8, 0);
    lv_obj_set_style_pad_all(roller, 4, 0);
}

/* 滚轮列：上方小标题 + 滚轮 */
static lv_obj_t *roller_column_create(lv_obj_t *panel, const char *caption,
                                      const char *options, int width)
{
    lv_obj_t *col = lv_obj_create(panel);
    lv_obj_set_size(col, width, TD_ROLLER_COL_H);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(cap, TD_DIM, 0);

    lv_obj_t *roller = lv_roller_create(col);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 4);
    /* ⚠ set_visible_row_count 会按字体行高重设高度，必须在它之后再钉死尺寸 */
    lv_obj_set_size(roller, width, TD_ROLLER_H);
    roller_style_apply(roller);
    return roller;
}

/* 按钮：primary=true 用黄色底黑字，否则深灰底白字 */
static lv_obj_t *action_button_create(lv_obj_t *parent, const char *text,
                                      bool primary, lv_event_cb_t cb,
                                      void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_height(btn, TD_BTN_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, primary ? TD_ACCENT : TD_ROW_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? lv_color_hex(0xFFD24D)
                                           : lv_color_hex(0x2A2A2A),
                              LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, primary ? lv_color_black() : TD_TEXT, 0);
    lv_obj_center(lbl);

    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

/* 按钮行容器（两个按钮等宽） */
static lv_obj_t *button_row_create(lv_obj_t *panel)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, TD_PANEL_W - TD_PANEL_PAD * 2, TD_BTN_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

static void popup_hide(lv_obj_t *root)
{
    if (root != NULL) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void popup_show(lv_obj_t *root)
{
    if (root == NULL) {
        return;
    }
    lv_obj_move_foreground(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
}

/* 读取当前 RTC 时间（无效/未就绪返回 false） */
static bool read_rtc(rtc_time_t *t)
{
    if (!rtc_is_ready()) {
        return false;
    }
    if (rtc_get_time(t) != ESP_OK) {
        return false;
    }
    return rtc_time_is_sane(t);
}

/* ------------------------------------------------------------------ */
/* 日期弹窗                                                             */
/* ------------------------------------------------------------------ */

static void date_day_options_refresh(int keep_index)
{
    int year = 2000 + (int)lv_roller_get_selected(s_date_year);
    int month = 1 + (int)lv_roller_get_selected(s_date_month);
    int count = days_in_month(year, month);

    if (count != s_date_day_count) {
        char opts[31 * 3];
        opts[0] = '\0';
        for (int d = 1; d <= count; d++) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%d", d);
            strcat(opts, tmp);
            if (d < count) {
                strcat(opts, "\n");
            }
        }
        lv_roller_set_options(s_date_day, opts, LV_ROLLER_MODE_NORMAL);
        s_date_day_count = count;
    }
    if (keep_index < 0) {
        keep_index = 0;
    }
    if (keep_index >= s_date_day_count) {
        keep_index = s_date_day_count - 1;
    }
    lv_roller_set_selected(s_date_day, (uint32_t)keep_index, LV_ANIM_OFF);
}

static void date_year_month_changed_cb(lv_event_t *e)
{
    (void)e;
    /* 保持当前选中的日尽量不变，超出当月天数则收敛到月末 */
    int keep = (int)lv_roller_get_selected(s_date_day);
    date_day_options_refresh(keep);
}

static void date_cancel_cb(lv_event_t *e)
{
    (void)e;
    popup_hide(s_date_root);
}

static void date_save_cb(lv_event_t *e)
{
    (void)e;
    rtc_time_t cur;
    if (!read_rtc(&cur)) {
        ui_toast_show("RTC 不可用，无法设置日期");
        return;
    }
    int year = 2000 + (int)lv_roller_get_selected(s_date_year);
    int month = 1 + (int)lv_roller_get_selected(s_date_month);
    int day = 1 + (int)lv_roller_get_selected(s_date_day);
    int dim = days_in_month(year, month);
    if (day > dim) {
        day = dim;
    }

    cur.year = (uint16_t)year;
    cur.month = (uint8_t)month;
    cur.day = (uint8_t)day;
    cur.weekday = rtc_weekday_from_date(cur.year, cur.month, cur.day);

    if (rtc_set_time(&cur) == ESP_OK) {
        ESP_LOGI(TAG, "date set: %04u-%02u-%02u", cur.year, cur.month, cur.day);
        ui_toast_show("日期已保存");
        popup_hide(s_date_root);
        time_date_popup_notify_changed();
    } else {
        ESP_LOGE(TAG, "rtc_set_time failed (date)");
        ui_toast_show("日期保存失败");
    }
}

static void date_popup_create(void)
{
    s_date_root = popup_root_create();
    lv_obj_add_event_cb(s_date_root, popup_bg_cb, LV_EVENT_CLICKED, s_date_root);

    lv_obj_t *panel = panel_create(s_date_root, "设置日期",
                                   TD_PANEL_PAD * 2 + TD_TITLE_H + TD_GAP
                                   + TD_ROLLER_COL_H + TD_GAP + TD_BTN_H);
    lv_obj_t *rollers = lv_obj_create(panel);
    lv_obj_set_size(rollers, TD_PANEL_W - TD_PANEL_PAD * 2, TD_ROLLER_COL_H);
    lv_obj_set_style_bg_opa(rollers, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rollers, 0, 0);
    lv_obj_set_style_pad_all(rollers, 0, 0);
    lv_obj_set_style_pad_column(rollers, 8, 0);
    lv_obj_clear_flag(rollers, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(rollers, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rollers, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    const int col_w = (TD_PANEL_W - TD_PANEL_PAD * 2 - 16) / 3;
    s_date_year = roller_column_create(rollers, "年", s_year_opts, col_w);
    s_date_month = roller_column_create(rollers, "月", s_month_opts, col_w);
    s_date_day = roller_column_create(rollers, "日", s_day_opts, col_w);
    s_date_day_count = 31;

    lv_obj_add_event_cb(s_date_year, date_year_month_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_date_month, date_year_month_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *btns = button_row_create(panel);
    action_button_create(btns, "取消", false, date_cancel_cb, NULL);
    action_button_create(btns, "保存", true, date_save_cb, NULL);
}

void time_date_popup_show_date(void)
{
    options_init();
    if (s_date_root == NULL) {
        date_popup_create();
    }
    rtc_time_t cur;
    if (!read_rtc(&cur)) {
        ui_toast_show("RTC 不可用，无法设置日期");
        return;
    }
    if (cur.year < 2000 || cur.year > 2099) {
        cur.year = 2026;
    }
    if (cur.month < 1 || cur.month > 12) {
        cur.month = 1;
    }
    lv_roller_set_selected(s_date_year, (uint32_t)(cur.year - 2000),
                           LV_ANIM_OFF);
    lv_roller_set_selected(s_date_month, (uint32_t)(cur.month - 1),
                           LV_ANIM_OFF);
    date_day_options_refresh((int)cur.day - 1);
    popup_show(s_date_root);
    ESP_LOGI(TAG, "date popup shown (%04u-%02u-%02u)", cur.year, cur.month,
             cur.day);
}

/* ------------------------------------------------------------------ */
/* 时间弹窗                                                             */
/* ------------------------------------------------------------------ */

static void time_cancel_cb(lv_event_t *e)
{
    (void)e;
    popup_hide(s_time_root);
}

static void time_save_cb(lv_event_t *e)
{
    (void)e;
    rtc_time_t cur;
    if (!read_rtc(&cur)) {
        ui_toast_show("RTC 不可用，无法设置时间");
        return;
    }
    cur.hour = (uint8_t)lv_roller_get_selected(s_time_hour);
    cur.minute = (uint8_t)lv_roller_get_selected(s_time_min);
    cur.second = (uint8_t)lv_roller_get_selected(s_time_sec);

    if (rtc_set_time(&cur) == ESP_OK) {
        ESP_LOGI(TAG, "time set: %02u:%02u:%02u", cur.hour, cur.minute,
                 cur.second);
        ui_toast_show("时间已保存");
        popup_hide(s_time_root);
        time_date_popup_notify_changed();
    } else {
        ESP_LOGE(TAG, "rtc_set_time failed (time)");
        ui_toast_show("时间保存失败");
    }
}

static void time_popup_create(void)
{
    s_time_root = popup_root_create();
    lv_obj_add_event_cb(s_time_root, popup_bg_cb, LV_EVENT_CLICKED, s_time_root);

    lv_obj_t *panel = panel_create(s_time_root, "设置时间",
                                   TD_PANEL_PAD * 2 + TD_TITLE_H + TD_GAP
                                   + TD_ROLLER_COL_H + TD_GAP + TD_BTN_H);
    lv_obj_t *rollers = lv_obj_create(panel);
    lv_obj_set_size(rollers, TD_PANEL_W - TD_PANEL_PAD * 2, TD_ROLLER_COL_H);
    lv_obj_set_style_bg_opa(rollers, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rollers, 0, 0);
    lv_obj_set_style_pad_all(rollers, 0, 0);
    lv_obj_set_style_pad_column(rollers, 8, 0);
    lv_obj_clear_flag(rollers, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(rollers, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rollers, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    char opts[24 * 4];
    const int col_w = (TD_PANEL_W - TD_PANEL_PAD * 2 - 16) / 3;

    opts[0] = '\0';
    for (int h = 0; h < 24; h++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%02d", h);
        strcat(opts, tmp);
        if (h < 23) {
            strcat(opts, "\n");
        }
    }
    s_time_hour = roller_column_create(rollers, "时", opts, col_w);

    /* 60 个档位：每个 2 字符 + 换行，约 180 字节 */
    char opts60[200];
    opts60[0] = '\0';
    for (int m = 0; m < 60; m++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%02d", m);
        strcat(opts60, tmp);
        if (m < 59) {
            strcat(opts60, "\n");
        }
    }
    s_time_min = roller_column_create(rollers, "分", opts60, col_w);

    opts60[0] = '\0';
    for (int s = 0; s < 60; s++) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%02d", s);
        strcat(opts60, tmp);
        if (s < 59) {
            strcat(opts60, "\n");
        }
    }
    s_time_sec = roller_column_create(rollers, "秒", opts60, col_w);

    lv_obj_t *btns = button_row_create(panel);
    action_button_create(btns, "取消", false, time_cancel_cb, NULL);
    action_button_create(btns, "保存", true, time_save_cb, NULL);
}

void time_date_popup_show_time(void)
{
    if (s_time_root == NULL) {
        time_popup_create();
    }
    rtc_time_t cur;
    if (!read_rtc(&cur)) {
        ui_toast_show("RTC 不可用，无法设置时间");
        return;
    }
    lv_roller_set_selected(s_time_hour, cur.hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_time_min, cur.minute, LV_ANIM_OFF);
    lv_roller_set_selected(s_time_sec, cur.second, LV_ANIM_OFF);
    popup_show(s_time_root);
    ESP_LOGI(TAG, "time popup shown (%02u:%02u:%02u)", cur.hour, cur.minute,
             cur.second);
}

/* ------------------------------------------------------------------ */
/* 时区弹窗                                                             */
/* ------------------------------------------------------------------ */

static int tz_index_of(int minutes)
{
    for (size_t i = 0; i < TZ_COUNT; i++) {
        if (TZ_TABLE[i].minutes == minutes) {
            return (int)i;
        }
    }
    return -1;
}

/* 把 RTC 平移 delta 分钟（保持绝对时刻不变） */
static bool rtc_shift_minutes(int delta)
{
    if (delta == 0) {
        return true;
    }
    rtc_time_t t;
    if (!read_rtc(&t)) {
        return false;
    }
    struct tm tm_in;
    rtc_time_to_tm(&t, &tm_in);
    time_t abs = timegm(&tm_in);
    if (abs == (time_t)-1) {
        return false;
    }
    abs += (time_t)delta * 60;
    struct tm tm_out;
    if (gmtime_r(&abs, &tm_out) == NULL) {
        return false;
    }
    rtc_time_t nt;
    rtc_time_from_tm(&tm_out, &nt);
    nt.weekday = rtc_weekday_from_date(nt.year, nt.month, nt.day);
    return rtc_set_time(&nt) == ESP_OK;
}

static void tz_cancel_cb(lv_event_t *e)
{
    (void)e;
    popup_hide(s_tz_root);
}

static void tz_save_cb(lv_event_t *e)
{
    (void)e;
    int idx = (int)lv_roller_get_selected(s_tz_roller);
    if (idx < 0 || idx >= (int)TZ_COUNT) {
        popup_hide(s_tz_root);
        return;
    }
    int new_min = TZ_TABLE[idx].minutes;
    int old_min = config_store_get_tz_offset_min();
    if (new_min == old_min) {
        popup_hide(s_tz_root);
        return;
    }

    config_store_set_tz_offset_min(new_min);
    bool shifted = rtc_shift_minutes(new_min - old_min);
    if (!shifted) {
        ESP_LOGW(TAG, "timezone saved but RTC shift failed");
        ui_toast_show("时区已保存，但 RTC 不可用");
    } else {
        ui_toast_show("时区已保存");
    }
    popup_hide(s_tz_root);
    time_date_popup_notify_changed();
    ESP_LOGI(TAG, "timezone %d -> %d min (shift %s)", old_min, new_min,
             shifted ? "ok" : "failed");
}

static void tz_popup_create(void)
{
    char options[TZ_COUNT * 30];
    options[0] = '\0';
    for (size_t i = 0; i < TZ_COUNT; i++) {
        strcat(options, s_tz_opts[i]);
        if (i + 1 < TZ_COUNT) {
            strcat(options, "\n");
        }
    }

    s_tz_root = popup_root_create();
    lv_obj_add_event_cb(s_tz_root, popup_bg_cb, LV_EVENT_CLICKED, s_tz_root);

    lv_obj_t *panel = panel_create(s_tz_root, "时区设置",
                                   TD_PANEL_PAD * 2 + TD_TITLE_H + TD_GAP
                                   + TD_ROLLER_H + TD_GAP + TD_BTN_H);
    s_tz_roller = lv_roller_create(panel);
    lv_roller_set_options(s_tz_roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_tz_roller, 4);
    lv_obj_set_size(s_tz_roller, TD_PANEL_W - TD_PANEL_PAD * 2, TD_ROLLER_H);
    roller_style_apply(s_tz_roller);

    lv_obj_t *btns = button_row_create(panel);
    action_button_create(btns, "取消", false, tz_cancel_cb, NULL);
    action_button_create(btns, "保存", true, tz_save_cb, NULL);
}

void time_date_popup_show_tz(void)
{
    options_init();
    if (s_tz_root == NULL) {
        tz_popup_create();
    }
    int cur = config_store_get_tz_offset_min();
    int idx = tz_index_of(cur);
    if (idx < 0) {
        /* 不在档位表里（理论上不会，setter 已按 15 分钟取整）：就近取整点 */
        idx = tz_index_of((cur >= 0) ? (cur / 60 * 60) : -((-cur) / 60 * 60));
        if (idx < 0) {
            idx = tz_index_of(CONFIG_STORE_TZ_OFFSET_DEFAULT);
        }
    }
    lv_roller_set_selected(s_tz_roller, (uint32_t)idx, LV_ANIM_OFF);
    popup_show(s_tz_root);
    ESP_LOGI(TAG, "tz popup shown (%d min)", cur);
}

/* ------------------------------------------------------------------ */
/* 小时区间弹窗（睡眠定时器 → 时间段：开始 / 结束两个滚轮）              */
/* ------------------------------------------------------------------ */

static void range_cancel_cb(lv_event_t *e)
{
    (void)e;
    popup_hide(s_range_root);
}

/* 保存：两个滚轮都只选整点，**相等时拒绝保存**（时间段不可为空），
   弹 Toast 且保持弹窗，让用户继续调 */
static void range_save_cb(lv_event_t *e)
{
    (void)e;
    uint8_t start = (uint8_t)lv_roller_get_selected(s_range_start);
    uint8_t end = (uint8_t)lv_roller_get_selected(s_range_end);
    if (start == end) {
        ui_toast_show("开始和结束时间不能相同");
        return;
    }
    if (s_range_cb != NULL) {
        s_range_cb(start, end);
    }
    popup_hide(s_range_root);
    ESP_LOGI(TAG, "hour range saved: %02u:00 -> %02u:00", (unsigned)start,
             (unsigned)end);
}

static void range_popup_create(void)
{
    options_init();
    s_range_root = popup_root_create();
    lv_obj_add_event_cb(s_range_root, popup_bg_cb, LV_EVENT_CLICKED, s_range_root);

    lv_obj_t *panel = panel_create(s_range_root, "时间段设置",
                                   TD_PANEL_PAD * 2 + TD_TITLE_H + TD_GAP
                                   + TD_ROLLER_COL_H + TD_GAP + TD_BTN_H);
    s_range_title_lbl = lv_obj_get_child(panel, 0);

    lv_obj_t *rollers = lv_obj_create(panel);
    lv_obj_set_size(rollers, TD_PANEL_W - TD_PANEL_PAD * 2, TD_ROLLER_COL_H);
    lv_obj_set_style_bg_opa(rollers, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rollers, 0, 0);
    lv_obj_set_style_pad_all(rollers, 0, 0);
    lv_obj_set_style_pad_column(rollers, 8, 0);
    lv_obj_clear_flag(rollers, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(rollers, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rollers, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    const int col_w = (TD_PANEL_W - TD_PANEL_PAD * 2 - 8) / 2;
    s_range_start = roller_column_create(rollers, "开始", s_hour_opts, col_w);
    s_range_end = roller_column_create(rollers, "结束", s_hour_opts, col_w);

    lv_obj_t *btns = button_row_create(panel);
    action_button_create(btns, "取消", false, range_cancel_cb, NULL);
    action_button_create(btns, "保存", true, range_save_cb, NULL);
}

void time_date_popup_show_hour_range(const char *title, uint8_t start_hour,
                                     uint8_t end_hour,
                                     time_date_hour_range_cb_t on_save)
{
    options_init();
    if (s_range_root == NULL) {
        range_popup_create();
    }
    s_range_cb = on_save;
    if (title != NULL && s_range_title_lbl != NULL) {
        lv_label_set_text(s_range_title_lbl, title);
    }
    if (start_hour > 23) {
        start_hour = 0;
    }
    if (end_hour > 23) {
        end_hour = 0;
    }
    lv_roller_set_selected(s_range_start, start_hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_range_end, end_hour, LV_ANIM_OFF);
    popup_show(s_range_root);
    ESP_LOGI(TAG, "hour range popup shown (%02u:00 -> %02u:00)",
             (unsigned)start_hour, (unsigned)end_hour);
}

/* ------------------------------------------------------------------ */
/* 对外状态接口                                                         */
/* ------------------------------------------------------------------ */

void time_date_popup_set_refresh_cb(void (*cb)(void))
{
    s_refresh_cb = cb;
}

void time_date_popup_notify_changed(void)
{
    if (s_refresh_cb != NULL) {
        s_refresh_cb();
    }
}

bool time_date_popup_any_visible(void)
{
    return (s_date_root != NULL && !lv_obj_has_flag(s_date_root, LV_OBJ_FLAG_HIDDEN))
        || (s_time_root != NULL && !lv_obj_has_flag(s_time_root, LV_OBJ_FLAG_HIDDEN))
        || (s_tz_root != NULL && !lv_obj_has_flag(s_tz_root, LV_OBJ_FLAG_HIDDEN))
        || (s_range_root != NULL && !lv_obj_has_flag(s_range_root, LV_OBJ_FLAG_HIDDEN));
}
