#include "ui/cassette/m5_list.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_ui.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m5";

/* Web TRACKS：落日磁带机/暖风与旧广播/午夜慢行列车/雨后街角唱片店/琥珀色地平线 */
typedef struct {
    const char *name;
    uint32_t    sec;
} cas_track_t;

static const cas_track_t s_tracks[5] = {
    { "落日磁带机",       261 },
    { "暖风与旧广播",     225 },
    { "午夜慢行列车",     312 },
    { "雨后街角唱片店",   238 },
    { "琥珀色地平线",     246 },
};

#define M5_PAD       CAS_CQW(3)
#define M5_W         (EXAMPLE_LCD_H_RES - 2 * M5_PAD)
#define M5_ROW_H     CAS_CQW(11)
#define M5_ROW_GAP   CAS_CQW(0.8)
#define M5_ROWS_Y    CAS_CQW(1.5)
#define M5_KEYS_Y    (M5_ROWS_Y + 5 * (M5_ROW_H + M5_ROW_GAP) + CAS_CQW(2))
#define M5_KEYS_H    CAS_CQW(11)

typedef enum { M5_PREV = 0, M5_PP, M5_NEXT, M5_SIDE, M5_KEY_COUNT } m5_key_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_rows[5];
static lv_obj_t *s_leds[5];
static lv_obj_t *s_keys[M5_KEY_COUNT];

static void m5_format_dur(uint32_t sec, char *buf, size_t len)
{
    snprintf(buf, len, "%02u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

/* 曲目行点击 → 选中并高亮（P0）；P1: player_controller_play_by_index */
static void m5_row_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 5u) {
        return;
    }
    cas_state_get()->track_idx = (uint8_t)idx;
    cas_m5_refresh();
}

static void m5_key_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(e);
    if (raw >= (uintptr_t)M5_KEY_COUNT) {
        return;
    }
    cas_state_t *st = cas_state_get();
    switch ((m5_key_t)raw) {
    case M5_PREV:
        st->track_idx = (st->track_idx + 5u - 1u) % 5u;
        break;
    case M5_PP:
        st->play_mode = (st->play_mode == 1u) ? 2u : 1u;
        break;
    case M5_NEXT:
        st->track_idx = (st->track_idx + 1u) % 5u;
        break;
    case M5_SIDE:
        st->side = st->side ? 0u : 1u;
        break;
    default:
        break;
    }
    /* P1: player_controller_prev() / play_pause() / next() / switch_side() */
    cas_m5_refresh();
}

void cas_m5_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;

    for (int i = 0; i < 5; ++i) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, M5_W, M5_ROW_H);
        lv_obj_set_pos(row, M5_PAD, M5_ROWS_Y + i * (M5_ROW_H + M5_ROW_GAP));
        /* 交错米白/深色纸卡 */
        if ((i & 1) == 0) {
            lv_obj_set_style_bg_color(row, CAS_COL_KEYCAP, 0);
            lv_obj_set_style_text_color(row, CAS_COL_INK, 0);
        } else {
            lv_obj_set_style_bg_color(row, CAS_COL_PANEL, 0);
        }
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, CAS_COL_METAL, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, m5_row_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_rows[i] = row;

        /* LED（当前曲点亮） */
        s_leds[i] = cas_led_create(row);
        lv_obj_set_pos(s_leds[i], 4, M5_ROW_H / 2 - CAS_CQW(1.3));

        /* 序号（等宽） */
        lv_obj_t *no = lv_label_create(row);
        char nbuf[4];
        snprintf(nbuf, sizeof(nbuf), "0%d", i + 1);
        lv_label_set_text(no, nbuf);
        lv_obj_set_style_text_font(no, CAS_FONT_NUM, 0);
        lv_obj_set_style_text_color(no, CAS_COL_DIM, 0);
        lv_obj_set_pos(no, CAS_CQW(4), M5_ROW_H / 2 - CAS_CQW(2));

        /* 曲名 */
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s_tracks[i].name);
        lv_obj_set_style_text_font(nm, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, (i & 1) ? CAS_COL_FG : CAS_COL_INK, 0);
        lv_obj_set_pos(nm, CAS_CQW(11), M5_ROW_H / 2 - CAS_CQW(2));

        /* 时长（等宽） */
        lv_obj_t *du = lv_label_create(row);
        char dbuf[8];
        m5_format_dur(s_tracks[i].sec, dbuf, sizeof(dbuf));
        lv_label_set_text(du, dbuf);
        lv_obj_set_style_text_font(du, CAS_FONT_NUM, 0);
        lv_obj_set_style_text_color(du, (i & 1) ? CAS_COL_DIM : CAS_COL_INK, 0);
        lv_obj_align(du, LV_ALIGN_RIGHT_MID, -CAS_CQW(2), 0);

        /* 静态三色均衡器条（P1: 播放态循环起伏） */
        lv_obj_t *eq = cas_stripes_create(row, CAS_CQW(6), CAS_CQW(2.6));
        lv_obj_set_pos(eq, M5_W - CAS_CQW(10), M5_ROW_H / 2 - CAS_CQW(1.3));
    }

    /* 底部四键 */
    static const char *const caps[4] = { "◄", "❚❚", "►", "A/B" };
    static const char *const names[4] = { "上一曲", "播放/暂停", "下一曲", "甲乙面切换" };
    const int kw = (M5_W - 3 * CAS_CQW(1.5)) / 4;
    for (int i = 0; i < M5_KEY_COUNT; ++i) {
        s_keys[i] = cas_metal_key_create(s_root, caps[i], names[i]);
        lv_obj_set_size(s_keys[i], kw, M5_KEYS_H);
        lv_obj_set_pos(s_keys[i], M5_PAD + i * (kw + CAS_CQW(1.5)), M5_KEYS_Y);
        lv_obj_add_event_cb(s_keys[i], m5_key_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    cas_m5_refresh();
    ESP_LOGI(TAG, "M5 created: 5 alternating rows + 4 keys");
}

void cas_m5_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    const cas_state_t *st = cas_state_get();

    for (int i = 0; i < 5; ++i) {
        const bool cur = (i == (int)st->track_idx);
        cas_led_set_on(s_leds[i], cur);
        if (s_rows[i] != NULL) {
            lv_obj_set_style_border_color(s_rows[i],
                                          cur ? CAS_COL_AMBER : CAS_COL_METAL, 0);
            lv_obj_set_style_border_width(s_rows[i], cur ? 2 : 1, 0);
        }
    }
    cas_metal_key_set_lit(s_keys[M5_PP], st->play_mode == 1u);
    cas_metal_key_set_lit(s_keys[M5_SIDE], st->side == 1u);

    /* 甲乙面标题联动状态栏（Web #listHead = 播放列表 · X面） */
    char title[24];
    snprintf(title, sizeof(title), "播放列表 · %s面", st->side ? "乙" : "甲");
    cas_shell_set_title(title);
}
