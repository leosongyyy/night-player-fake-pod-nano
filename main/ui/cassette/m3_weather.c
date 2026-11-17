#include "ui/cassette/m3_weather.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m3";

/* Web 原型 const WEATHER 三日示例数据 */
typedef enum { WB_SUNCLOUD = 0, WB_RAIN, WB_SUN, WB_COUNT } cas_badge_t;
typedef struct {
    const char *city;
    int        temp;
    cas_badge_t badge;
    const char *key_label;   /* 三日键下方小字 */
} cas_weather_day_t;

static const cas_weather_day_t s_days[3] = {
    { "本地 · 晴转多云（示例）", 22, WB_SUNCLOUD, "22°C" },
    { "本地 · 小雨（示例）",     19, WB_RAIN,      "19°C 小雨" },
    { "本地 · 晴（示例）",       24, WB_SUN,       "24°C 晴" },
};

/* 布局（page root 为 460×422，状态栏下方） */
#define M3_PAD        CAS_CQW(3)
#define M3_W          (EXAMPLE_LCD_H_RES - 2 * M3_PAD)
#define M3_DIAL_Y     CAS_CQW(1.4)
#define M3_DIAL_H     CAS_CQW(55)
#define M3_GAUGE_Y    (M3_DIAL_Y + M3_DIAL_H + CAS_CQW(1.6))
#define M3_GAUGE_H    CAS_CQW(15)
#define M3_KEYS_Y     (M3_GAUGE_Y + M3_GAUGE_H + CAS_CQW(2))
#define M3_KEYS_H     CAS_CQW(13)

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_city = NULL;
static lv_obj_t *s_temp = NULL;
static lv_obj_t *s_badges[WB_COUNT];
static lv_obj_t *s_keys[3];

/* 徽章内坐标基于 100×100 视图盒，LVGL 缩放到控件尺寸 */
static const lv_point_precise_t s_rays[8][2] = {
    { { 50, 4 },  { 50, 14 } }, { { 50, 86 }, { 50, 96 } },
    { { 4, 50 },  { 14, 50 } }, { { 86, 50 }, { 96, 50 } },
    { { 18, 18 }, { 25, 25 } }, { { 75, 75 }, { 82, 82 } },
    { { 82, 18 }, { 75, 25 } }, { { 18, 82 }, { 25, 75 } },
};
static const lv_point_precise_t s_rain[3][2] = {
    { { 34, 66 }, { 29, 80 } },
    { { 52, 66 }, { 47, 80 } },
    { { 70, 66 }, { 65, 80 } },
};

/* lv_line_set_points 只保存数组地址：用静态池存放缩放后的坐标，避免栈局部悬垂。
   三态徽章合计射线数 = 8(晴转多云) + 3(雨) + 8(晴) = 19，留 24 个槽。 */
static lv_point_precise_t s_line_pts[24][2];
static uint8_t s_line_pts_used = 0;

/* 在 badge 容器内画一个圆（坐标/尺寸按 100×100 → 实际像素换算） */
static lv_obj_t *m3_badge_circle(lv_obj_t *parent, int cx, int cy, int r,
                                 lv_color_t fill)
{
    const int box = lv_obj_get_width(parent);
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, box * r * 2 / 100, box * r * 2 / 100);
    lv_obj_set_pos(c, box * (cx - r) / 100, box * (cy - r) / 100);
    lv_obj_set_style_radius(c, 999, 0);
    lv_obj_set_style_bg_color(c, fill, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, CAS_COL_INK, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void m3_badge_rays(lv_obj_t *parent, const lv_point_precise_t pts[][2],
                          int n, lv_color_t color, int width)
{
    const int box = lv_obj_get_width(parent);
    for (int i = 0; i < n; ++i) {
        lv_obj_t *ln = lv_line_create(parent);
        if (s_line_pts_used >= sizeof(s_line_pts) / sizeof(s_line_pts[0])) {
            continue;
        }
        lv_point_precise_t *slot = s_line_pts[s_line_pts_used++];
        for (int k = 0; k < 2; ++k) {
            slot[k].x = (lv_value_precise_t)(pts[i][k].x * box / 100);
            slot[k].y = (lv_value_precise_t)(pts[i][k].y * box / 100);
        }
        lv_line_set_points(ln, slot, 2);   /* 静态池地址，生命周期与 UI 一致 */
        lv_obj_set_style_line_color(ln, color, 0);
        lv_obj_set_style_line_width(ln, width, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }
}

/** 一朵云：三个圆叠加（近似原型 path） */
static void m3_badge_cloud(lv_obj_t *parent, lv_color_t fill)
{
    m3_badge_circle(parent, 30, 58, 14, fill);
    m3_badge_circle(parent, 47, 50, 17, fill);
    m3_badge_circle(parent, 66, 58, 13, fill);
    m3_badge_circle(parent, 48, 65, 20, fill);
}

static lv_obj_t *m3_badge_create(lv_obj_t *parent, cas_badge_t type)
{
    const int box = CAS_CQW(26);
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, box, box);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);

    if (type == WB_SUNCLOUD) {
        m3_badge_circle(g, 38, 36, 16, CAS_COL_STRIPE_Y);
        m3_badge_rays(g, s_rays, 8, CAS_COL_STRIPE_Y, 2);
        m3_badge_cloud(g, CAS_COL_KEYCAP);
    } else if (type == WB_RAIN) {
        m3_badge_cloud(g, CAS_COL_KEYCAP);
        m3_badge_rays(g, s_rain, 3, CAS_COL_STRIPE_R, 3);
    } else {
        m3_badge_circle(g, 50, 50, 22, CAS_COL_STRIPE_Y);
        m3_badge_rays(g, s_rays, 8, CAS_COL_STRIPE_Y, 3);
    }
    return g;
}

