#include "ui/cassette/cassette_widgets.h"

#include <stdio.h>

#include "lvgl.h"

/* ========================================================================= */
/* 推子：LVGL 9 lv_slider + 部件样式（MAIN/INDICATOR/KNOB），拖拽由 LVGL 处理   */
/* ========================================================================= */

static void cas_fader_value_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    cas_fader_t *f = (cas_fader_t *)lv_event_get_user_data(e);
    if (f == NULL || slider == NULL) {
        return;
    }
    f->value = (uint8_t)lv_slider_get_value(slider);
}

cas_fader_t *cas_fader_create(lv_obj_t *parent, cas_fader_type_t type)
{
    if (parent == NULL) {
        return NULL;
    }
    /* 静态工作集：一屏最多 3 条推子（亮度/音量/进度），无需堆分配。 */
    static cas_fader_t pool[6];
    static uint8_t used = 0;
    if (used >= sizeof(pool) / sizeof(pool[0])) {
        return NULL;
    }
    cas_fader_t *f = &pool[used++];
    f->type = type;
    f->value = 0;

    const int32_t h = (type == CAS_FADER_PROG) ? CAS_CQW(4) : CAS_CQW(5.6);
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, CAS_CQW(90), h);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLLABLE);

    /* 凹槽 */
    lv_obj_set_style_bg_color(slider, CAS_COL_GROOVE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, CAS_COL_METAL, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, h / 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_MAIN);

    /* 填充：亮度黄→橙，进度黄→琥珀，音量琥珀单色 */
    lv_color_t fill = CAS_COL_AMBER;
    lv_color_t fill_end = CAS_COL_STRIPE_O;
    if (type == CAS_FADER_BRI) {
        fill = CAS_COL_STRIPE_Y;
    } else if (type == CAS_FADER_PROG) {
        fill = CAS_COL_STRIPE_Y;
        fill_end = CAS_COL_AMBER;
    }
    lv_obj_set_style_bg_color(slider, fill, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider, fill_end, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, h / 2, LV_PART_INDICATOR);

    /* thumb：金属块（进度用圆形亮色旋钮） */
    const int32_t kw = (type == CAS_FADER_PROG) ? CAS_CQW(6) : CAS_CQW(6.6);
    const int32_t kh = (type == CAS_FADER_PROG) ? CAS_CQW(6) : CAS_CQW(9);
    lv_obj_set_style_width(slider, kw, LV_PART_KNOB);
    lv_obj_set_style_height(slider, kh, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, (type == CAS_FADER_PROG) ? kw / 2 : 4,
                            LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider,
                              (type == CAS_FADER_PROG) ? CAS_COL_KEYCAP
                                                       : CAS_COL_METAL_HI,
                              LV_PART_KNOB);
    lv_obj_set_style_bg_grad_color(slider, CAS_COL_METAL, LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_VER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, CAS_COL_GROOVE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, CAS_COL_GROOVE, LV_PART_KNOB);

    f->root = slider;
    lv_obj_add_event_cb(slider, cas_fader_value_cb, LV_EVENT_VALUE_CHANGED, f);
    return f;
}

void cas_fader_set_value(cas_fader_t *f, uint8_t value)
{
    if (f == NULL || f->root == NULL) {
        return;
    }
    if (value > 100) {
        value = 100;
    }
    f->value = value;
    /* 编程式 set：不依赖 VALUE_CHANGED 事件，显式刷新滑块并让调用方
       随后刷新自己的数值 label（§7.5 的 main_tabs_sync 教训）。 */
    lv_slider_set_value(f->root, (int32_t)value, LV_ANIM_OFF);
    lv_obj_invalidate(f->root);
}

uint8_t cas_fader_get_value(const cas_fader_t *f)
{
    return (f == NULL) ? 0 : f->value;
}

void cas_fader_set_on_change(cas_fader_t *f, lv_event_cb_t cb, void *user_data)
{
    if (f != NULL && f->root != NULL && cb != NULL) {
        lv_obj_add_event_cb(f->root, cb, LV_EVENT_VALUE_CHANGED, user_data);
    }
}

