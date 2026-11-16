#include "ui/bt_settings.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/hardware/bt/bt_service.h"
#include "core/hardware/display.h"
#include "core/storage/bt_store.h"
#include "core/storage/config_store.h"
#include "core/ui/click_wheel.h"
#include "ui/eq_popup.h"
#include "ui/main_ui.h"
#include "ui/settings_menu.h"
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
#define TEXT_DANGER_COLOR lv_color_hex(0xFF5A5A)   /* 删除等危险操作 */

#define CARD_PAD_HOR  16
#define CARD_PAD_VER  14
#define CARD_GAP      12
#define PAGE_PAD      12
#define ICON_FONT     (&lv_font_montserrat_24)

/* 扫描结果行数上限：与 bt_service.c 的 BT_SCAN_MAX 一致（那边是内部宏） */
#define SCAN_ROWS_MAX  16

/* 刷新周期（ms）：轮询 bt_service/bt_store 状态，增量更新当前页 */
#define REFRESH_PERIOD_MS  500

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

/* 本页全屏不透明，挂在**屏幕层**（lv_scr_act），与设置菜单/主界面同一图层树。
   ⚠ 不能挂 lv_layer_top()：LVGL 的覆盖判定 lv_refr_get_top_obj() 只在当前
   屏幕的子对象树里查找"完全盖住绘制块的不透明对象"，顶层对象不在其中，
   于是每个绘制块都要把被盖住的下层整屏内容重画一遍，再无条件重画顶层——
   绘制量翻 2~3 倍，滚动掉到十几 fps（本项目 DONE_PLANS.md 第 38 点已踩过）。
   放到屏幕层后，本页的全屏不透明背景 s_bg 就成了覆盖对象，下层一律跳过。
   为彻底不让下层参与重绘，打开时还会隐藏设置菜单/主界面（见 overlay_enter）。 */

static lv_obj_t *s_bg = NULL;       /* 全屏黑色背景层（盖住状态栏） */
static lv_obj_t *s_menu = NULL;     /* lv_menu 面板（与设置菜单同款） */
static bool      s_visible = false;
/* 打开前设置菜单是否可见：从设置菜单进来 → 关闭时回设置菜单；
   从控制中心快速面板进来 → 关闭时回主界面 */
static bool      s_restore_menu = false;

static lv_obj_t *s_page_main = NULL;
static lv_obj_t *s_page_dev  = NULL;
static lv_obj_t *s_page_scan = NULL;

static int       s_dev_idx = -1;          /* 正在查看详情的设备槽位 */
static bool      s_delete_armed = false;  /* 删除设备的两段确认 */
static lv_timer_t *s_timer = NULL;

/* 三个页面的内容**只创建一次**，之后全部走 update_*() 增量刷新。
   ⚠ 不要再改回"lv_obj_clean + 重建"：重建会重置页面滚动位置（连接/断开、
   切换 EQ、点"删除设备"等任何状态变化都会把页面弹回顶部，2026-11 用户反馈），
   而这些变化本质上只是"改几个文字/颜色"，直接更新控件最省也最稳。 */

/* ---- 主视图 ---- */
static lv_obj_t *s_main_switch_status = NULL;  /* 蓝牙开关行的右侧状态 */
static lv_obj_t *s_main_count         = NULL;  /* "已保存设备  x / 5" 的 label */
static lv_obj_t *s_main_dev_row[BT_STORE_MAX_DEVICES];     /* 5 个设备行（空槽隐藏） */
static lv_obj_t *s_main_dev_name[BT_STORE_MAX_DEVICES];    /* 设备名 label */
static lv_obj_t *s_main_dev_status[BT_STORE_MAX_DEVICES];  /* 已连接/已保存 label */
static lv_obj_t *s_main_empty = NULL;          /* 空列表提示（整行） */
static lv_obj_t *s_main_codec = NULL;          /* 耳机不支持 SBC 的警告（整行） */

