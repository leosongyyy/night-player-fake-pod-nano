#include "ui/cassette/m1_player.h"

#include <stdio.h>
#include <string.h>

#include "core/image/cassette_tape.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/cassette_widgets.h"

static const char *TAG = "cas_m1";

/* Web 原型 TRACKS 的时长（秒）：261/225/312/238/246 */
static const uint32_t TRACK_SEC[5] = { 261u, 225u, 312u, 238u, 246u };

/* 布局：page root 为 460×422（状态栏下方），左右留白参照 Web 的 2.6cqw */
#define M1_PAD          CAS_CQW(2.6)         /* 12 */
#define M1_INNER_W      (EXAMPLE_LCD_H_RES - 2 * M1_PAD)
#define M1_BAY_Y        M1_PAD
#define M1_BAY_H        CAS_CQW(61)          /* 281 */
#define M1_PROW_Y       (M1_BAY_Y + M1_BAY_H + CAS_CQW(2.2))
#define M1_PROW_H       CAS_CQW(5.4)         /* 25 */
#define M1_KEYS_Y       (M1_PROW_Y + M1_PROW_H + CAS_CQW(2.2))
#define M1_KEYS_H       CAS_CQW(13)          /* 60 */
#define M1_KEY_GAP      CAS_CQW(1.8)         /* 8 */

typedef enum {
    M1_KEY_RW = 0, M1_KEY_PAUSE, M1_KEY_PLAY, M1_KEY_STOP, M1_KEY_FF,
    M1_KEY_COUNT,
} m1_key_id_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_tapebay = NULL;
static lv_obj_t *s_glow = NULL;
static lv_obj_t *s_cur_label = NULL;
static lv_obj_t *s_rem_label = NULL;
static cas_fader_t *s_fader = NULL;
static lv_obj_t *s_keys[M1_KEY_COUNT];
static bool s_busy = false;   /* 防止 set_value → VALUE_CHANGED 递归 */

static uint32_t m1_total_sec(void)
{
    const cas_state_t *st = cas_state_get();
    if (st->track_idx >= (sizeof(TRACK_SEC) / sizeof(TRACK_SEC[0]))) {
        return TRACK_SEC[0];
    }
    return TRACK_SEC[st->track_idx];
}

static void m1_format_time(uint32_t sec, char *buf, size_t len)
{
    if (sec > 35999u) {
        sec = 35999u;
    }
    snprintf(buf, len, "%02u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

/* 推子拖动 → 更新 RAM 状态（P1: player_controller_seek） */
static void m1_fader_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy || s_fader == NULL) {
        return;
    }
    cas_state_t *st = cas_state_get();
    const uint32_t total = m1_total_sec();
    st->pos_s = (uint32_t)(cas_fader_get_value(s_fader) * total / 100u);
    // P1: player_controller_seek(st->pos_s)
    cas_m1_refresh();
}

static void m1_key_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(e);
    if (raw >= (uintptr_t)M1_KEY_COUNT) {
        return;
    }
    cas_state_t *st = cas_state_get();
    switch ((m1_key_id_t)raw) {
    case M1_KEY_RW:
        st->pos_s = (st->pos_s > 10u) ? (st->pos_s - 10u) : 0u;
        // P1: player_controller_rewind()
        break;
    case M1_KEY_PAUSE:
        st->play_mode = (st->play_mode == 2u) ? 1u : 2u;
        // P1: player_controller_pause()
        break;
    case M1_KEY_PLAY:
        st->play_mode = 1u;
        // P1: player_controller_play()
        break;
    case M1_KEY_STOP:
        st->play_mode = 0u;
        st->pos_s = 0u;
        // P1: player_controller_stop()
        break;
    case M1_KEY_FF: {
        const uint32_t total = m1_total_sec();
        st->pos_s = (st->pos_s + 10u > total) ? total : (st->pos_s + 10u);
        // P1: player_controller_forward()
        break;
    }
    default:
        break;
    }
    cas_m1_refresh();
}