static void m3_day_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 3u) {
        return;
    }
    cas_state_get()->weather_day = (uint8_t)idx;
    cas_m3_refresh();
}

void cas_m3_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;

    /* 米白纸盘 + 顶部三色条 */
    lv_obj_t *dial = lv_obj_create(s_root);
    lv_obj_set_size(dial, M3_W, M3_DIAL_H);
    lv_obj_set_pos(dial, M3_PAD, M3_DIAL_Y);
    lv_obj_add_style(dial, cas_style_paper(), 0);
    lv_obj_set_style_radius(dial, CAS_CQW(2.4), 0);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *stripes = cas_stripes_create(dial, M3_W - CAS_CQW(2), CAS_CQW(1.4));
    lv_obj_set_pos(stripes, CAS_CQW(1), CAS_CQW(1));

    s_city = lv_label_create(dial);
    lv_label_set_text(s_city, s_days[0].city);
    lv_obj_set_style_text_font(s_city, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_city, CAS_COL_INK, 0);
    lv_obj_set_pos(s_city, CAS_CQW(3), CAS_CQW(3.2));

    s_temp = lv_label_create(dial);
    lv_label_set_text(s_temp, "22°C");
    lv_obj_set_style_text_font(s_temp, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_temp, CAS_COL_INK, 0);
    lv_obj_set_pos(s_temp, CAS_CQW(3), CAS_CQW(9));

    /* 三态徽章叠放，刷新时切 HIDDEN */
    const int box = CAS_CQW(26);
    for (int i = 0; i < WB_COUNT; ++i) {
        s_badges[i] = m3_badge_create(dial, (cas_badge_t)i);
        lv_obj_set_pos(s_badges[i], M3_W - box - CAS_CQW(3), CAS_CQW(4));
    }

    /* 三表：湿度 / 东南风 / 气压 */
    const char *gauge_lbl[3] = { "湿度", "东南风", "气压" };
    const char *gauge_val[3] = { "58%", "3级", "1013" };
    const int gw = (M3_W - 2 * CAS_CQW(2)) / 3;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *win = lv_obj_create(s_root);
        lv_obj_set_size(win, gw, M3_GAUGE_H);
        lv_obj_set_pos(win, M3_PAD + i * (gw + CAS_CQW(2)), M3_GAUGE_Y);
        lv_obj_add_style(win, cas_style_metal(), 0);
        lv_obj_set_style_radius(win, 6, 0);
        lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(win, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(win);
        lv_label_set_text(l, gauge_lbl[i]);
        lv_obj_set_style_text_font(l, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(l, CAS_COL_DIM, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 3);
        lv_obj_t *v = lv_label_create(win);
        lv_label_set_text(v, gauge_val[i]);
        lv_obj_set_style_text_font(v, CAS_FONT_NUM, 0);
        lv_obj_set_style_text_color(v, CAS_COL_AMBER_HI, 0);
        lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -3);
    }

    /* 三日切换键 */
    const int kw = (M3_W - 2 * CAS_CQW(2)) / 3;
    for (int i = 0; i < 3; ++i) {
        s_keys[i] = cas_metal_key_create(s_root, s_days[i].key_label,
                                         i == 0 ? "今日" : (i == 1 ? "明日" : "后日"));
        lv_obj_set_size(s_keys[i], kw, M3_KEYS_H);
        lv_obj_set_pos(s_keys[i], M3_PAD + i * (kw + CAS_CQW(2)), M3_KEYS_Y);
        lv_obj_add_event_cb(s_keys[i], m3_day_key_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    cas_m3_refresh();
    ESP_LOGI(TAG, "M3 created: paper dial + 3 programmatic badges + 3 gauges");
}

void cas_m3_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    const uint8_t day = cas_state_get()->weather_day;
    const cas_weather_day_t *w = &s_days[day < 3 ? day : 0];

    if (s_city != NULL) {
        lv_label_set_text(s_city, w->city);
    }
    if (s_temp != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d°C", w->temp);
        lv_label_set_text(s_temp, buf);
    }
    for (int i = 0; i < WB_COUNT; ++i) {
        if (s_badges[i] == NULL) {
            continue;
        }
        if (i == (int)w->badge) {
            lv_obj_clear_flag(s_badges[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_badges[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 3; ++i) {
        cas_metal_key_set_lit(s_keys[i], i == (int)day);
    }
}
