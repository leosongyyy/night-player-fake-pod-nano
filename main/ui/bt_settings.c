#include "ui/bt_settings.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/bt/bt_service.h"
#include "core/hardware/display.h"
#include "core/storage/bt_store.h"
#include "core/storage/config_store.h"
#include "ui/eq_popup.h"
#include "ui/toast.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "bt_settings";

/* ------------------------------------------------------------------ */
/* 布局与配色：与 ui/settings_menu.c 完全同规格                          */
/* ------------------------------------------------------------------ */

/* 菜单面板四周留白（与设置菜单一致：左右 37 / 上下 14，外围纯黑） */
#define MENU_MARGIN_VER  14
#define MENU_MARGIN_HOR  37
#define MENU_W        (EXAMPLE_LCD_H_RES - MENU_MARGIN_HOR * 2)
#define MENU_H        (EXAMPLE_LCD_V_RES - MENU_MARGIN_VER * 2)

#define MENU_BG_COLOR     lv_color_hex(0x000000)   /* 页面背景：纯黑 */
#define CARD_BG_COLOR     lv_color_hex(0x1E1E1E)   /* 行卡片：深灰 */
#define CARD_PRESS_COLOR  lv_color_hex(0xFFC000)   /* 按下高亮：黄 */
#define TEXT_COLOR        lv_color_white()
#define TEXT_DIM_COLOR    lv_palette_main(LV_PALETTE_GREY)
#define TEXT_HL_COLOR     lv_palette_main(LV_PALETTE_YELLOW)

#define CARD_PAD_HOR  16
#define CARD_PAD_VER  14
#define CARD_GAP      12
#define PAGE_PAD      12
#define ICON_FONT     (&lv_font_montserrat_24)

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_bg = NULL;       /* 全屏黑色背景层（盖住状态栏） */
static lv_obj_t *s_menu = NULL;     /* lv_menu 面板（与设置菜单同款） */
static bool      s_visible = false;

static lv_obj_t *s_page_main = NULL;
static lv_obj_t *s_page_dev  = NULL;
static lv_obj_t *s_page_scan = NULL;

static int       s_dev_idx = -1;
static bool      s_delete_armed = false;
static uint32_t  s_hash = 0;
static lv_timer_t *s_timer = NULL;