/* ========================================================================= */
/* 金属琴键 / LED / 拨杆 / 旋钮 / 分段 / 翻页卡 / 螺丝 / 三色条 / 容量条        */
/* ========================================================================= */

lv_obj_t *cas_metal_key_create(lv_obj_t *parent, const char *cap,
                               const char *label)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *key = lv_obj_create(parent);
    lv_obj_set_style_bg_color(key, CAS_COL_METAL, 0);
    lv_obj_set_style_bg_grad_color(key, CAS_COL_METAL_HI, 0);
    lv_obj_set_style_bg_grad_dir(key, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(key, 1, 0);
    lv_obj_set_style_border_color(key, CAS_COL_GROOVE, 0);
    lv_obj_set_style_radius(key, 7, 0);
    lv_obj_set_style_pad_all(key, 2, 0);
    lv_obj_set_style_shadow_width(key, 8, 0);
    lv_obj_set_style_shadow_color(key, CAS_COL_GROOVE, 0);
    lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);

    /* 按下：整体下移 4px，阴影收缩 */
    lv_obj_set_style_translate_y(key, 4, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(key, 2, LV_STATE_PRESSED);

    /* 点亮：更暗的底 + 琥珀描边/文字 */
    lv_obj_set_style_bg_grad_color(key, CAS_COL_GROOVE, LV_STATE_CHECKED);
    lv_obj_set_style_translate_y(key, 4, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(key, CAS_COL_AMBER, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(key, CAS_COL_AMBER, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(key, 10, LV_STATE_CHECKED);

    lv_obj_t *cap_lbl = lv_label_create(key);
    lv_label_set_text(cap_lbl, cap == NULL ? "" : cap);
    lv_obj_set_style_text_font(cap_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(cap_lbl, CAS_COL_FG, 0);
    lv_obj_set_style_text_color(cap_lbl, CAS_COL_AMBER_HI, LV_STATE_CHECKED);
    lv_obj_align(cap_lbl, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *txt_lbl = lv_label_create(key);
    lv_label_set_text(txt_lbl, label == NULL ? "" : label);
    lv_obj_set_style_text_font(txt_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(txt_lbl, CAS_COL_DIM, 0);
    lv_obj_set_style_text_color(txt_lbl, CAS_COL_AMBER, LV_STATE_CHECKED);
    lv_obj_align(txt_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
    return key;
}

void cas_metal_key_set_lit(lv_obj_t *key, bool lit)
{
    if (key == NULL) {
        return;
    }
    if (lit) {
        lv_obj_add_state(key, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(key, LV_STATE_CHECKED);
    }
}

lv_obj_t *cas_led_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *led = lv_obj_create(parent);
    const int size = CAS_CQW(2.6);
    lv_obj_set_size(led, size, size);
    lv_obj_set_style_radius(led, size / 2, 0);
    lv_obj_set_style_bg_color(led, CAS_COL_GROOVE, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(led, 1, 0);
    lv_obj_set_style_border_color(led, CAS_COL_METAL, 0);
    lv_obj_set_style_pad_all(led, 0, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE);
    return led;
}

void cas_led_set_on(lv_obj_t *led, bool on)
{
    if (led == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(led, on ? CAS_COL_AMBER : CAS_COL_GROOVE, 0);
    lv_obj_set_style_shadow_width(led, on ? 8 : 0, 0);
    lv_obj_set_style_shadow_color(led, CAS_COL_AMBER, 0);
}

lv_obj_t *cas_lever_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *lever = lv_obj_create(parent);
    lv_obj_set_size(lever, CAS_CQW(18), CAS_CQW(7));
    lv_obj_add_style(lever, cas_style_groove(), 0);
    lv_obj_clear_flag(lever, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *knob = lv_obj_create(lever);
    lv_obj_set_size(knob, CAS_CQW(7), CAS_CQW(6));
    lv_obj_set_pos(knob, 2, 2);
    lv_obj_add_style(knob, cas_style_metal(), 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(lever, knob);
    return lever;
}

void cas_lever_set_on(lv_obj_t *lever, bool on)
{
    if (lever == NULL) {
        return;
    }
    lv_obj_t *knob = (lv_obj_t *)lv_obj_get_user_data(lever);
    if (knob != NULL) {
        lv_obj_set_x(knob, on ? (CAS_CQW(18) - CAS_CQW(7) - 4) : 2);
    }
}

lv_obj_t *cas_knob_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return NULL;
    }
    const int size = CAS_CQW(16);
    lv_obj_t *knob = lv_obj_create(parent);
    lv_obj_set_size(knob, size, size);
    lv_obj_set_style_radius(knob, size / 2, 0);
    lv_obj_set_style_bg_color(knob, CAS_COL_METAL_HI, 0);
    lv_obj_set_style_bg_grad_color(knob, CAS_COL_METAL, 0);
    lv_obj_set_style_bg_grad_dir(knob, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 2, 0);
    lv_obj_set_style_border_color(knob, CAS_COL_GROOVE, 0);
    lv_obj_set_style_shadow_width(knob, 8, 0);
    lv_obj_set_style_shadow_color(knob, CAS_COL_GROOVE, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mark = lv_obj_create(knob);
    lv_obj_set_size(mark, 3, size / 3);
    lv_obj_set_pos(mark, size / 2 - 1, 3);
    lv_obj_set_style_bg_color(mark, CAS_COL_AMBER_HI, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_radius(mark, 2, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(knob, mark);
    return knob;
}

void cas_knob_set_angle(lv_obj_t *knob, int32_t angle10)
{
    if (knob != NULL) {
        /* 指示条是子对象，整体旋转父对象即可；0.1° 单位。 */
        lv_obj_set_style_transform_rotation(knob, angle10, 0);
    }
}

lv_obj_t *cas_seg_create(lv_obj_t *parent, const char *const labels[])
{
    if (parent == NULL || labels == NULL) {
        return NULL;
    }
    uint8_t count = 0;
    while (labels[count] != NULL && count < 8) {
        ++count;
    }
    if (count == 0) {
        return NULL;
    }
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_set_style_pad_all(seg, 0, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < count; ++i) {
        lv_obj_t *btn = lv_obj_create(seg);
        lv_obj_set_size(btn, CAS_CQW(26), CAS_CQW(9));
        lv_obj_set_pos(btn, i * (CAS_CQW(26) + CAS_CQW(2)), 0);
        lv_obj_add_style(btn, cas_style_metal(), 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, CAS_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, CAS_COL_FG, 0);
        lv_obj_center(lbl);
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
    }
    return seg;
}

void cas_seg_set_active(lv_obj_t *seg, uint8_t idx)
{
    if (seg == NULL) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(seg);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *btn = lv_obj_get_child(seg, (int32_t)i);
        if (btn == NULL) {
            continue;
        }
        const bool active = (i == idx);
        lv_obj_set_style_bg_color(btn,
                                  active ? CAS_COL_AMBER : CAS_COL_METAL, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl,
                                        active ? CAS_COL_INK : CAS_COL_FG, 0);
        }
    }
}

lv_obj_t *cas_flip_card_create(lv_obj_t *parent)
{
    if (parent == NULL) {
        return NULL;
    }
    const int w = CAS_CQW(19);
    const int h = CAS_CQW(24);
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, CAS_COL_GROOVE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, CAS_COL_GROOVE, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *top = lv_label_create(card);
    lv_label_set_text(top, "0");
    lv_obj_set_style_text_font(top, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(top, CAS_COL_FG, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, CAS_CQW(1));

    lv_obj_t *bottom = lv_label_create(card);
    lv_label_set_text(bottom, "0");
    lv_obj_set_style_text_font(bottom, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(bottom, CAS_COL_FG, 0);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -CAS_CQW(1));

    lv_obj_t *seam = lv_obj_create(card);
    lv_obj_set_size(seam, w, 3);
    lv_obj_align(seam, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(seam, CAS_COL_GROOVE, 0);
    lv_obj_set_style_bg_opa(seam, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(seam, 0, 0);
    lv_obj_clear_flag(seam, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(seam, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *rivet = lv_obj_create(card);
        lv_obj_set_size(rivet, 7, 7);
        lv_obj_set_style_radius(rivet, 4, 0);
        lv_obj_set_style_bg_color(rivet, CAS_COL_METAL_HI, 0);
        lv_obj_set_style_bg_opa(rivet, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rivet, 0, 0);
        lv_obj_align(rivet, i == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                     0, 0);
        lv_obj_clear_flag(rivet, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(rivet, LV_OBJ_FLAG_CLICKABLE);
    }
    return card;
}

void cas_flip_card_set_digit(lv_obj_t *card, char digit)
{
    if (card == NULL) {
        return;
    }
    char buf[2] = { digit, '\0' };
    lv_obj_t *top = lv_obj_get_child(card, 0);
    lv_obj_t *bottom = lv_obj_get_child(card, 1);
    if (top != NULL) {
        lv_label_set_text(top, buf);
    }
    if (bottom != NULL) {
        lv_label_set_text(bottom, buf);
    }
}

void cas_flip_card_set_text(lv_obj_t *card, const char *text)
{
    if (card == NULL || text == NULL) {
        return;
    }
    /* 卡宽 CAS_CQW(19)≈87px，等宽 24px 字体最多容纳两位，多截保证不溢出 */
    char buf[3];
    snprintf(buf, sizeof(buf), "%.2s", text);
    lv_obj_t *top = lv_obj_get_child(card, 0);
    lv_obj_t *bottom = lv_obj_get_child(card, 1);
    if (top != NULL) {
        lv_label_set_text(top, buf);
    }
    if (bottom != NULL) {
        lv_label_set_text(bottom, buf);
    }
}

lv_obj_t *cas_screw_create(lv_obj_t *parent, int32_t deg10)
{
    if (parent == NULL) {
        return NULL;
    }
    const int size = CAS_CQW(3.6);
    lv_obj_t *screw = lv_obj_create(parent);
    lv_obj_set_size(screw, size, size);
    lv_obj_set_style_radius(screw, 999, 0);
    lv_obj_set_style_bg_color(screw, CAS_COL_METAL_HI, 0);
    lv_obj_set_style_bg_opa(screw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screw, 1, 0);
    lv_obj_set_style_border_color(screw, CAS_COL_GROOVE, 0);
    lv_obj_set_style_pad_all(screw, 0, 0);
    lv_obj_clear_flag(screw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(screw, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *slot = lv_obj_create(screw);
        lv_obj_set_size(slot, size / 2, 2);
        lv_obj_center(slot);
        lv_obj_set_style_bg_color(slot, CAS_COL_GROOVE, 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(slot, 0, 0);
        lv_obj_set_style_radius(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_set_style_transform_rotation(slot, deg10 + i * 900, 0);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    }
    return screw;
}

lv_obj_t *cas_stripes_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_set_size(strip, w, h);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);

    const lv_color_t colors[3] = {
        CAS_COL_STRIPE_Y, CAS_COL_STRIPE_O, CAS_COL_STRIPE_R
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *bar = lv_obj_create(strip);
        lv_obj_set_size(bar, w / 3 + 1, h);
        lv_obj_set_pos(bar, i * (w / 3), 0);
        lv_obj_set_style_bg_color(bar, colors[i], 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    return strip;
}

lv_obj_t *cas_capbar_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_add_style(bar, cas_style_groove(), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_set_size(seg, w / 3 - 2, h - 4);
        lv_obj_set_pos(seg, 2 + i * (w / 3), 2);
        lv_obj_set_style_bg_color(seg, CAS_COL_METAL, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    }
    return bar;
}

void cas_capbar_set_pct(lv_obj_t *bar, uint8_t pct)
{
    if (bar == NULL) {
        return;
    }
    const lv_color_t colors[3] = {
        CAS_COL_STRIPE_Y, CAS_COL_STRIPE_O, CAS_COL_STRIPE_R
    };
    for (uint32_t i = 0; i < 3; ++i) {
        lv_obj_t *seg = lv_obj_get_child(bar, (int32_t)i);
        if (seg == NULL) {
            continue;
        }
        bool filled = (pct >= (uint8_t)((i + 1) * 100 / 3));
        lv_obj_set_style_bg_color(seg, filled ? colors[i] : CAS_COL_METAL, 0);
    }
}