/* ---- 设备详情 ---- */
static lv_obj_t *s_dev_name           = NULL;  /* 顶部：设备名 */
static lv_obj_t *s_dev_connect_status = NULL;  /* 顶部行右侧：连接 / 断开（点整行切换） */
static lv_obj_t *s_dev_status         = NULL;  /* 状态：未连接 / 已连接 / 播放中 */
static lv_obj_t *s_dev_eq             = NULL;  /* EQ 名 */
static lv_obj_t *s_dev_quiet          = NULL;  /* 睡眠免打扰 开/关 */
static lv_obj_t *s_dev_delete_title   = NULL;  /* 行标题：删除设备 / 再次点击确认删除 */
static lv_obj_t *s_dev_delete_status  = NULL;  /* 行右侧：删除 */
static lv_obj_t *s_dev_mac            = NULL;  /* 页面最底部：MAC 地址 */

/* ---- 扫描视图 ---- */
static lv_obj_t *s_scan_status = NULL;         /* 顶部状态行（标题 label） */
static lv_obj_t *s_scan_rows[SCAN_ROWS_MAX];   /* 结果行（按需显示，全部预建） */
static lv_obj_t *s_scan_name[SCAN_ROWS_MAX];   /* 设备名 */
static lv_obj_t *s_scan_rssi[SCAN_ROWS_MAX];   /* RSSI */

/* 更新函数（页面切换/状态变化时调用；定义在页面创建之后） */
static void update_main(void);
static void update_device(void);
static void update_scan(void);

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

/* 信号强度档位：bt_service 返回的是标准 HCI Read RSSI 的 delta（相对
   golden receive power range，**不是 dBm**）：0 = 落在理想窗口内，
   负数 = 低于下限多少 dB。按用户要求（2026-11）：
     - 0 / 正数 / 未知 → 不显示（窗口内不夸大成"信号好"）
     - -12..-1        → 信号中
     - < -12          → 信号差
   阈值 RSSI_DELTA_FAIR 是真机可调的唯一常量。 */
#define RSSI_DELTA_FAIR  (-12)

static const char *rssi_level_text(int delta)
{
    if (delta == -128 || delta >= 0) {
        return "";              /* 未知 / 落在窗口内：不显示 */
    }
    if (delta >= RSSI_DELTA_FAIR) {
        return "信号中";
    }
    return "信号差";
}

/* 增量更新三件套：文字/颜色/显隐都先比较，值没变就不碰对象（不产生重绘）。
   这是"状态轮询但页面不闪"的关键——每 500ms 调一次也几乎零开销。 */