void cas_m1_create(lv_obj_t *parent)
{
    if (parent == NULL || s_root != NULL) {
        return;
    }
    s_root = parent;
    cas_state_t *st = cas_state_get();
    if (st->pos_s > m1_total_sec()) {
        st->pos_s = m1_total_sec();
    }

    /* 磁带仓：深色凹陷槽 */
    s_tapebay = lv_obj_create(s_root);
    lv_obj_set_size(s_tapebay, M1_INNER_W, M1_BAY_H);
    lv_obj_set_pos(s_tapebay, M1_PAD, M1_BAY_Y);
    lv_obj_add_style(s_tapebay, cas_style_groove(), 0);
    lv_obj_set_style_radius(s_tapebay, CAS_CQW(2.4), 0);
    lv_obj_clear_flag(s_tapebay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_tapebay, LV_OBJ_FLAG_CLICKABLE);

    /* 唯一位图：96% 槽高、contain 居中（image const 在 Flash，不占 PSRAM） */
    const int img_size = M1_BAY_H * 96 / 100;
    lv_obj_t *img = lv_image_create(s_tapebay);
    lv_image_set_src(img, CAS_TAPE_IMG);
    lv_obj_set_size(img, img_size, img_size);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    /* P0 直接显示烘焙好的静态卷轴与"甲面/复古金曲集·壹"手写贴纸（F17）。
       P1: 另出 tape_base_noreel + 2 张 64×64 sprite，用 lv_image_set_angle 旋转。 */

    /* 播放态底部琥珀漫反射光晕（Palette 色 + 低不透明度，零位图） */
    s_glow = lv_obj_create(s_tapebay);
    lv_obj_set_size(s_glow, M1_INNER_W - 2 * CAS_CQW(6), CAS_CQW(16));
    lv_obj_set_pos(s_glow, CAS_CQW(6), M1_BAY_H - CAS_CQW(17));
    lv_obj_set_style_bg_color(s_glow, CAS_COL_AMBER, 0);
    lv_obj_set_style_bg_opa(s_glow, 60, 0);
    lv_obj_set_style_border_width(s_glow, 0, 0);
    lv_obj_set_style_radius(s_glow, CAS_CQW(8), 0);
    lv_obj_clear_flag(s_glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_glow, LV_OBJ_FLAG_CLICKABLE);

    /* 进度行：01:24 —— fader —— -02:57 */
    s_cur_label = lv_label_create(s_root);
    lv_obj_set_size(s_cur_label, CAS_CQW(16), M1_PROW_H);
    lv_obj_set_pos(s_cur_label, M1_PAD, M1_PROW_Y);
    lv_obj_set_style_text_font(s_cur_label, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_cur_label, CAS_COL_FG, 0);
    lv_obj_clear_flag(s_cur_label, LV_OBJ_FLAG_SCROLLABLE);

    s_fader = cas_fader_create(s_root, CAS_FADER_PROG);
    if (s_fader != NULL && s_fader->root != NULL) {
        /* 90 + 276 = 366，右侧剩余时间标签从 370 起，留 4px 间隙不重叠 */
        lv_obj_set_size(s_fader->root, CAS_CQW(60), M1_PROW_H);
        lv_obj_set_pos(s_fader->root, M1_PAD + CAS_CQW(17), M1_PROW_Y);
        cas_fader_set_on_change(s_fader, m1_fader_changed_cb, s_fader);
    }

    s_rem_label = lv_label_create(s_root);
    lv_obj_set_size(s_rem_label, CAS_CQW(17), M1_PROW_H);
    lv_obj_set_pos(s_rem_label,
                   EXAMPLE_LCD_H_RES - M1_PAD - CAS_CQW(17), M1_PROW_Y);
    lv_obj_set_style_text_font(s_rem_label, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_rem_label, CAS_COL_DIM, 0);
    lv_obj_clear_flag(s_rem_label, LV_OBJ_FLAG_SCROLLABLE);

    /* 五联琴键 */
    static const char *const caps[M1_KEY_COUNT] = {
        "◄◄", "❚❚", "►", "■", "►►"
    };
    static const char *const names[M1_KEY_COUNT] = {
        "快退", "暂停", "播放", "停止", "快进"
    };
    const int key_w = (M1_INNER_W - 4 * M1_KEY_GAP) / M1_KEY_COUNT;
    for (int i = 0; i < M1_KEY_COUNT; ++i) {
        s_keys[i] = cas_metal_key_create(s_root, caps[i], names[i]);
        lv_obj_set_size(s_keys[i], key_w, M1_KEYS_H);
        lv_obj_set_pos(s_keys[i], M1_PAD + i * (key_w + M1_KEY_GAP),
                       M1_KEYS_Y);
        lv_obj_add_event_cb(s_keys[i], m1_key_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    cas_m1_refresh();
    ESP_LOGI(TAG, "M1 created: tape=%dx%d, 5 keys, progress fader",
             img_size, img_size);
}

void cas_m1_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    s_busy = true;

    const cas_state_t *st = cas_state_get();
    const uint32_t total = m1_total_sec();
    uint32_t pos = st->pos_s;
    if (pos > total) {
        pos = total;
    }

    char buf[16];
    m1_format_time(pos, buf, sizeof(buf));
    lv_label_set_text(s_cur_label, buf);
    snprintf(buf, sizeof(buf), "-%02u:%02u",
             (unsigned)((total - pos) / 60u), (unsigned)((total - pos) % 60u));
    lv_label_set_text(s_rem_label, buf);

    if (s_fader != NULL) {
        cas_fader_set_value(s_fader,
                            (uint8_t)(total ? (pos * 100u / total) : 0u));
    }

    /* 三态与键位一致：Web render() 只用 mode 决定唯一 lit 键 */
    cas_metal_key_set_lit(s_keys[M1_KEY_PLAY], st->play_mode == 1u);
    cas_metal_key_set_lit(s_keys[M1_KEY_PAUSE], st->play_mode == 2u);
    cas_metal_key_set_lit(s_keys[M1_KEY_STOP], st->play_mode == 0u);

    if (s_glow != NULL) {
        if (st->play_mode == 1u) {
            lv_obj_clear_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_glow, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_busy = false;
}
