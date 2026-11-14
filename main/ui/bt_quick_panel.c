#include "ui/bt_quick_panel.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/bt/bt_service.h"
#include "core/hardware/display.h"
#include "core/storage/bt_store.h"
#include "ui/bt_settings.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "bt_panel";

/* ------------------------------------------------------------------ */
/* 尺寸与配色：与设置菜单/蓝牙设置页同一套深色卡片风格                    */
/* ------------------------------------------------------------------ */

#define PANEL_W       380
#define PANEL_H       432
#define PANEL_PAD     16
#define PANEL_GAP     10
#define ROW_H         52
#define ROW_GAP       8
#define FOOTER_H      48

#define CARD_BG_COLOR     lv_color_hex(0x1E1E1E)
#define ROW_BG_COLOR      lv_color_hex(0x2C2C2E)
#define PRESS_COLOR       lv_color_hex(0xFFC000)
#define FOOTER_BG_COLOR   lv_color_hex(0x3A3A3C)
#define TEXT_COLOR        lv_color_white()
#define TEXT_DIM_COLOR    lv_palette_main(LV_PALETTE_GREY)
#define TEXT_HL_COLOR     lv_palette_main(LV_PALETTE_YELLOW)
#define ACCENT_COLOR      lv_color_hex(0x007AFF)
#define ICON_FONT         (&lv_font_montserrat_24)

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t   *s_root = NULL;    /* 全屏遮罩（点击空白关闭） */
static lv_obj_t   *s_card = NULL;
static lv_obj_t   *s_status = NULL;  /* 标题右侧状态文字 */
static lv_obj_t   *s_list = NULL;    /* 设备列表容器（滚动） */
static lv_timer_t *s_timer = NULL;
static bool        s_visible = false;
static uint32_t    s_sig = 0;

static void rebuild(void);

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static const char *panel_status_text(void)
{
    if (!bt_service_is_enabled()) {
        return "已关闭";
    }
    if (bt_service_is_connected()) {
        return bt_service_is_streaming() ? "播放中" : "已连接";
    }
    return "已开启";
}

static uint32_t panel_sig(void)
{
    uint32_t h = bt_service_is_enabled() ? 1u : 2u;
    h = h * 31u + (uint32_t)(bt_service_connected_index() + 2);
    h = h * 31u + (bt_service_is_streaming() ? 3u : 4u);
    h = h * 31u + (uint32_t)bt_service_power_state();
    h = h * 31u + (uint32_t)bt_store_count();
    return h;
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

static void cb_backdrop(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_root) {
        bt_quick_panel_hide();
    }
}

static void cb_device(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!bt_service_is_enabled()) {
        ui_toast_show("请先开启蓝牙");
        return;
    }
    if (bt_service_connected_index() == idx) {
        bt_service_disconnect();
        ui_toast_show("正在断开...");
    } else {
        /* 已连着别的设备：服务层会先断开当前设备、等链路收尾后再连目标；
         * 目标连接失败就保持断开，不回退旧设备 */
        bool switching = (bt_service_connected_index() >= 0);
        bt_service_connect_index(idx);
        ui_toast_show(switching ? "正在切换设备..." : "正在连接...");
    }
    s_sig = 0;   /* 立即重建，让"已连接"状态跟上 */
}

static void cb_open_settings(lv_event_t *e)
{
    (void)e;
    bt_quick_panel_hide();
    bt_settings_show();
}

/* ------------------------------------------------------------------ */
/* 构建                                                                */
/* ------------------------------------------------------------------ */