static void set_label_text(lv_obj_t *lbl, const char *text)
{
    if (lbl == NULL || text == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(lbl);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(lbl, text);
}

static void set_label_color(lv_obj_t *lbl, lv_color_t color)
{
    if (lbl == NULL) {
        return;
    }
    if (lv_color_eq(lv_obj_get_style_text_color(lbl, 0), color)) {
        return;
    }
    lv_obj_set_style_text_color(lbl, color, 0);
}

static void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
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

/* 创建一行卡片：图标 + 标题 + 可选右侧状态文字（同设置菜单 menu_row_create）。
   title_out / status_out 用于后续增量更新（可为 NULL）。 */
static lv_obj_t *row_create(lv_obj_t *page, const char *icon,
                            const char *title,
                            lv_obj_t **title_out, lv_obj_t **status_out)
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
    if (title_out != NULL) {
        *title_out = title_lbl;
    }

    if (status_out != NULL) {
        *status_out = lv_label_create(row);
        lv_label_set_text(*status_out, "");
        lv_obj_set_style_text_font(*status_out, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(*status_out, TEXT_DIM_COLOR, 0);
    }
    return row;
}

/* 灰色说明行（同设置菜单 menu_note_row_create，多段文本换行）。
   label_out 用于后续改文字（如"已保存设备 x / 5"），可为 NULL。 */
static lv_obj_t *note_row(lv_obj_t *page, const char *text, lv_obj_t **label_out)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style(row);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, TEXT_DIM_COLOR, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
    if (label_out != NULL) {
        *label_out = lbl;
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* 页面切换（lv_menu 历史维护）                                          */
/* ------------------------------------------------------------------ */

/* 回到主视图（本页的"根页面"）。
   ⚠ lv_menu_set_page() 对**同一页面没有去重**：每次调用都会往历史链里插一个
   节点并把 cur_depth++。本页的显示/重建路径多次切回主视图，若直接 set_page，
   cur_depth 会一路涨到 2、3…，于是：
     - lv_menu 认为 cur_depth>=2 → 返回键显示成"返回上一级"的行为；
     - lv_menu_back_button_is_root() 用 prev_depth(=点击前的 cur_depth)<=1 判定
       根页面 → 误判成"不是根"，点击时先被 lv_menu 自己的回调弹历史，
       我们的回调又因为不是根而不关闭本页 → 用户看到的就是"点返回键，
       返回按钮消失、页面还在"（2026-11 用户反馈）。
   所以切主视图统一走"清历史 + 重新设置"，保证主视图上 cur_depth 恒为 1
   （与设置菜单 settings_menu_show() 的做法一致）；子页面（设备详情/扫描）
   用普通 lv_menu_set_page，cur_depth=2，返回键由 lv_menu 退回主视图。 */
static void menu_goto_main(void)
{
    if (s_menu == NULL || s_page_main == NULL) {
        return;
    }
    if (lv_menu_get_cur_main_page(s_menu) == s_page_main) {
        return;   /* 已在主视图：历史里只有它，无需重复设置 */
    }
    lv_menu_clear_history(s_menu);        /* cur_depth 归零 */
    lv_menu_set_page(s_menu, s_page_main); /* cur_depth = 1，历史 = [主视图] */
}

/* ------------------------------------------------------------------ */
/* 事件回调                                                            */
/* ------------------------------------------------------------------ */

static void cb_back(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    /* 主视图上返回键 = 关闭本页（根返回按钮在 ensure_created 里启用）；
       子页面上 lv_menu 自己的回调已先退回主视图，这里不要再关闭整页 */
    if (lv_menu_back_button_is_root(s_menu, btn)) {
        bt_settings_hide();
    } else {
        /* 从子页面退回主视图：立即刷新一次（不等 500ms 定时器） */
        update_main();
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
    update_main();   /* 开关意图立即生效（电源状态异步跟上，由定时器刷新） */
}

/* 完整设置页里点击设备行 = 进入该设备的详情页（不在这里切连接状态；
   连接/断开放在详情页，避免误触） */
static void cb_device_row(lv_event_t *e)
{
    s_dev_idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_delete_armed = false;
    if (!bt_store_get(s_dev_idx, NULL)) {
        s_dev_idx = -1;
        menu_goto_main();   /* 设备刚被删除 */
        update_main();
        return;
    }
    lv_menu_set_page(s_menu, s_page_dev);
    /* 打开新设备：回到页面顶部（同一页面上更新内容时不动滚动位置） */
    lv_obj_scroll_to_y(s_page_dev, 0, LV_ANIM_OFF);
    update_device();
}

static void cb_pair(lv_event_t *e)
{
    (void)e;
    if (!bt_service_is_enabled()) {
        ui_toast_show("请先开启蓝牙");
        return;
    }
    lv_menu_set_page(s_menu, s_page_scan);
    lv_obj_scroll_to_y(s_page_scan, 0, LV_ANIM_OFF);
    /* 先清空旧结果、显示"正在搜索"（服务层的 scan_active/结果计数是异步
       置位的，等它反而会先闪一下"没有发现设备"） */
    set_label_text(s_scan_status, "正在搜索...（已发现 0 个）");
    for (int i = 0; i < SCAN_ROWS_MAX; i++) {
        set_obj_hidden(s_scan_rows[i], true);
    }
    bt_service_scan_start();
}

static void cb_rescan(lv_event_t *e)
{
    (void)e;
    set_label_text(s_scan_status, "正在搜索...（已发现 0 个）");
    for (int i = 0; i < SCAN_ROWS_MAX; i++) {
        set_obj_hidden(s_scan_rows[i], true);
    }
    bt_service_scan_start();
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
    menu_goto_main();   /* 设备表由服务层异步更新，主视图随后由定时器刷新 */
    update_main();
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
}

static void cb_dev_quiet(lv_event_t *e)
{
    (void)e;
    bt_device_t d;
    if (s_dev_idx < 0 || !bt_store_get(s_dev_idx, &d)) {
        return;
    }
    bt_store_set_quiet(s_dev_idx, !d.quiet_mode);
    ui_toast_show(d.quiet_mode ? "睡眠免打扰已关闭" : "睡眠免打扰已开启");
    ESP_LOGI(TAG, "device %d quiet_mode -> %d", s_dev_idx, (int)!d.quiet_mode);
    update_device();   /* 开关状态就地刷新（不重建页面、不跳滚动位置） */
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
        update_device();   /* 只改本行标题，页面不滚回顶部 */
        return;
    }
    bt_service_forget(s_dev_idx);
    ui_toast_show("设备已删除");
    s_delete_armed = false;
    s_dev_idx = -1;
    menu_goto_main();
    update_main();
}

/* ------------------------------------------------------------------ */
/* 页面创建（只创建一次）                                                */
/* ------------------------------------------------------------------ */

static void main_page_create(void)
{
    /* 蓝牙开关行 */
    lv_obj_t *row = row_create(s_page_main, LV_SYMBOL_BLUETOOTH, "蓝牙",
                               NULL, &s_main_switch_status);
    lv_obj_add_event_cb(row, cb_switch, LV_EVENT_CLICKED, NULL);

    /* 已保存设备计数（文字在 update_main 里刷新） */
    note_row(s_page_main, "", &s_main_count);

    /* 5 个设备行：全部预建，空槽隐藏。行下标 = 设备槽位下标，
       bt_store 删除设备时后项前移，这里无需重建，update_main 会把
       新的设备名/状态填进来。 */
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        s_main_dev_row[i] = row_create(s_page_main, LV_SYMBOL_BLUETOOTH, "",
                                       &s_main_dev_name[i], &s_main_dev_status[i]);
        lv_obj_add_event_cb(s_main_dev_row[i], cb_device_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(s_main_dev_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 空列表提示（仅 0 个设备时显示） */
    s_main_empty = note_row(s_page_main,
                            "还没有配对过设备。把耳机进入配对模式后点击下面一项。",
                            NULL);
    lv_obj_add_flag(s_main_empty, LV_OBJ_FLAG_HIDDEN);

    /* 配对新设备 */
    row = row_create(s_page_main, LV_SYMBOL_PLUS, "配对新设备", NULL, NULL);
    lv_obj_t *pair_status = lv_label_create(row);
    lv_label_set_text(pair_status, "搜索");
    lv_obj_set_style_text_font(pair_status, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(pair_status, TEXT_HL_COLOR, 0);
    lv_obj_add_event_cb(row, cb_pair, LV_EVENT_CLICKED, NULL);

    /* 耳机编码不支持时的警告（仅出错时显示） */
    s_main_codec = note_row(s_page_main,
                            "注意：该耳机不支持 SBC 48k/44.1k，无法播放。",
                            NULL);
    lv_obj_add_flag(s_main_codec, LV_OBJ_FLAG_HIDDEN);
}

static void device_page_create(void)
{
    /* 顶部：设备名 + 连接/断开。整行可点 = 连接或断开当前设备
       （原来单独一行"连接/断开"已合并到这里；MAC 移到页面最底部） */
    lv_obj_t *row = row_create(s_page_dev, LV_SYMBOL_BLUETOOTH, "",
                               &s_dev_name, &s_dev_connect_status);
    lv_obj_set_style_text_color(s_dev_connect_status, TEXT_HL_COLOR, 0);
    lv_obj_add_event_cb(row, cb_dev_connect, LV_EVENT_CLICKED, NULL);

    /* 状态：未连接 / 已连接 / 播放中 */
    row_create(s_page_dev, NULL, "状态", NULL, &s_dev_status);

    /* EQ：点击选 .eq（连接期间即时生效） */
    row = row_create(s_page_dev, NULL, "EQ", NULL, &s_dev_eq);
    lv_obj_add_event_cb(row, cb_dev_eq, LV_EVENT_CLICKED, NULL);

    /* 睡眠免打扰（UI 名；代码里仍叫 quiet_mode） */
    row = row_create(s_page_dev, NULL, "睡眠免打扰", NULL, &s_dev_quiet);
    lv_obj_add_event_cb(row, cb_dev_quiet, LV_EVENT_CLICKED, NULL);

    /* 删除设备（两段确认：第一次点击只改本行标题） */
    row = row_create(s_page_dev, NULL, "", &s_dev_delete_title,
                     &s_dev_delete_status);
    lv_label_set_text(s_dev_delete_status, "删除");
    lv_obj_set_style_text_color(s_dev_delete_status, TEXT_DANGER_COLOR, 0);
    lv_obj_add_event_cb(row, cb_dev_delete, LV_EVENT_CLICKED, NULL);

    /* 唯一一条说明：睡眠免打扰的行为（2026-11 按用户要求精简） */
    note_row(s_page_dev,
             "开启睡眠免打扰后，设备在睡眠定时器结束后不会断电，"
             "而是进入低功耗模式并保持蓝牙连接。"
             "防止耳机出现断连提示音或灯光闪烁。",
             NULL);

    /* MAC 地址放最底部（2026-11 用户要求：从顶部移到这里） */
    row_create(s_page_dev, NULL, "MAC 地址", NULL, &s_dev_mac);
}

static void scan_page_create(void)
{
    /* 顶部状态行（文字在 update_scan 里刷新） */
    row_create(s_page_scan, NULL, "", &s_scan_status, NULL);

    /* 结果行：最多 SCAN_ROWS_MAX 个，全部预建（空位隐藏）。
       行下标 = 扫描结果下标，结果数量变化时只切换显隐，不重建页面。 */
    for (int i = 0; i < SCAN_ROWS_MAX; i++) {
        s_scan_rows[i] = row_create(s_page_scan, LV_SYMBOL_BLUETOOTH, "",
                                    &s_scan_name[i], &s_scan_rssi[i]);
        lv_obj_add_event_cb(s_scan_rows[i], cb_scan_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_flag(s_scan_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 重新搜索 */
    lv_obj_t *row = row_create(s_page_scan, LV_SYMBOL_REFRESH, "重新搜索",
                               NULL, NULL);
    lv_obj_add_event_cb(row, cb_rescan, LV_EVENT_CLICKED, NULL);

    /* 配对操作提示（首次配对的步骤，属操作引导而非设置说明） */
    note_row(s_page_scan,
             "首次配对请先让耳机进入配对模式（通常长按电源键直到指示灯快闪）。"
             "配对成功后会自动连接；最多保存 5 个设备。",
             NULL);
}

/* ------------------------------------------------------------------ */
/* 增量刷新（只改文字/颜色/显隐，永远不重建页面）                        */
/* ------------------------------------------------------------------ */

static void update_main(void)
{
    if (s_page_main == NULL) {
        return;
    }

    /* 蓝牙开关状态 */
    set_label_text(s_main_switch_status, main_status_text());
    set_label_color(s_main_switch_status,
                    bt_service_is_connected() ? TEXT_HL_COLOR : TEXT_DIM_COLOR);

    /* 设备计数 */
    char buf[48];
    snprintf(buf, sizeof(buf), "已保存设备  %d / %d", bt_store_count(),
             BT_STORE_MAX_DEVICES);
    set_label_text(s_main_count, buf);

    /* 5 个设备行：空槽隐藏，非空槽填名称 + 状态 */
    int shown = 0;
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        bt_device_t d;
        bool valid = bt_store_get(i, &d);
        set_obj_hidden(s_main_dev_row[i], !valid);
        if (!valid) {
            continue;
        }
        shown++;
        set_label_text(s_main_dev_name[i], d.name);
        bool conn = (bt_service_connected_index() == i);
        set_label_text(s_main_dev_status[i],
                       conn ? (bt_service_is_streaming() ? "播放中" : "已连接")
                            : "已保存");
        set_label_color(s_main_dev_status[i],
                        conn ? TEXT_HL_COLOR : TEXT_DIM_COLOR);
    }
    set_obj_hidden(s_main_empty, shown != 0);
    set_obj_hidden(s_main_codec, !bt_service_codec_unsupported());
}

static void update_device(void)
{
    if (s_page_dev == NULL || s_dev_idx < 0) {
        return;
    }
    bt_device_t d;
    if (!bt_store_get(s_dev_idx, &d)) {
        /* 设备已不存在（被删除）：回到主视图 */
        s_dev_idx = -1;
        menu_goto_main();
        update_main();
        return;
    }

    bool connected = (bt_service_connected_index() == s_dev_idx);

    set_label_text(s_dev_name, d.name);
    set_label_text(s_dev_connect_status, connected ? "断开" : "连接");

    /* 状态：未连接 / 已连接 / 播放中；已连接时附一个简易信号档位
       （只有信号变差才显示"信号中/差"，来自后台轮询的 HCI Read RSSI delta） */
    char st[48];
    const char *base = connected
                           ? (bt_service_is_streaming() ? "播放中" : "已连接")
                           : "未连接";
    const char *sig = connected
                          ? rssi_level_text(bt_service_connected_rssi_delta())
                          : "";
    if (sig[0] != '\0') {
        snprintf(st, sizeof(st), "%s  %s", base, sig);
    } else {
        snprintf(st, sizeof(st), "%s", base);
    }
    set_label_text(s_dev_status, st);
    set_label_color(s_dev_status, connected ? TEXT_HL_COLOR : TEXT_DIM_COLOR);

    set_label_text(s_dev_eq, (d.eq_valid && d.eq_name[0]) ? d.eq_name : "无 EQ");
    set_label_color(s_dev_eq, d.eq_valid ? TEXT_HL_COLOR : TEXT_DIM_COLOR);

    set_label_text(s_dev_quiet, d.quiet_mode ? "开" : "关");
    set_label_color(s_dev_quiet, d.quiet_mode ? TEXT_HL_COLOR : TEXT_DIM_COLOR);

    set_label_text(s_dev_delete_title,
                   s_delete_armed ? "再次点击确认删除" : "删除设备");

    char mac[24];
    bda_to_str(d.bda, mac, sizeof(mac));
    set_label_text(s_dev_mac, mac);
}

static void update_scan(void)
{
    if (s_page_scan == NULL) {
        return;
    }

    int n = bt_service_scan_count();
    char buf[64];
    if (bt_service_scan_active()) {
        snprintf(buf, sizeof(buf), "正在搜索...（已发现 %d 个）", n);
    } else if (n == 0) {
        snprintf(buf, sizeof(buf), "没有发现设备，请让耳机进入配对模式后重试");
    } else {
        snprintf(buf, sizeof(buf), "搜索完成，共 %d 个设备", n);
    }
    set_label_text(s_scan_status, buf);

    for (int i = 0; i < SCAN_ROWS_MAX; i++) {
        bool show = (i < n);
        set_obj_hidden(s_scan_rows[i], !show);
        if (!show) {
            continue;
        }
        bt_scan_result_t r;
        if (!bt_service_scan_get(i, &r)) {
            set_obj_hidden(s_scan_rows[i], true);
            continue;
        }
        set_label_text(s_scan_name[i], r.name[0] ? r.name : "未知设备");
        if (r.rssi > -128) {
            char rssi[16];
            snprintf(rssi, sizeof(rssi), "%d dBm", r.rssi);
            set_label_text(s_scan_rssi[i], rssi);
        } else {
            set_label_text(s_scan_rssi[i], "");
        }
    }
}

/* 状态轮询：每 500ms 只更新**当前页**的控件（值没变的 set_* 会直接返回，
   不产生任何重绘；也不会动滚动位置） */
static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_visible || s_menu == NULL) {
        return;
    }
    lv_obj_t *cur = lv_menu_get_cur_main_page(s_menu);
    if (cur == s_page_dev) {
        update_device();
    } else if (cur == s_page_scan) {
        update_scan();
    } else {
        update_main();
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
    /* 父对象 = 当前屏幕（屏幕层），不是 lv_layer_top()：原因见文件上方
       "状态"处的说明（顶层会破坏 LVGL 的覆盖判定 → 下层整屏每帧重绘） */
    lv_obj_t *scr = lv_scr_act();

    /* 全屏黑色背景层（盖住状态栏；边界点击 = 关闭） */
    s_bg = lv_obj_create(scr);
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

    /* 三个页面（内容只创建一次，之后增量刷新） */
    s_page_main = lv_menu_page_create(s_menu, (char *)"蓝牙");
    page_style(s_page_main);
    s_page_dev = lv_menu_page_create(s_menu, (char *)"设备设置");
    page_style(s_page_dev);
    s_page_scan = lv_menu_page_create(s_menu, (char *)"配对新设备");
    page_style(s_page_scan);

    main_page_create();
    device_page_create();
    scan_page_create();
    update_main();

    /* 主视图（根页面）也显示返回按钮，点了 = 关闭本页（与设置菜单一致）。
       ⚠ 不启用时 lv_menu 只在 cur_depth>=2 才显示返回键，主视图上没有
       "关闭"入口（只能点面板外的黑边） */
    lv_menu_set_mode_root_back_button(s_menu, LV_MENU_ROOT_BACK_BUTTON_ENABLED);

    lv_menu_set_page(s_menu, s_page_main);   /* cur_depth = 1 */

    s_timer = lv_timer_create(refresh_timer_cb, REFRESH_PERIOD_MS, NULL);
    ESP_LOGI(TAG, "bluetooth settings page created (incremental update)");
}

/* ------------------------------------------------------------------ */
/* 显示 / 隐藏：本身是全屏不透明覆盖层，打开时让下层整体让位              */
/* ------------------------------------------------------------------ */

/* 取覆盖层前的准备：本页要盖住设置菜单/主界面，且不希望它们在下方参与
   重绘（虽然本页不透明背景已能让覆盖判定命中，但隐藏是最彻底的：
   被隐藏的对象连"是否可见"都不必算，也不会被任何定时器无效化刷到）。
   ⚠ 必须在 LVGL 任务/持锁上下文调用（本模块所有入口都满足）。 */
static void overlay_enter(void)
{
    s_restore_menu = settings_menu_is_visible();
    if (s_restore_menu) {
        /* 从设置菜单进来：先把菜单收起来（内部会恢复主界面与 ClickWheel），
           下面再统一隐藏主界面，避免主界面在两个调用之间闪一帧 */
        settings_menu_hide();
    }
    main_ui_set_visible(false);
    click_wheel_set_overlay(true);
}

/* 关闭后恢复被让位的下层：从设置菜单进来 → 回设置菜单；否则回主界面 */
static void overlay_leave(void)
{
    if (s_restore_menu) {
        s_restore_menu = false;
        /* settings_menu_show() 内部会隐藏主界面并收起 ClickWheel，
           同时把菜单重置到根页面（蓝牙入口就在根页面） */
        settings_menu_show();
        return;
    }
    main_ui_set_visible(true);
    click_wheel_set_overlay(false);
}

void bt_settings_show(void)
{
    ensure_created();
    if (s_bg == NULL) {
        return;
    }
    if (!s_visible) {
        overlay_enter();
    }
    s_visible = true;
    s_dev_idx = -1;
    s_delete_armed = false;
    update_main();
    menu_goto_main();   /* 每次打开都回到主视图（并压平浏览历史） */
    lv_obj_scroll_to_y(s_page_main, 0, LV_ANIM_OFF);
    /* 屏幕层可能还有更晚创建的兄弟对象（如懒加载的设置菜单外壳）：
       每次显示都置顶，保证本页在最上层 */
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bg);
    if (s_timer != NULL) {
        lv_timer_reset(s_timer);
    }
    ESP_LOGI(TAG, "bt settings shown (screen layer, restore_menu=%d)",
             (int)s_restore_menu);
}

void bt_settings_hide(void)
{
    if (s_bg == NULL || !s_visible) {
        return;
    }
    s_visible = false;
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_HIDDEN);
    bool back_to_menu = s_restore_menu;
    overlay_leave();
    ESP_LOGI(TAG, "bt settings hidden (back to %s)", back_to_menu ? "settings menu"
                                                                 : "main UI");
}

bool bt_settings_is_visible(void)
{
    return s_visible;
}