/* 页面内容构建（定义在后面，回调里会先重建再切页） */
static void build_main(void);
static void build_device(void);
static void build_scan(void);

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void bda_to_str(const uint8_t bda[6], char *buf, size_t size)
{
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static const char *power_text(void)
{
    switch (bt_service_power_state()) {
    case BT_SVC_POWER_STARTING: return "启动中";
    case BT_SVC_POWER_ON:       return "已开启";
    case BT_SVC_POWER_STOPPING: return "关闭中";
    default:                    return "已关闭";
    }
}

static const char *main_status_text(void)
{
    if (!bt_service_is_enabled()) {
        return "关";
    }
    if (bt_service_is_connected()) {
        return bt_service_is_streaming() ? "播放中" : "已连接";
    }
    if (bt_service_power_state() != BT_SVC_POWER_ON) {
        return power_text();
    }
    return "开";
}

/* 行卡片：深灰圆角底 + 按下黄色高亮（同设置菜单 card_style_apply） */
static void card_style(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, CARD_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_right(card, CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_top(card, CARD_PAD_VER, 0);
    lv_obj_set_style_pad_bottom(card, CARD_PAD_VER, 0);
    lv_obj_set_style_bg_color(card, CARD_PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
}

/* 页面通用样式：黑底 + 留白 + 卡片间距（同设置菜单 page_style_apply） */
static void page_style(lv_obj_t *page)
{
    lv_obj_set_style_bg_color(page, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, PAGE_PAD, 0);
    lv_obj_set_style_pad_row(page, CARD_GAP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
}

/* 创建一行卡片：图标 + 标题 + 可选右侧状态文字（同设置菜单 menu_row_create） */
static lv_obj_t *row_create(lv_obj_t *page, const char *icon,
                            const char *title, lv_obj_t **status_out)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style(row);
    /* lv_menu_cont 默认不可点击：与设置菜单一致，行需要手动加 CLICKABLE */
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (icon != NULL) {
        lv_obj_t *icon_lbl = lv_label_create(row);
        lv_label_set_text(icon_lbl, icon);
        lv_obj_set_style_text_font(icon_lbl, ICON_FONT, 0);
        lv_obj_set_style_text_color(icon_lbl, TEXT_DIM_COLOR, 0);
    }

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title_lbl, TEXT_COLOR, 0);
    lv_obj_set_flex_grow(title_lbl, 1);

    if (status_out != NULL) {
        *status_out = lv_label_create(row);
        lv_label_set_text(*status_out, "");
        lv_obj_set_style_text_font(*status_out, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(*status_out, TEXT_DIM_COLOR, 0);
    }
    return row;
}

/* 灰色说明行（同设置菜单 menu_note_row_create，多段文本换行） */
static void note_row(lv_obj_t *page, const char *text)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style(row);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, TEXT_DIM_COLOR, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
}

/* ------------------------------------------------------------------ */
/* 事件回调                                                            */
/* ------------------------------------------------------------------ */

static void cb_back(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (lv_menu_back_button_is_root(s_menu, btn)) {
        bt_settings_hide();
    }
}

static void cb_edge_click(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_bg) {
        bt_settings_hide();   /* 点击菜单面板外的黑色区域 = 关闭 */
    }
}

static void cb_switch(lv_event_t *e)
{
    (void)e;
    bool on = !bt_service_is_enabled();
    bt_service_set_enabled(on);
    ui_toast_show(on ? "蓝牙已开启" : "蓝牙已关闭");
    s_hash = 0;
}

/* 完整设置页里点击设备行 = 进入该设备的详情页（不在这里切连接状态；
   连接/断开放在详情页，避免误触） */
static void cb_device_row(lv_event_t *e)
{
    s_dev_idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_delete_armed = false;
    s_hash = 0;
    if (!bt_store_get(s_dev_idx, NULL)) {
        lv_menu_set_page(s_menu, s_page_main);   /* 设备刚被删除 */
        return;
    }
    build_device();              /* 先按该设备重建内容，再切页 */
    lv_menu_set_page(s_menu, s_page_dev);
}

static void cb_pair(lv_event_t *e)
{
    (void)e;
    if (!bt_service_is_enabled()) {
        ui_toast_show("请先开启蓝牙");
        return;
    }
    s_hash = 0;
    if (s_page_scan != NULL) {
        build_scan();            /* 先清空旧扫描结果再切页 */
        lv_menu_set_page(s_menu, s_page_scan);
    }
    bt_service_scan_start();
}

static void cb_rescan(lv_event_t *e)
{
    (void)e;
    bt_service_scan_start();
    s_hash = 0;
}

static void cb_scan_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bt_scan_result_t r;
    if (!bt_service_scan_get(i, &r)) {
        return;
    }
    if (bt_store_count() >= BT_STORE_MAX_DEVICES && bt_store_find(r.bda) < 0) {
        ui_toast_show("最多保存 5 个设备，请先删除一个");
        return;
    }
    bt_service_scan_connect(i);
    ui_toast_show("正在配对/连接...");
    s_hash = 0;
    lv_menu_set_page(s_menu, s_page_main);
}

static void cb_dev_connect(lv_event_t *e)
{
    (void)e;
    if (s_dev_idx < 0) {
        return;
    }
    if (bt_service_connected_index() == s_dev_idx) {
        bt_service_disconnect();
        ui_toast_show("正在断开...");
    } else {
        /* 连接另一台设备：服务层先断开当前设备再连目标；失败不回退 */
        bool switching = (bt_service_connected_index() >= 0);
        bt_service_connect_index(s_dev_idx);
        ui_toast_show(switching ? "正在切换设备..." : "正在连接...");
    }
    s_hash = 0;
}

static void cb_dev_quiet(lv_event_t *e)
{
    (void)e;
    bt_device_t d;
    if (!bt_store_get(s_dev_idx, &d)) {
        return;
    }
    bt_store_set_quiet(s_dev_idx, !d.quiet_mode);
    ui_toast_show(d.quiet_mode ? "安静模式已关闭" : "安静模式已开启");
    ESP_LOGI(TAG, "device %d quiet_mode -> %d", s_dev_idx, (int)!d.quiet_mode);
    s_hash = 0;
}

static void cb_dev_eq(lv_event_t *e)
{
    (void)e;
    if (s_dev_idx >= 0 && s_bg != NULL) {
        eq_popup_toggle_for_device(s_bg, s_dev_idx);
    }
}

