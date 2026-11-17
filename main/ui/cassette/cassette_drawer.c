#include "ui/cassette/cassette_drawer.h"

#include <stdint.h>

#include "esp_log.h"
#include "lvgl.h"

#include "ui/cassette/cassette_theme.h"
#include "ui/cassette/m2_quick.h"

static const char *TAG = "cas_drawer";

#define CAS_DRAWER_OPEN_Y    0
#define CAS_DRAWER_CLOSED_Y  (-EXAMPLE_LCD_V_RES)
#define CAS_DRAWER_ANIM_MS   200u

/* 布局（基于 460×460，全部用 CAS_CQW 换算） */
#define CAS_DRAWER_STRIPE_H  CAS_CQW(1.2)    /* 顶部三色条约 6px */
#define CAS_DRAWER_TITLE_Y   CAS_CQW(3.0)
#define CAS_DRAWER_BTN_W     CAS_CQW(21)
#define CAS_DRAWER_BTN_H     CAS_CQW(9)
#define CAS_DRAWER_BTN_X0    CAS_CQW(4)
#define CAS_DRAWER_BTN_Y0    CAS_CQW(17)
#define CAS_DRAWER_BTN_GAP_X CAS_CQW(2)
#define CAS_DRAWER_BTN_GAP_Y CAS_CQW(1.5)
#define CAS_DRAWER_BTN_COLS  4

static lv_obj_t *s_drawer = NULL;
static lv_obj_t *s_nav_btns[PAGE_COUNT];
static lv_obj_t *s_nav_labels[PAGE_COUNT];
static lv_obj_t *s_body = NULL;         /* T03: cas_m2_build_body() 挂这里 */
static bool s_is_open = false;

/* 动画回调：只改 y（同 notification_panel.c 的 notif_anim_cb） */
static void cas_drawer_anim_cb(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, value);
}

static void cas_drawer_animate_to(int32_t target)
{
    if (s_drawer == NULL) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, cas_drawer_anim_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_drawer), target);
    lv_anim_set_time(&a, CAS_DRAWER_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void cas_drawer_nav_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t raw = (uintptr_t)lv_event_get_user_data(e);
    if (raw >= (uintptr_t)PAGE_COUNT) {
        return;
    }
    cas_nav_show((cas_page_id_t)raw);
    cas_drawer_close();
}

static void cas_drawer_close_btn_cb(lv_event_t *e)
{
    (void)e;
    cas_drawer_close();
}

/* 顶部收起按钮旁的把手提示（点击整条状态栏也由 cassette_ui.c 转发开合） */
static lv_obj_t *cas_drawer_make_nav_button(lv_obj_t *parent, cas_page_id_t id,
                                            int col, int row)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, CAS_DRAWER_BTN_W, CAS_DRAWER_BTN_H);
    lv_obj_set_pos(btn,
                   CAS_DRAWER_BTN_X0 + col * (CAS_DRAWER_BTN_W
                                              + CAS_DRAWER_BTN_GAP_X),
                   CAS_DRAWER_BTN_Y0 + row * (CAS_DRAWER_BTN_H
                                              + CAS_DRAWER_BTN_GAP_Y));
    lv_obj_set_style_bg_color(btn, CAS_COL_METAL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, CAS_COL_GROOVE, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, cas_nav_short_title(id));
    lv_obj_set_style_text_font(label, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, CAS_COL_FG, 0);
    lv_obj_center(label);
    s_nav_labels[id] = label;

    lv_obj_add_event_cb(btn, cas_drawer_nav_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)id);
    return btn;
}

