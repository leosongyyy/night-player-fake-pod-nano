#include "ui/cassette/m6_settings.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "ui/cassette/cassette_fallback.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m6";

#define M6_PAD       CAS_CQW(3)
#define M6_W         (EXAMPLE_LCD_H_RES - 2 * M6_PAD)
#define M6_ROW_H     CAS_CQW(13)
#define M6_ROW_GAP   CAS_CQW(1)
#define M6_ROWS_Y    CAS_CQW(2)
#define M6_HIDDEN_Y  (M6_ROWS_Y + 5 * (M6_ROW_H + M6_ROW_GAP) + CAS_CQW(0.5))

static const char *const SLEEP_LABELS[4] = { "关闭", "15分", "30分", "60分" };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_eq_seg = NULL;
static lv_obj_t *s_key_lever = NULL;
static lv_obj_t *s_key_txt = NULL;
static lv_obj_t *s_bl_lever = NULL;
static lv_obj_t *s_bl_txt = NULL;
static lv_obj_t *s_knob = NULL;
static lv_obj_t *s_sleep_txt = NULL;

/* ---------------------- 事件 ---------------------- */

static void m6_eq_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    if (target == NULL || lv_obj_get_parent(target) != s_eq_seg) {
        return;
    }
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(target);
    if (idx >= 3u) {
        return;
    }
    cas_state_get()->eq_idx = (uint8_t)idx;
    /* P1: audio_eq_set_mode(idx) */
    cas_m6_refresh();
}

static void m6_keytone_toggle_cb(lv_event_t *e)
{
    (void)e;
    cas_state_t *st = cas_state_get();
    st->keytone = !st->keytone;
    /* P1: keytone_enable(st->keytone) */
    cas_m6_refresh();
}

static void m6_backlight_toggle_cb(lv_event_t *e)
{
    (void)e;
    cas_state_t *st = cas_state_get();
    st->backlight = !st->backlight;
    /* P1: display_set_backlight_always_on(st->backlight) */
    cas_m6_refresh();
}

static void m6_sleep_click_cb(lv_event_t *e)
{
    (void)e;
    cas_state_t *st = cas_state_get();
    st->sleep_idx = (st->sleep_idx + 1u) % 4u;
    /* P1: sleep_timer_set_minutes(SLEEP_MIN[st->sleep_idx]) */
    cas_m6_refresh();
}

static void m6_switch_to_legacy_cb(lv_event_t *e)
{
    (void)e;
    /* L1 反向入口：磁带 UI -> 经典（旧）UI。写 ui_mode=LEGACY 后重启，
       boot_enter_normal 走旧 UI；反向恢复由开机 A+B 组合键完成（toggle）。
       ⚠ 此回调在 LVGL 事件上下文，NVS 写入会关缓存——这里仅写一次后立刻重启，
       不碰 LVGL 对象（F16 的例外：仅开机早期/重启前这一刻）。 */
    cassette_ui_mode_set(CAS_UI_MODE_LEGACY);
    esp_restart();
}

