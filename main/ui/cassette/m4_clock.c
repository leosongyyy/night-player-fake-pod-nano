#include "ui/cassette/m4_clock.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui/cassette/cassette_nav.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m4";

/* 布局（page root 460×422） */
#define M4_PAD        CAS_CQW(3)
#define M4_W          (EXAMPLE_LCD_H_RES - 2 * M4_PAD)
#define M4_DATE_Y     CAS_CQW(1.5)
#define M4_DATE_H     CAS_CQW(9)
#define M4_FLIP_Y     (M4_DATE_Y + M4_DATE_H + CAS_CQW(3))
#define M4_CARD_W     CAS_CQW(19)
#define M4_CARD_H     CAS_CQW(24)
#define M4_TABS_Y     (M4_FLIP_Y + M4_CARD_H + CAS_CQW(3))
#define M4_TABS_H     CAS_CQW(9)
#define M4_PANEL_Y    (M4_TABS_Y + M4_TABS_H + CAS_CQW(2.5))
#define M4_PANEL_H    CAS_CQW(24)
#define M4_TICK_MS    200u   /* 秒表/时钟刷新：200ms 足够秒位且不占 CPU */

static const char *const WEEK_CN[7] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_date = NULL;
static lv_obj_t *s_cards[3];       /* 时/分/秒 */
static lv_obj_t *s_big = NULL;     /* 实时时钟面板大文本 */
static lv_obj_t *s_alarm_big = NULL;
static lv_obj_t *s_alarm_lever = NULL;
static lv_obj_t *s_alarm_txt = NULL;
static lv_obj_t *s_sw_big = NULL;
static lv_obj_t *s_tabs = NULL;
static lv_obj_t *s_panels[3];
static lv_timer_t *s_tick = NULL;

/* ---------------------- 事件 ---------------------- */

static void m4_tab_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 分段按钮的子按钮 user_data = 下标（cas_seg_create 写入） */
    lv_obj_t *target = lv_event_get_target(e);
    lv_obj_t *btn = (target != NULL && lv_obj_get_parent(target) == s_tabs)
                    ? target : NULL;
    if (btn == NULL) {
        return;
    }
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(btn);
    if (idx >= 3u) {
        return;
    }
    cas_state_get()->cmode = (uint8_t)idx;
    cas_m4_refresh();
}

static void m4_alarm_toggle_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    cas_state_t *st = cas_state_get();
    st->alarm = !st->alarm;
    cas_m4_refresh();
}

static void m4_sw_start_cb(lv_event_t *e)
{
    (void)e;
    cas_stopwatch_t *sw = &cas_state_get()->sw;
    if (!sw->run) {
        sw->last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        sw->run = true;
    }
    cas_m4_refresh();
}

static void m4_sw_pause_cb(lv_event_t *e)
{
    (void)e;
    cas_stopwatch_t *sw = &cas_state_get()->sw;
    if (sw->run) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        sw->acc_ms += now - sw->last_ms;
        sw->run = false;
    }
    cas_m4_refresh();
}

static void m4_sw_reset_cb(lv_event_t *e)
{
    (void)e;
    cas_stopwatch_t *sw = &cas_state_get()->sw;
    sw->run = false;
    sw->acc_ms = 0;
    sw->last_ms = 0;
    cas_m4_refresh();
}

/* ---------------------- 秒表文本 ---------------------- */

/* 定时器回调：转调全量刷新（保持 lv_timer_cb_t 正确签名，避免函数指针强转告警）。
   OBS-05：M4 隐藏时跳过 RTC I2C 读取与秒表刷新，省总线/省电；
   切回 M4 时 cas_nav_show() 会立刻 refresh 一次，显示不会滞后。 */
static void m4_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (cas_nav_active() != PAGE_CLOCK) {
        return;
    }
    cas_m4_refresh();
}

static void m4_format_sw(uint32_t ms, char *buf, size_t len)
{
    if (ms > 3599999u) {
        ms = 3599999u;   /* 59:59.9 上限 */
    }
    snprintf(buf, len, "%02u:%02u.%u",
             (unsigned)(ms / 60000u),
             (unsigned)((ms / 1000u) % 60u),
             (unsigned)((ms / 100u) % 10u));
}

/* 小键（秒表三键 / 面板用）：复用金属琴键工厂，只放一行 cap */
static lv_obj_t *m4_small_key(lv_obj_t *parent, const char *cap, int x, int y,
                              int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *k = cas_metal_key_create(parent, cap, "");
    lv_obj_set_size(k, w, h);
    lv_obj_set_pos(k, x, y);
    if (cb != NULL) {
        lv_obj_add_event_cb(k, cb, LV_EVENT_CLICKED, NULL);
    }
    return k;
}