void cas_drawer_create(lv_obj_t *parent)
{
    if (parent == NULL || s_drawer != NULL) {
        return;
    }

    /* 全屏不透明抽屉：屏幕层（父级 cas_root），初始停在上屏幕外。 */
    s_drawer = lv_obj_create(parent);
    lv_obj_set_size(s_drawer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_drawer, 0, CAS_DRAWER_CLOSED_Y);
    lv_obj_set_style_bg_color(s_drawer, CAS_COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_drawer, 0, 0);
    lv_obj_set_style_radius(s_drawer, 0, 0);
    lv_obj_set_style_pad_all(s_drawer, 0, 0);
    lv_obj_clear_flag(s_drawer, LV_OBJ_FLAG_SCROLLABLE);

    /* 头部三色条（黄/橙/红，flex 语义用等宽绝对定位实现，避免布局耦合） */
    const lv_color_t stripes[3] = {
        CAS_COL_STRIPE_Y, CAS_COL_STRIPE_O, CAS_COL_STRIPE_R
    };
    const int stripe_w = EXAMPLE_LCD_H_RES / 3;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *bar = lv_obj_create(s_drawer);
        lv_obj_set_size(bar, stripe_w + 1, CAS_DRAWER_STRIPE_H);
        lv_obj_set_pos(bar, i * stripe_w, 0);
        lv_obj_set_style_bg_color(bar, stripes[i], 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 标题：「下拉快捷栏」（Web 抽屉 .dhead b 文案） */
    lv_obj_t *title = lv_label_create(s_drawer);
    lv_label_set_text(title, "下拉快捷栏");
    lv_obj_set_style_text_font(title, CAS_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, CAS_COL_FG, 0);
    lv_obj_set_pos(title, CAS_CQW(4), CAS_DRAWER_TITLE_Y);

    /* 收起按钮 */
    lv_obj_t *close_btn = lv_obj_create(s_drawer);
    lv_obj_set_size(close_btn, CAS_CQW(22), CAS_CQW(8));
    lv_obj_set_pos(close_btn, EXAMPLE_LCD_H_RES - CAS_CQW(26),
                   CAS_CQW(2));
    lv_obj_set_style_bg_color(close_btn, CAS_COL_METAL, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, CAS_COL_GROOVE, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "收起 ▲");
    lv_obj_set_style_text_font(close_lbl, CAS_FONT_BODY, 0);
    lv_obj_set_style_text_color(close_lbl, CAS_COL_AMBER_HI, 0);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, cas_drawer_close_btn_cb, LV_EVENT_CLICKED,
                        NULL);

    /* 7 个模块导航按钮（4 列 × 2 行），点击切页并自动收起 */
    for (int i = 0; i < PAGE_COUNT; ++i) {
        s_nav_btns[i] = cas_drawer_make_nav_button(
            s_drawer, (cas_page_id_t)i, i % CAS_DRAWER_BTN_COLS,
            i / CAS_DRAWER_BTN_COLS);
    }

    /* M2 真实控件体（T03）：与 M2 独立页共用 cas_m2_build_body() 与
       cas_state_t，抽屉里拖亮度/音量、点蓝牙/无线后两处同步刷新。 */
    s_body = lv_obj_create(s_drawer);
    lv_obj_set_size(s_body, EXAMPLE_LCD_H_RES - 2 * CAS_CQW(4),
                    CAS_CQW(52));
    lv_obj_set_pos(s_body, CAS_CQW(4), CAS_CQW(38));
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    (void)cas_m2_build_body(s_body);

    /* P1: 参照 notification_panel.c 的 PRESSED/PRESSING/RELEASED 实现
       状态栏下拉拖拽跟随 + 速度判定；P0 只用点击开合。 */
    s_is_open = false;
    ESP_LOGI(TAG, "drawer created (screen layer, %d nav buttons)", PAGE_COUNT);
}

void cas_drawer_open(void)
{
    if (s_drawer == NULL || s_is_open) {
        return;
    }
    s_is_open = true;
    cas_drawer_animate_to(CAS_DRAWER_OPEN_Y);
    lv_obj_move_foreground(s_drawer);
}

void cas_drawer_close(void)
{
    if (s_drawer == NULL || !s_is_open) {
        return;
    }
    s_is_open = false;
    cas_drawer_animate_to(CAS_DRAWER_CLOSED_Y);
}

void cas_drawer_toggle(void)
{
    if (s_is_open) {
        cas_drawer_close();
    } else {
        cas_drawer_open();
    }
}

bool cas_drawer_is_open(void)
{
    return s_is_open;
}

void cas_drawer_highlight(cas_page_id_t id)
{
    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (s_nav_btns[i] == NULL) {
            continue;
        }
        if (i == (int)id) {
            lv_obj_set_style_bg_color(s_nav_btns[i], CAS_COL_AMBER, 0);
            if (s_nav_labels[i] != NULL) {
                lv_obj_set_style_text_color(s_nav_labels[i], CAS_COL_INK, 0);
            }
        } else {
            lv_obj_set_style_bg_color(s_nav_btns[i], CAS_COL_METAL, 0);
            if (s_nav_labels[i] != NULL) {
                lv_obj_set_style_text_color(s_nav_labels[i], CAS_COL_FG, 0);
            }
        }
    }
}