/* 凹陷滑槽行容器 */
static lv_obj_t *m6_row(lv_obj_t *parent, int y)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, M6_W, M6_ROW_H);
    lv_obj_set_pos(row, M6_PAD, y);
    lv_obj_add_style(row, cas_style_groove(), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *m6_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(l, CAS_COL_FG, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

void cas_m6_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;

    /* 1. 音效均衡器（三段分段按钮） */
    lv_obj_t *r0 = m6_row(s_root, M6_ROWS_Y);
    m6_row_label(r0, "音效均衡器");
    static const char *const eq_labels[4] = { "复古暖声", "清亮人声", "磁带重低音", NULL };
    s_eq_seg = cas_seg_create(r0, eq_labels);
    lv_obj_align(s_eq_seg, LV_ALIGN_RIGHT_MID, 0, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_eq_seg); ++i) {
        lv_obj_add_event_cb(lv_obj_get_child(s_eq_seg, (int32_t)i),
                            m6_eq_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 2. 机械按键音（拨杆 + 文本） */
    lv_obj_t *r1 = m6_row(s_root, M6_ROWS_Y + (M6_ROW_H + M6_ROW_GAP));
    m6_row_label(r1, "机械按键音");
    s_key_txt = lv_label_create(r1);
    lv_obj_set_style_text_font(s_key_txt, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_key_txt, CAS_COL_AMBER_HI, 0);
    lv_obj_align(s_key_txt, LV_ALIGN_RIGHT_MID, -CAS_CQW(22), 0);
    s_key_lever = cas_lever_create(r1);
    lv_obj_align(s_key_lever, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_key_lever, m6_keytone_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* 3. 背光常亮模式 */
    lv_obj_t *r2 = m6_row(s_root, M6_ROWS_Y + 2 * (M6_ROW_H + M6_ROW_GAP));
    m6_row_label(r2, "背光常亮模式");
    s_bl_txt = lv_label_create(r2);
    lv_obj_set_style_text_font(s_bl_txt, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_bl_txt, CAS_COL_AMBER_HI, 0);
    lv_obj_align(s_bl_txt, LV_ALIGN_RIGHT_MID, -CAS_CQW(22), 0);
    s_bl_lever = cas_lever_create(r2);
    lv_obj_align(s_bl_lever, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_bl_lever, m6_backlight_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* 4. 自动休眠定时（旋钮 4 档） */
    lv_obj_t *r3 = m6_row(s_root, M6_ROWS_Y + 3 * (M6_ROW_H + M6_ROW_GAP));
    m6_row_label(r3, "自动休眠定时");
    s_sleep_txt = lv_label_create(r3);
    lv_obj_set_style_text_font(s_sleep_txt, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_sleep_txt, CAS_COL_AMBER_HI, 0);
    lv_obj_align(s_sleep_txt, LV_ALIGN_RIGHT_MID, -CAS_CQW(19), 0);
    s_knob = cas_knob_create(r3);
    lv_obj_align(s_knob, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_knob, m6_sleep_click_cb, LV_EVENT_CLICKED, NULL);

    /* 5. 关于本机铭牌（米白）+ 隐藏项「切换到经典界面」 */
    lv_obj_t *r4 = m6_row(s_root, M6_ROWS_Y + 4 * (M6_ROW_H + M6_ROW_GAP));
    m6_row_label(r4, "关于本机");
    lv_obj_t *plate = lv_obj_create(r4);
    lv_obj_set_size(plate, M6_W - CAS_CQW(22), M6_ROW_H - CAS_CQW(2));
    lv_obj_align(plate, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(plate, cas_style_paper(), 0);
    lv_obj_set_style_radius(plate, 5, 0);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *about = lv_label_create(plate);
    lv_label_set_text(about, "型号：纳米磁带机 壹型 · 固件 v2.4（示例）");
    lv_obj_set_style_text_font(about, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(about, CAS_COL_INK, 0);
    lv_obj_center(about);

    /* 隐藏项：低对比小字，点击切到经典 UI（L1 反向入口，语义为「去向经典」） */
    lv_obj_t *back = lv_obj_create(s_root);
    lv_obj_set_size(back, M6_W, CAS_CQW(6));
    lv_obj_set_pos(back, M6_PAD, M6_HIDDEN_Y);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "切换到经典界面（示例）");
    lv_obj_set_style_text_font(back_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(back_lbl, CAS_COL_DIM, 0); /* 隐藏：低对比 */
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, m6_switch_to_legacy_cb, LV_EVENT_CLICKED, NULL);

    cas_m6_refresh();
    ESP_LOGI(TAG, "M6 created: 5 rows + hidden switch-to-legacy item");
}

void cas_m6_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    const cas_state_t *st = cas_state_get();

    cas_seg_set_active(s_eq_seg, st->eq_idx);
    cas_lever_set_on(s_key_lever, st->keytone);
    cas_lever_set_on(s_bl_lever, st->backlight);
    if (s_key_txt != NULL) {
        lv_label_set_text(s_key_txt, st->keytone ? "开启" : "关闭");
    }
    if (s_bl_txt != NULL) {
        lv_label_set_text(s_bl_txt, st->backlight ? "开启" : "关闭");
    }
    /* 旋钮：原型 -135° + idx*90°（0.1° 单位） */
    cas_knob_set_angle(s_knob, (-135 + (int32_t)st->sleep_idx * 90) * 10);
    if (s_sleep_txt != NULL) {
        lv_label_set_text(s_sleep_txt,
                          SLEEP_LABELS[st->sleep_idx < 4 ? st->sleep_idx : 0]);
    }
}