void cas_m4_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;

    /* 日期铭牌（米白纸面）：初值取 Web「公历 11月17日 · 星期二（示例）」，
       有 RTC 时由 refresh 覆盖为真实日期。 */
    lv_obj_t *plate = lv_obj_create(s_root);
    lv_obj_set_size(plate, M4_W, M4_DATE_H);
    lv_obj_set_pos(plate, M4_PAD, M4_DATE_Y);
    lv_obj_add_style(plate, cas_style_paper(), 0);
    lv_obj_set_style_radius(plate, 6, 0);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_CLICKABLE);
    s_date = lv_label_create(plate);
    lv_label_set_text(s_date, "公历 11月17日 · 星期二（示例）");
    lv_obj_set_style_text_font(s_date, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_date, CAS_COL_INK, 0);
    lv_obj_center(s_date);

    /* 3 组翻页卡 + 2 个琥珀冒号 */
    const int group_w = M4_CARD_W + CAS_CQW(3) + M4_CARD_W + CAS_CQW(3)
                        + M4_CARD_W;
    int x = (EXAMPLE_LCD_H_RES - group_w) / 2;
    for (int i = 0; i < 3; ++i) {
        s_cards[i] = cas_flip_card_create(s_root);
        lv_obj_set_size(s_cards[i], M4_CARD_W, M4_CARD_H);
        lv_obj_set_pos(s_cards[i], x, M4_FLIP_Y);
        cas_flip_card_set_text(s_cards[i], "--");
        x += M4_CARD_W + CAS_CQW(3);
        if (i < 2) {
            lv_obj_t *colon = lv_label_create(s_root);
            lv_label_set_text(colon, ":");
            lv_obj_set_style_text_font(colon, CAS_FONT_NUM, 0);
            lv_obj_set_style_text_color(colon, CAS_COL_AMBER, 0);
            lv_obj_set_pos(colon, x - CAS_CQW(3) + CAS_CQW(0.3),
                           M4_FLIP_Y + M4_CARD_H / 3);
            lv_obj_clear_flag(colon, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* 三 tab（分段按钮，复用控件工厂） */
    static const char *const tabs[4] = { "实时时钟", "机械闹钟", "秒表计时", NULL };
    s_tabs = cas_seg_create(s_root, tabs);
    lv_obj_set_size(s_tabs, M4_W, M4_TABS_H);
    lv_obj_set_pos(s_tabs, M4_PAD, M4_TABS_Y);
    const int tab_w = M4_W / 3 - CAS_CQW(1);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_tabs); ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_tabs, (int32_t)i);
        /* cas_seg_create 按 CAS_CQW(26) 定位；这里重排宽度与 x 避免重叠 */
        lv_obj_set_size(btn, tab_w, M4_TABS_H - CAS_CQW(0.5));
        lv_obj_set_pos(btn, (int)i * (tab_w + CAS_CQW(1)), 0);
        lv_obj_add_event_cb(btn, m4_tab_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 面板 0：实时时钟 */
    s_panels[0] = lv_obj_create(s_root);
    lv_obj_set_size(s_panels[0], M4_W, M4_PANEL_H);
    lv_obj_set_pos(s_panels[0], M4_PAD, M4_PANEL_Y);
    lv_obj_set_style_bg_opa(s_panels[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panels[0], 0, 0);
    lv_obj_set_style_pad_all(s_panels[0], 0, 0);
    lv_obj_clear_flag(s_panels[0], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cl0 = lv_label_create(s_panels[0]);
    lv_label_set_text(cl0, "钨丝翻页机芯 · 走时中");
    lv_obj_set_style_text_font(cl0, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(cl0, CAS_COL_DIM, 0);
    lv_obj_set_pos(cl0, 0, CAS_CQW(2));
    s_big = lv_label_create(s_panels[0]);
    lv_obj_set_style_text_font(s_big, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_big, CAS_COL_AMBER_HI, 0);
    lv_obj_set_pos(s_big, 0, CAS_CQW(9));

    /* 面板 1：机械闹钟 */
    s_panels[1] = lv_obj_create(s_root);
    lv_obj_set_size(s_panels[1], M4_W, M4_PANEL_H);
    lv_obj_set_pos(s_panels[1], M4_PAD, M4_PANEL_Y);
    lv_obj_set_style_bg_opa(s_panels[1], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panels[1], 0, 0);
    lv_obj_set_style_pad_all(s_panels[1], 0, 0);
    lv_obj_clear_flag(s_panels[1], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cl1 = lv_label_create(s_panels[1]);
    lv_label_set_text(cl1, "机械闹钟 · 铃响时刻");
    lv_obj_set_style_text_font(cl1, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(cl1, CAS_COL_DIM, 0);
    lv_obj_set_pos(cl1, 0, CAS_CQW(2));
    s_alarm_big = lv_label_create(s_panels[1]);
    lv_label_set_text(s_alarm_big, "07:30");
    lv_obj_set_style_text_font(s_alarm_big, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_alarm_big, CAS_COL_AMBER_HI, 0);
    lv_obj_set_pos(s_alarm_big, 0, CAS_CQW(9));
    s_alarm_lever = cas_lever_create(s_panels[1]);
    lv_obj_set_pos(s_alarm_lever, M4_W - CAS_CQW(20), CAS_CQW(8));
    lv_obj_add_event_cb(s_alarm_lever, m4_alarm_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_alarm_txt = lv_label_create(s_panels[1]);
    lv_obj_set_style_text_font(s_alarm_txt, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_alarm_txt, CAS_COL_FG, 0);
    lv_obj_set_pos(s_alarm_txt, M4_W - CAS_CQW(28), CAS_CQW(3));

    /* 面板 2：秒表 */
    s_panels[2] = lv_obj_create(s_root);
    lv_obj_set_size(s_panels[2], M4_W, M4_PANEL_H);
    lv_obj_set_pos(s_panels[2], M4_PAD, M4_PANEL_Y);
    lv_obj_set_style_bg_opa(s_panels[2], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panels[2], 0, 0);
    lv_obj_set_style_pad_all(s_panels[2], 0, 0);
    lv_obj_clear_flag(s_panels[2], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cl2 = lv_label_create(s_panels[2]);
    lv_label_set_text(cl2, "秒表计时");
    lv_obj_set_style_text_font(cl2, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(cl2, CAS_COL_DIM, 0);
    lv_obj_set_pos(cl2, 0, CAS_CQW(2));
    s_sw_big = lv_label_create(s_panels[2]);
    lv_obj_set_style_text_font(s_sw_big, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_sw_big, CAS_COL_AMBER_HI, 0);
    lv_obj_set_pos(s_sw_big, 0, CAS_CQW(9));
    const int swkw = (M4_W - 2 * CAS_CQW(2)) / 3;
    m4_small_key(s_panels[2], "开始", 0, CAS_CQW(17), swkw, CAS_CQW(6),
                 m4_sw_start_cb);
    m4_small_key(s_panels[2], "暂停", swkw + CAS_CQW(2), CAS_CQW(17),
                 swkw, CAS_CQW(6), m4_sw_pause_cb);
    m4_small_key(s_panels[2], "归零", 2 * (swkw + CAS_CQW(2)), CAS_CQW(17),
                 swkw, CAS_CQW(6), m4_sw_reset_cb);

    /* 定时刷新：秒表运行中每 200ms 刷一次；有 RTC 每秒重读时间 */
    s_tick = lv_timer_create(m4_tick_cb, M4_TICK_MS, NULL);

    cas_m4_refresh();
    ESP_LOGI(TAG, "M4 created: 3 flip cards + 3 tabs + stopwatch timer");
}

void cas_m4_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    cas_state_t *st = cas_state_get();

    /* --- RTC：真实时/分/秒；失败退化 "--"（Q4/U5） --- */
    rtc_time_t now;
    bool rtc_ok = (rtc_get_time(&now) == ESP_OK);
    char hh[3] = "--", mm[3] = "--", ss[3] = "--";
    if (rtc_ok) {
        snprintf(hh, sizeof(hh), "%02u", (unsigned)now.hour);
        snprintf(mm, sizeof(mm), "%02u", (unsigned)now.minute);
        snprintf(ss, sizeof(ss), "%02u", (unsigned)now.second);
        if (s_date != NULL) {
            char dbuf[40];
            snprintf(dbuf, sizeof(dbuf), "公历 %u月%u日 · %s",
                     (unsigned)now.month, (unsigned)now.day,
                     WEEK_CN[now.weekday < 7 ? now.weekday : 0]);
            lv_label_set_text(s_date, dbuf);
        }
    }
    cas_flip_card_set_text(s_cards[0], hh);
    cas_flip_card_set_text(s_cards[1], mm);
    cas_flip_card_set_text(s_cards[2], ss);
    if (s_big != NULL) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%s:%s:%s", hh, mm, ss);
        lv_label_set_text(s_big, tbuf);
    }

    /* --- 三 tab + 三面板显隐 --- */
    cas_seg_set_active(s_tabs, st->cmode);
    for (int i = 0; i < 3; ++i) {
        if (s_panels[i] == NULL) {
            continue;
        }
        if (i == (int)st->cmode) {
            lv_obj_clear_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* --- 闹钟拨杆 --- */
    cas_lever_set_on(s_alarm_lever, st->alarm);
    if (s_alarm_txt != NULL) {
        lv_label_set_text(s_alarm_txt, st->alarm ? "开启" : "关闭");
    }

    /* --- 秒表 --- */
    uint32_t elapsed = st->sw.acc_ms;
    if (st->sw.run) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        elapsed += now_ms - st->sw.last_ms;
    }
    char swbuf[16];
    m4_format_sw(elapsed, swbuf, sizeof(swbuf));
    if (s_sw_big != NULL) {
        lv_label_set_text(s_sw_big, swbuf);
    }
}