static void cb_dev_delete(lv_event_t *e)
{
    (void)e;
    if (s_dev_idx < 0) {
        return;
    }
    if (!s_delete_armed) {
        s_delete_armed = true;
        ui_toast_show("再次点击确认删除");
        s_hash = 0;
        return;
    }
    bt_service_forget(s_dev_idx);
    ui_toast_show("设备已删除");
    s_delete_armed = false;
    s_dev_idx = -1;
    s_hash = 0;
    lv_menu_set_page(s_menu, s_page_main);
}

/* ------------------------------------------------------------------ */
/* 页面内容构建（每次状态变化时清空重建当前页）                          */
/* ------------------------------------------------------------------ */

static void build_main(void)
{
    lv_obj_clean(s_page_main);

    lv_obj_t *st = NULL;
    lv_obj_t *row = row_create(s_page_main, LV_SYMBOL_BLUETOOTH, "蓝牙", &st);
    lv_label_set_text(st, main_status_text());
    lv_obj_set_style_text_color(st, bt_service_is_connected() ? TEXT_HL_COLOR
                                                              : TEXT_DIM_COLOR, 0);
    lv_obj_add_event_cb(row, cb_switch, LV_EVENT_CLICKED, NULL);

    char buf[48];
    snprintf(buf, sizeof(buf), "已保存设备  %d / %d", bt_store_count(),
             BT_STORE_MAX_DEVICES);
    note_row(s_page_main, buf);

    int shown = 0;
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        bt_device_t d;
        if (!bt_store_get(i, &d)) {
            continue;
        }
        shown++;
        bool conn = (bt_service_connected_index() == i);
        row = row_create(s_page_main, LV_SYMBOL_BLUETOOTH, d.name, &st);
        lv_label_set_text(st, conn ? (bt_service_is_streaming() ? "播放中" : "已连接")
                                   : "已保存");
        lv_obj_set_style_text_color(st, conn ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);
        lv_obj_add_event_cb(row, cb_device_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    if (shown == 0) {
        note_row(s_page_main, "还没有配对过设备。把耳机进入配对模式后点击下面一项。");
    }

    row = row_create(s_page_main, LV_SYMBOL_PLUS, "配对新设备", &st);
    lv_label_set_text(st, "搜索");
    lv_obj_set_style_text_color(st, TEXT_HL_COLOR, 0);
    lv_obj_add_event_cb(row, cb_pair, LV_EVENT_CLICKED, NULL);

    note_row(s_page_main, "点击设备进入详情页（连接/断开、EQ、安静模式、删除设备）。");
    note_row(s_page_main,
             "连接后音量与 EQ 使用该设备的记忆值（优先于全局配置）；"
             "断开后自动切回配置槽位的音量/EQ。");
    if (bt_service_codec_unsupported()) {
        note_row(s_page_main, "注意：该耳机不支持 SBC 48k/44.1k，无法播放。");
    }
}

static void build_device(void)
{
    bt_device_t d;
    if (!bt_store_get(s_dev_idx, &d)) {
        lv_menu_set_page(s_menu, s_page_main);
        return;
    }
    lv_obj_clean(s_page_dev);

    char mac[24];
    bda_to_str(d.bda, mac, sizeof(mac));
    lv_obj_t *st = NULL;
    row_create(s_page_dev, LV_SYMBOL_BLUETOOTH, d.name, &st);
    lv_label_set_text(st, mac);

    bool connected = (bt_service_connected_index() == s_dev_idx);
    lv_obj_t *row = row_create(s_page_dev, NULL, "状态", &st);
    lv_label_set_text(st, connected ? (bt_service_is_streaming() ? "播放中" : "已连接")
                                    : "未连接");
    lv_obj_set_style_text_color(st, connected ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);

    char vol_buf[32];
    uint8_t vol = connected ? audio_get_volume_level() : d.volume;
    snprintf(vol_buf, sizeof(vol_buf), "%u / %u", (unsigned)vol,
             (unsigned)AUDIO_VOL_LEVEL_MAX);
    row = row_create(s_page_dev, NULL, "音量", &st);
    lv_label_set_text(st, vol_buf);
    lv_obj_set_style_text_color(st, TEXT_COLOR, 0);

    row = row_create(s_page_dev, NULL, "EQ", &st);
    lv_label_set_text(st, (d.eq_valid && d.eq_name[0]) ? d.eq_name : "无 EQ");
    lv_obj_set_style_text_color(st, d.eq_valid ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);
    lv_obj_add_event_cb(row, cb_dev_eq, LV_EVENT_CLICKED, NULL);

    row = row_create(s_page_dev, NULL, "安静模式", &st);
    lv_label_set_text(st, d.quiet_mode ? "开" : "关");
    lv_obj_set_style_text_color(st, d.quiet_mode ? TEXT_HL_COLOR : TEXT_DIM_COLOR, 0);
    lv_obj_add_event_cb(row, cb_dev_quiet, LV_EVENT_CLICKED, NULL);

    row = row_create(s_page_dev, NULL, connected ? "断开连接" : "连接", &st);
    lv_label_set_text(st, connected ? "断开" : "连接");
    lv_obj_set_style_text_color(st, TEXT_HL_COLOR, 0);
    lv_obj_add_event_cb(row, cb_dev_connect, LV_EVENT_CLICKED, NULL);

    row = row_create(s_page_dev, NULL,
                     s_delete_armed ? "再次点击确认删除" : "删除设备", &st);
    lv_label_set_text(st, "删除");
    lv_obj_set_style_text_color(st, lv_color_hex(0xFF5A5A), 0);
    lv_obj_add_event_cb(row, cb_dev_delete, LV_EVENT_CLICKED, NULL);

    note_row(s_page_dev, "音量：用音量键或控制中心调节，记忆到本设备。");
    note_row(s_page_dev, "EQ：连接期间使用该设备的 EQ，优先于全局配置。");
    note_row(s_page_dev,
             "安静模式：睡眠定时到点后停播、关屏、关外设，但保留蓝牙连接；"
             "超过 12 小时、电量低于 15% 或蓝牙断开时自动断电。"
             "长按电源键可重启恢复。");
}

static void build_scan(void)
{
    lv_obj_clean(s_page_scan);

    int n = bt_service_scan_count();
    char buf[64];
    if (bt_service_scan_active()) {
        snprintf(buf, sizeof(buf), "正在搜索...（已发现 %d 个）", n);
    } else if (n == 0) {
        snprintf(buf, sizeof(buf), "没有发现设备，请让耳机进入配对模式后重试");
    } else {
        snprintf(buf, sizeof(buf), "搜索完成，共 %d 个设备", n);
    }
    lv_obj_t *st = NULL;
    row_create(s_page_scan, NULL, buf, NULL);

    for (int i = 0; i < n; i++) {
        bt_scan_result_t r;
        if (!bt_service_scan_get(i, &r)) {
            continue;
        }
        lv_obj_t *row = row_create(s_page_scan, LV_SYMBOL_BLUETOOTH,
                                   r.name[0] ? r.name : "未知设备", &st);
        if (r.rssi > -128) {
            snprintf(buf, sizeof(buf), "%d dBm", r.rssi);
            lv_label_set_text(st, buf);
        } else {
            lv_label_set_text(st, "");
        }
        lv_obj_set_style_text_color(st, TEXT_DIM_COLOR, 0);
        lv_obj_add_event_cb(row, cb_scan_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    lv_obj_t *row = row_create(s_page_scan, LV_SYMBOL_REFRESH, "重新搜索", &st);
    lv_label_set_text(st, "");
    lv_obj_add_event_cb(row, cb_rescan, LV_EVENT_CLICKED, NULL);

    note_row(s_page_scan,
             "首次配对请先让耳机进入配对模式（通常长按电源键直到指示灯快闪）。"
             "配对成功后会自动连接；最多保存 5 个设备。");
}

/* 状态指纹：任何可见信息变化就重建当前页（不比较控件，简单可靠） */
static uint32_t state_hash(void)
{
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX((int)bt_service_power_state());
    MIX(bt_service_is_enabled() ? 1 : 2);
    MIX(bt_service_connected_index());
    MIX(bt_service_is_streaming() ? 3 : 4);
    MIX(bt_store_count());
    MIX(audio_get_volume_level());
    MIX(bt_service_scan_active() ? 5 : 6);
    MIX(bt_service_scan_count());
    MIX(s_delete_armed ? 7 : 8);
    MIX(s_dev_idx);
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        bt_device_t d;
        if (bt_store_get(i, &d)) {
            MIX(d.bda[5]);
            MIX(d.quiet_mode ? 9 : 10);
            MIX(d.eq_valid ? (uint32_t)d.eq.band_count : 11);
            for (const char *p = d.name; *p; p++) {
                MIX((uint8_t)*p);
            }
        }
    }
    if (bt_service_is_connected()) {
        for (const char *p = bt_service_connected_name(); *p; p++) {
            MIX((uint8_t)*p);
        }
    }
    MIX(bt_service_codec_unsupported() ? 12 : 13);
#undef MIX
    return h;
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_visible || s_menu == NULL) {
        return;
    }
    uint32_t h = state_hash();
    if (h == s_hash) {
        return;
    }
    s_hash = h;
    lv_obj_t *cur = lv_menu_get_cur_main_page(s_menu);
    if (cur == s_page_dev) {
        build_device();
    } else if (cur == s_page_scan) {
        build_scan();
    } else {
        build_main();
    }
}

/* ------------------------------------------------------------------ */
/* 创建 / 显示                                                         */
/* ------------------------------------------------------------------ */

/* header 暗色化（同设置菜单 header_style_apply：遍历子对象） */
static void header_style(void)
{
    lv_obj_t *header = lv_menu_get_main_header(s_menu);
    if (header == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(header, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    uint32_t cnt = lv_obj_get_child_cnt(header);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(header, i);
        if (lv_obj_check_type(child, &lv_button_class)) {
            lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(child, 0, 0);
            lv_obj_set_style_pad_all(child, 8, 0);
            uint32_t icnt = lv_obj_get_child_cnt(child);
            for (uint32_t j = 0; j < icnt; j++) {
                lv_obj_t *icon = lv_obj_get_child(child, j);
                if (lv_obj_check_type(icon, &lv_image_class)) {
                    lv_obj_set_style_image_recolor(icon, TEXT_COLOR, 0);
                    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
                }
            }
        } else if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_font(child, UI_SYSTEM_FONT, 0);
            lv_obj_set_style_text_color(child, TEXT_COLOR, 0);
        }
    }
}

static void ensure_created(void)
{
    if (s_bg != NULL) {
        return;
    }
    lv_obj_t *top = lv_layer_top();

    /* 全屏黑色背景层（盖住状态栏；边界点击 = 关闭） */
    s_bg = lv_obj_create(top);
    lv_obj_set_size(s_bg, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_set_style_bg_color(s_bg, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bg, 0, 0);
    lv_obj_set_style_radius(s_bg, 0, 0);
    lv_obj_set_style_pad_all(s_bg, 0, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_bg, cb_edge_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);

    /* 菜单面板：与设置菜单同尺寸/位置 */
    s_menu = lv_menu_create(s_bg);
    lv_obj_set_size(s_menu, MENU_W, MENU_H);
    lv_obj_set_pos(s_menu, MENU_MARGIN_HOR, MENU_MARGIN_VER);
    lv_obj_set_style_bg_color(s_menu, MENU_BG_COLOR, 0);
    lv_obj_set_style_border_width(s_menu, 0, 0);
    lv_obj_set_style_radius(s_menu, 16, 0);
    header_style();

    lv_obj_t *back = lv_menu_get_main_header_back_button(s_menu);
    lv_obj_add_event_cb(back, cb_back, LV_EVENT_CLICKED, NULL);

    /* 三个页面（内容在每次打开/状态变化时重建） */
    s_page_main = lv_menu_page_create(s_menu, (char *)"蓝牙");
    page_style(s_page_main);
    s_page_dev = lv_menu_page_create(s_menu, (char *)"设备设置");
    page_style(s_page_dev);
    s_page_scan = lv_menu_page_create(s_menu, (char *)"配对新设备");
    page_style(s_page_scan);

    lv_menu_set_page(s_menu, s_page_main);

    s_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    ESP_LOGI(TAG, "bluetooth settings page created");
}

void bt_settings_show(void)
{
    ensure_created();
    if (s_bg == NULL) {
        return;
    }
    s_visible = true;
    s_dev_idx = -1;
    s_delete_armed = false;
    s_hash = 0;
    build_main();
    lv_menu_set_page(s_menu, s_page_main);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bg);
    if (s_timer != NULL) {
        lv_timer_reset(s_timer);
    }
    ESP_LOGI(TAG, "bt settings shown");
}

void bt_settings_hide(void)
{
    if (s_bg == NULL) {
        return;
    }
    s_visible = false;
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "bt settings hidden");
}

bool bt_settings_is_visible(void)
{
    return s_visible;
}
