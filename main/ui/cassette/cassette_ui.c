#include "ui/cassette/cassette_ui.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/display.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/battery_indicator.h"
#include "ui/clock_display.h"
#include "ui/cassette/cassette_drawer.h"
#include "ui/cassette/cassette_nav.h"
#include "ui/cassette/cassette_state.h"
#include "ui/cassette/cassette_theme.h"

static const char *TAG = "cas_ui";

#define CAS_REFRESH_MS        5000u   /* 电池/蓝牙状态刷新（电池 I2C ~1ms） */
#define CAS_SCREW_SIZE        CAS_CQW(3.6)
#define CAS_FRAME_INSET_1     CAS_CQW(0.5)
#define CAS_FRAME_INSET_2     CAS_CQW(1.6)

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_shell = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_clock_label = NULL;
static lv_obj_t *s_batt_label = NULL;
static lv_obj_t *s_bt_label = NULL;

/* ------------------------- 机箱拟物绘制 ------------------------- */

/**
 * 四角十字螺丝：两层同心圆近似金属车削面 + 两根旋转细条做一字槽。
 * LVGL 9.4 把 v9 早期的 transform_angle 改名为 transform_rotation
 * （单位仍是 0.1°），这里按实装头文件调用。
 */
static void cas_shell_screw_create(lv_obj_t *parent, int x, int y, int deg10)
{
    lv_obj_t *screw = lv_obj_create(parent);
    lv_obj_set_size(screw, CAS_SCREW_SIZE, CAS_SCREW_SIZE);
    lv_obj_set_pos(screw, x, y);
    lv_obj_set_style_radius(screw, 999, 0);
    lv_obj_set_style_bg_color(screw, CAS_COL_METAL_HI, 0);
    lv_obj_set_style_bg_opa(screw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screw, 1, 0);
    lv_obj_set_style_border_color(screw, CAS_COL_GROOVE, 0);
    lv_obj_set_style_pad_all(screw, 0, 0);
    lv_obj_clear_flag(screw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(screw, LV_OBJ_FLAG_CLICKABLE);

    /* 内圈暗环：模拟螺丝头的凹槽深度 */
    lv_obj_t *inner = lv_obj_create(screw);
    lv_obj_set_size(inner, CAS_SCREW_SIZE * 2 / 3, CAS_SCREW_SIZE * 2 / 3);
    lv_obj_center(inner);
    lv_obj_set_style_radius(inner, 999, 0);
    lv_obj_set_style_bg_color(inner, CAS_COL_METAL, 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *slot = lv_obj_create(inner);
        lv_obj_set_size(slot, CAS_SCREW_SIZE / 2, 2);
        lv_obj_center(slot);
        lv_obj_set_style_bg_color(slot, CAS_COL_GROOVE, 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(slot, 0, 0);
        lv_obj_set_style_radius(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_set_style_transform_rotation(slot, deg10 + i * 900, 0); /* +90° */
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    }
}

/** 双层金属内框：两层透明矩形只画描边，底下的机箱渐变照常透出 */
static void cas_chassis_create(lv_obj_t *shell)
{
    /* 机箱底：深炭黑渐变（Palette 内两色，不引入第 16 个近似色） */
    lv_obj_t *chassis = lv_obj_create(shell);
    lv_obj_set_size(chassis, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(chassis, 0, 0);
    lv_obj_set_style_bg_color(chassis, CAS_COL_PANEL, 0);
    lv_obj_set_style_bg_grad_color(chassis, CAS_COL_BG, 0);
    lv_obj_set_style_bg_grad_dir(chassis, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_stop(chassis, 100, 0);
    lv_obj_set_style_bg_opa(chassis, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chassis, 0, 0);
    lv_obj_set_style_radius(chassis, 0, 0);
    lv_obj_set_style_pad_all(chassis, 0, 0);
    lv_obj_clear_flag(chassis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chassis, LV_OBJ_FLAG_CLICKABLE);

    /* 外圈金属框 + 内圈亮边（拉丝纹用极低对比实色近似，不叠全屏位图） */
    const int insets[2] = { CAS_FRAME_INSET_1, CAS_FRAME_INSET_2 };
    const lv_color_t colors[2] = { CAS_COL_METAL, CAS_COL_METAL_HI };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *frame = lv_obj_create(shell);
        lv_obj_set_size(frame, EXAMPLE_LCD_H_RES - 2 * insets[i],
                        EXAMPLE_LCD_V_RES - 2 * insets[i]);
        lv_obj_set_pos(frame, insets[i], insets[i]);
        lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(frame, 2 - i, 0);
        lv_obj_set_style_border_color(frame, colors[i], 0);
        lv_obj_set_style_radius(frame, 12 + i * 4, 0);
        lv_obj_set_style_pad_all(frame, 0, 0);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ------------------------- 状态栏 ------------------------- */

/**
 * 整条状态栏点击：开合抽屉（P0-3）。
 * 只挂在 bar 上：grip/label 等子对象的 LV_EVENT_CLICKED 会冒泡到 bar，
 * 子对象不再单独挂 cb，因此一次点击只会 toggle 一次。
 */
static void cas_statusbar_click_cb(lv_event_t *e)
{
    (void)e;
    cas_drawer_toggle();
}

static void cas_statusbar_create(lv_obj_t *shell)
{
    lv_obj_t *bar = lv_obj_create(shell);
    lv_obj_set_size(bar, EXAMPLE_LCD_H_RES, CAS_STATUS_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, CAS_COL_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, CAS_COL_METAL, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 整条状态栏（含子对象冒泡）都可点击开合抽屉；grip 不单独挂 cb。 */
    lv_obj_add_event_cb(bar, cas_statusbar_click_cb, LV_EVENT_CLICKED, NULL);

    /* 左：当前模块标题（Web 各屏 .st 文案，由 cas_nav_show 同步） */
    s_title_label = lv_label_create(bar);
    lv_label_set_text(s_title_label, "袖珍磁带机");
    lv_obj_set_style_text_font(s_title_label, CAS_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_title_label, CAS_COL_FG, 0);
    lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, CAS_CQW(3), 0);

    /* 中：琥珀时钟。复用 clock_display_attach()（已接真机 RTC / 12-24h），
       ⚠ 不另造演示时钟（Q4 / F15）。 */
    s_clock_label = lv_label_create(bar);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_font(s_clock_label, CAS_FONT_NUM, 0);
    lv_obj_set_style_text_color(s_clock_label, CAS_COL_AMBER, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, CAS_CQW(3), 0);
    clock_display_attach(s_clock_label);

    /* 右：蓝牙图标 + 电池百分比 */
    s_bt_label = lv_label_create(bar);
    lv_label_set_text(s_bt_label, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_bt_label, CAS_FONT_ICON, 0);
    lv_obj_set_style_text_color(s_bt_label, CAS_COL_DIM, 0);
    lv_obj_align(s_bt_label, LV_ALIGN_RIGHT_MID, -CAS_CQW(20), 0);

    s_batt_label = lv_label_create(bar);
    lv_label_set_text(s_batt_label, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_set_style_text_font(s_batt_label, CAS_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_batt_label, CAS_COL_FG, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -CAS_CQW(9), 0);

    /* 最右：下拉把手（视觉提示；点击由 bar 的冒泡回调统一处理，P1 加拖拽手势） */
    lv_obj_t *grip = lv_obj_create(bar);
    lv_obj_set_size(grip, CAS_CQW(7), CAS_STATUS_H - 6);
    lv_obj_set_align(grip, LV_ALIGN_RIGHT_MID);
    lv_obj_set_x(grip, -2);
    lv_obj_add_style(grip, cas_style_metal(), 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *ridge = lv_obj_create(grip);
        lv_obj_set_size(ridge, CAS_CQW(4), 2);
        lv_obj_set_pos(ridge, CAS_CQW(1.5), 6 + i * 5);
        lv_obj_set_style_bg_color(ridge, CAS_COL_METAL_HI, 0);
        lv_obj_set_style_bg_opa(ridge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ridge, 0, 0);
        lv_obj_set_style_radius(ridge, 0, 0);
        lv_obj_clear_flag(ridge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ridge, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* 5 秒周期：电池文本（I2C 读）+ 蓝牙状态色。时钟由 clock_display 自刷新。 */
static void cas_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_batt_label != NULL) {
        char buf[24];
        lv_color_t color = lv_color_white();
        battery_indicator_text(buf, sizeof(buf), &color);
        const char *cur = lv_label_get_text(s_batt_label);
        if (cur == NULL || strcmp(cur, buf) != 0) {
            lv_label_set_text(s_batt_label, buf);
        }
        lv_obj_set_style_text_color(s_batt_label, color, 0);
    }
    if (s_bt_label != NULL) {
        const bool bt = cas_state_get()->bt;
        lv_obj_set_style_text_color(s_bt_label,
                                    bt ? CAS_COL_AMBER : CAS_COL_DIM, 0);
    }
}

void cas_shell_set_title(const char *title)
{
    if (s_title_label == NULL || title == NULL) {
        return;
    }
    lv_label_set_text(s_title_label, title);
}

void cassette_ui_create(void)
{
    if (s_root != NULL) {
        ESP_LOGW(TAG, "cassette UI already created");
        return;
    }
    lv_obj_t *scr = lv_scr_act();

    /* P0 示例状态先落到 RAM（纯 RAM，不写 NVS，F16）；7 个模块及抽屉
       都只从这里读写，不各自持有跨屏变量。 */
    cas_state_reset();

    /* cas_root：全屏不透明底。任一层没铺到时也不会露出旧帧。 */
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, CAS_COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* shell 先建：机箱壳/状态栏在页面之下，透明页内容压在其上。 */
    s_shell = lv_obj_create(s_root);
    lv_obj_set_size(s_shell, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_shell, 0, 0);
    lv_obj_set_style_bg_opa(s_shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_shell, 0, 0);
    lv_obj_set_style_pad_all(s_shell, 0, 0);
    lv_obj_clear_flag(s_shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_shell, LV_OBJ_FLAG_CLICKABLE);

    cas_chassis_create(s_shell);
    cas_statusbar_create(s_shell);

    /* 四角螺丝：Web 原型 20°/70°/-30°/10°，LVGL 单位 0.1° */
    const int screw_size = CAS_SCREW_SIZE;
    const int xr = EXAMPLE_LCD_H_RES - CAS_CQW(3) - screw_size;
    const int yb = EXAMPLE_LCD_V_RES - CAS_CQW(3) - screw_size;
    cas_shell_screw_create(s_shell, CAS_CQW(3), CAS_CQW(3), 200);
    cas_shell_screw_create(s_shell, xr, CAS_CQW(3), 700);
    cas_shell_screw_create(s_shell, CAS_CQW(3), yb, -300);
    cas_shell_screw_create(s_shell, xr, yb, 100);

    /* pages：7 个全屏透明页，同一时刻仅一个非 HIDDEN */
    lv_obj_t *pages = lv_obj_create(s_root);
    lv_obj_set_size(pages, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(pages, 0, 0);
    lv_obj_set_style_bg_opa(pages, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pages, 0, 0);
    lv_obj_set_style_pad_all(pages, 0, 0);
    lv_obj_clear_flag(pages, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pages, LV_OBJ_FLAG_CLICKABLE);
    cas_nav_init(pages);

    /* drawer 最后创建：屏幕层最上（F11），不挂 lv_layer_top() */
    cas_drawer_create(s_root);

    /* 默认停在 M1 磁带仓；T03 起各模块内容填进对应 page root。 */
    cas_nav_show(PAGE_PLAYER);

    lv_timer_create(cas_refresh_timer_cb, CAS_REFRESH_MS, NULL);
    cas_refresh_timer_cb(NULL);   /* 创建后立即填充电池初值 */

    ESP_LOGI(TAG, "cassette UI created: root/shell/pages(7)/drawer");
}