/* 列表里的设备行：图标 + 名称 + 右侧状态；整行可点（连接/断开） */
static lv_obj_t *row_create(lv_obj_t *parent, const char *name, bool connected,
                            bool streaming, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_bg_color(row, ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, ICON_FONT, 0);
    lv_obj_set_style_text_color(icon, connected ? ACCENT_COLOR : TEXT_DIM_COLOR, 0);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(name_lbl, TEXT_COLOR, 0);
    lv_obj_set_flex_grow(name_lbl, 1);

    lv_obj_t *st = lv_label_create(row);
    lv_label_set_text(st, connected ? (streaming ? "播放中" : "已连接") : "未连接");
    lv_obj_set_style_text_font(st, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(st, connected ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);

    lv_obj_add_event_cb(row, cb_device, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return row;
}

static void rebuild(void)
{
    if (s_list == NULL || s_status == NULL) {
        return;
    }
    lv_obj_clean(s_list);
    lv_label_set_text(s_status, panel_status_text());
    lv_obj_set_style_text_color(
        s_status, bt_service_is_connected() ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);

    int shown = 0;
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        bt_device_t d;
        if (!bt_store_get(i, &d)) {
            continue;
        }
        shown++;
        row_create(s_list, d.name, bt_service_connected_index() == i,
                   bt_service_is_streaming(), i);
    }

    if (shown == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "还没有配对过设备，请到「蓝牙设置」中配对");
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(lbl, TEXT_DIM_COLOR, 0);
    }
}

/* ------------------------------------------------------------------ */
/* 创建 / 显示                                                         */
/* ------------------------------------------------------------------ */

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_visible) {
        return;
    }
    uint32_t sig = panel_sig();
    if (sig != s_sig) {
        s_sig = sig;
        rebuild();
    }
}

static void ensure_created(void)
{
    if (s_root != NULL) {
        return;
    }
    lv_obj_t *top = lv_layer_top();

    /* 全屏遮罩：压暗背景，点击卡片外关闭 */
    s_root = lv_obj_create(top);
    lv_obj_set_size(s_root, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, cb_backdrop, LV_EVENT_CLICKED, NULL);

    /* 居中卡片 */
    s_card = lv_obj_create(s_root);
    lv_obj_set_size(s_card, PANEL_W, PANEL_H);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, CARD_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 18, 0);
    lv_obj_set_style_border_width(s_card, 0, 0);
    lv_obj_set_style_pad_all(s_card, PANEL_PAD, 0);
    lv_obj_set_style_pad_row(s_card, PANEL_GAP, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题行：图标 + 「蓝牙」+ 右侧状态 */
    lv_obj_t *hdr = lv_obj_create(s_card);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, 32);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(hdr);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, ICON_FONT, 0);
    lv_obj_set_style_text_color(icon, ACCENT_COLOR, 0);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "蓝牙");
    lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title, TEXT_COLOR, 0);
    lv_obj_set_flex_grow(title, 1);

    s_status = lv_label_create(hdr);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_font(s_status, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_status, TEXT_DIM_COLOR, 0);

    /* 设备列表（占满剩余高度，超出滚动） */
    s_list = lv_obj_create(s_card);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, ROW_GAP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* 底部：蓝牙设置入口 */
    lv_obj_t *btn = lv_button_create(s_card);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, FOOTER_H);
    lv_obj_set_style_bg_color(btn, FOOTER_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb_open_settings, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "蓝牙设置");
    lv_obj_set_style_text_font(btn_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(btn_lbl, TEXT_COLOR, 0);
    lv_obj_center(btn_lbl);

    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(timer_cb, 500, NULL);
    ESP_LOGI(TAG, "quick panel created");
}

void bt_quick_panel_show(void)
{
    ensure_created();
    if (s_root == NULL) {
        return;
    }
    s_visible = true;
    s_sig = panel_sig();
    rebuild();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    if (s_timer != NULL) {
        lv_timer_reset(s_timer);
    }
    ESP_LOGI(TAG, "quick panel shown");
}

void bt_quick_panel_hide(void)
{
    if (s_root == NULL) {
        return;
    }
    s_visible = false;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "quick panel hidden");
}

bool bt_quick_panel_is_visible(void)
{
    return s_visible;
}
