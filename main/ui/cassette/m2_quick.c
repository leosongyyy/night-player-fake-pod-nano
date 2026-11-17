#include "ui/cassette/m2_quick.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui/cassette/cassette_drawer.h"
#include "ui/cassette/cassette_nav.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m2";

#define M2_BODY_W       CAS_CQW(92)    /* 423 */
#define M2_BODY_H       CAS_CQW(51)    /* 235 */
#define M2_ROW_H        CAS_CQW(7.4)   /* 34 */
#define M2_FADER_H      CAS_CQW(5.6)   /* 26 */
#define M2_LABEL_H      CAS_CQW(4.8)   /* 22 */
#define M2_VIEW_MAX     2              /* 独立页 + 抽屉，各一份视图 */

typedef struct {
    lv_obj_t   *root;
    cas_fader_t *bri;
    cas_fader_t *vol;
    lv_obj_t   *bri_val;
    lv_obj_t   *vol_val;
    lv_obj_t   *bt_led;
    lv_obj_t   *bt_val;
    lv_obj_t   *wifi_led;
    lv_obj_t   *wifi_val;
} cas_m2_view_t;

static cas_m2_view_t s_views[M2_VIEW_MAX];
static uint8_t s_view_count = 0;
static lv_obj_t *s_page = NULL;
static bool s_busy = false;

/* ---------------------- 事件 ---------------------- */

/* 从事件目标反查是哪个视图的推子：页与抽屉谁先建都不影响。 */
static cas_fader_t *m2_find_fader(lv_obj_t *slider)
{
    for (uint8_t i = 0; i < s_view_count; ++i) {
        if (s_views[i].bri != NULL && s_views[i].bri->root == slider) {
            return s_views[i].bri;
        }
        if (s_views[i].vol != NULL && s_views[i].vol->root == slider) {
            return s_views[i].vol;
        }
    }
    return NULL;
}

static void m2_bri_changed_cb(lv_event_t *e)
{
    if (s_busy) {
        return;
    }
    cas_fader_t *f = m2_find_fader(lv_event_get_target(e));
    if (f == NULL) {
        return;
    }
    cas_state_get()->bri_pct = cas_fader_get_value(f);
    // P1: display_set_brightness(cas_state_get()->bri_pct * 255 / 100)
    cas_m2_refresh();
}

static void m2_vol_changed_cb(lv_event_t *e)
{
    if (s_busy) {
        return;
    }
    cas_fader_t *f = m2_find_fader(lv_event_get_target(e));
    if (f == NULL) {
        return;
    }
    cas_state_get()->vol_pct = cas_fader_get_value(f);
    // P1: audio_set_volume_level(vol_pct * AUDIO_VOL_LEVEL_MAX / 100)
    cas_m2_refresh();
}

static lv_obj_t *m2_make_sqkey(lv_obj_t *parent, const char *name, int y,
                               lv_obj_t **out_led, lv_obj_t **out_val,
                               uintptr_t which)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, M2_BODY_W, M2_ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_add_style(row, cas_style_metal(), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)which);

    lv_obj_t *led = cas_led_create(row);
    lv_obj_align(led, LV_ALIGN_LEFT_MID, CAS_CQW(2.4), 0);
    *out_led = led;

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(name_lbl, CAS_COL_FG, 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, CAS_CQW(6), 0);

    *out_val = lv_label_create(row);
    lv_label_set_text(*out_val, "已连接");
    lv_obj_set_style_text_font(*out_val, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(*out_val, CAS_COL_AMBER_HI, 0);
    lv_obj_align(*out_val, LV_ALIGN_RIGHT_MID, -CAS_CQW(2), 0);
    return row;
}

static void m2_toggle_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t which = (uintptr_t)lv_event_get_user_data(e);
    cas_state_t *st = cas_state_get();
    if (which == 1u) {
        st->bt = !st->bt;
        // P1: bt_service_set_enabled(st->bt)
    } else if (which == 2u) {
        st->wifi = !st->wifi;
        // P1: wifi 开关联动（里世界固件）
    }
    cas_m2_refresh();
}

static void m2_settings_cb(lv_event_t *e)
{
    (void)e;
    cas_nav_show(PAGE_SETTINGS);
    cas_drawer_close();
}

static void m2_make_frow(lv_obj_t *parent, cas_m2_view_t *v, bool is_bri, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, is_bri ? "屏幕亮度" : "音量调节");
    lv_obj_set_size(label, CAS_CQW(28), M2_LABEL_H);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_style_text_font(label, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, CAS_COL_FG, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, is_bri ? "85%" : "70%");
    lv_obj_set_size(val, CAS_CQW(16), M2_LABEL_H);
    lv_obj_set_pos(val, M2_BODY_W - CAS_CQW(16), y);
    lv_obj_set_style_text_font(val, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(val, CAS_COL_AMBER_HI, 0);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_SCROLLABLE);

    cas_fader_t *f = cas_fader_create(parent,
                                      is_bri ? CAS_FADER_BRI : CAS_FADER_VOL);
    if (f != NULL && f->root != NULL) {
        lv_obj_set_size(f->root, M2_BODY_W, M2_FADER_H);
        lv_obj_set_pos(f->root, 0, y + M2_LABEL_H + 2);
        cas_fader_set_on_change(
            f, is_bri ? m2_bri_changed_cb : m2_vol_changed_cb,
            (void *)(uintptr_t)(is_bri ? 1u : 2u));
    }
    if (is_bri) {
        v->bri = f;
        v->bri_val = val;
    } else {
        v->vol = f;
        v->vol_val = val;
    }
}

void *cas_m2_build_body(lv_obj_t *parent)
{
    if (parent == NULL || s_view_count >= M2_VIEW_MAX) {
        return NULL;
    }
    cas_m2_view_t *v = &s_views[s_view_count++];
    v->root = parent;

    m2_make_frow(parent, v, true, 0);
    m2_make_frow(parent, v, false, M2_LABEL_H + M2_FADER_H + 8);

    const int y_bt = 2 * (M2_LABEL_H + M2_FADER_H + 8) + 4;
    lv_obj_t *bt = m2_make_sqkey(parent, "蓝牙", y_bt, &v->bt_led, &v->bt_val,
                                 1u);
    lv_obj_add_event_cb(bt, m2_toggle_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)1u);

    lv_obj_t *wifi = m2_make_sqkey(parent, "无线网络", y_bt + M2_ROW_H + 6,
                                   &v->wifi_led, &v->wifi_val, 2u);
    lv_obj_add_event_cb(wifi, m2_toggle_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)2u);

    lv_obj_t *settings = lv_obj_create(parent);
    lv_obj_set_size(settings, M2_BODY_W, M2_ROW_H);
    lv_obj_set_pos(settings, 0, y_bt + 2 * (M2_ROW_H + 6));
    lv_obj_add_style(settings, cas_style_metal(), 0);
    lv_obj_clear_flag(settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *settings_lbl = lv_label_create(settings);
    lv_label_set_text(settings_lbl, "进入系统设置 ⚙");
    lv_obj_set_style_text_font(settings_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(settings_lbl, CAS_COL_FG, 0);
    lv_obj_center(settings_lbl);
    lv_obj_add_event_cb(settings, m2_settings_cb, LV_EVENT_CLICKED, NULL);

    cas_m2_refresh();
    return v;
}

void cas_m2_create(lv_obj_t *parent)
{
    if (parent == NULL || s_page != NULL) {
        return;
    }
    /* 页内控件体垂直居中（Web .qbody justify-content:center） */
    const int content_h = 2 * (M2_LABEL_H + M2_FADER_H + 8)
                          + 2 * (M2_ROW_H + 6) + M2_ROW_H + 4;
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, M2_BODY_W, content_h);
    lv_obj_set_pos(s_page, (EXAMPLE_LCD_H_RES - M2_BODY_W) / 2,
                   (EXAMPLE_LCD_V_RES - CAS_STATUS_H - content_h) / 2);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_CLICKABLE);

    (void)cas_m2_build_body(s_page);
    ESP_LOGI(TAG, "M2 page created (%d view(s))", (int)s_view_count);
}

void cas_m2_refresh(void)
{
    if (s_busy) {
        return;
    }
    s_busy = true;
    const cas_state_t *st = cas_state_get();

    for (uint8_t i = 0; i < s_view_count; ++i) {
        cas_m2_view_t *v = &s_views[i];
        char buf[8];

        if (v->bri != NULL) {
            cas_fader_set_value(v->bri, st->bri_pct);
        }
        if (v->bri_val != NULL) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)st->bri_pct);
            lv_label_set_text(v->bri_val, buf);
        }
        if (v->vol != NULL) {
            cas_fader_set_value(v->vol, st->vol_pct);
        }
        if (v->vol_val != NULL) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)st->vol_pct);
            lv_label_set_text(v->vol_val, buf);
        }

        cas_led_set_on(v->bt_led, st->bt);
        cas_led_set_on(v->wifi_led, st->wifi);
        lv_label_set_text(v->bt_val, st->bt ? "已连接" : "已关闭");
        lv_obj_set_style_text_color(v->bt_val,
                                    st->bt ? CAS_COL_AMBER_HI : CAS_COL_DIM, 0);
        lv_label_set_text(v->wifi_val, st->wifi ? "已连接" : "已关闭");
        lv_obj_set_style_text_color(v->wifi_val,
                                    st->wifi ? CAS_COL_AMBER_HI : CAS_COL_DIM, 0);
    }
    s_busy = false;
}
