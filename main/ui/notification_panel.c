#include "ui/notification_panel.h"
#include "ui/list_common.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/fonts/system_font.h"
#include "core/fonts/system_smaller_font.h"
#include "core/storage/config_store.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "notification_panel";

#define NOTIF_BAR_HEIGHT   EXAMPLE_LCD_V_RES  /* 全屏下拉面板 */
#define NOTIF_TRIGGER_H    UI_STATUS_BAR_H    /* 顶部触发区域高度（= 状态栏高度） */
#define PANEL_OPEN_Y       0                  /* 面板展开时顶部 y（全屏） */
#define PANEL_CLOSED_Y     (-NOTIF_BAR_HEIGHT) /* 收起时完全隐藏于屏幕上方 */

/* 拖拽跟随与速度决策 */
#define VEL_SAMPLES  6
#define VEL_THRESH   400   /* px/s，超过则按速度方向决定 */

static lv_obj_t *notif_bar = NULL;

/* 亮度滑块句柄（供配置变更回调同步其它页面对同一配置源的修改） */
static lv_obj_t *brightness_slider = NULL;

static lv_obj_t *bat_value_label = NULL;
static lv_obj_t *bat_bar = NULL;
static lv_obj_t *bat_pct_label = NULL;

static bool dragging = false;
static int32_t drag_press_y = 0;
static int32_t drag_panel_start_y = 0;
static int32_t v_t[VEL_SAMPLES];
static int32_t v_y[VEL_SAMPLES];
static uint8_t v_idx = 0;
static uint8_t v_cnt = 0;

/* 下拉通知栏位置动画回调 */
static void notif_anim_cb(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, value);
}

/* 通知栏动画到指定位置 */
static void notif_animate_to(int32_t target)
{
    int32_t cur = lv_obj_get_y(notif_bar);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, notif_bar);
    lv_anim_set_exec_cb(&a, notif_anim_cb);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 通知栏/状态栏触摸事件回调：由 LVGL 输入设备事件驱动（复用 indev 触摸数据，无额外轮询）。
   - PRESSED：在顶部触发区（状态栏）或底部把手区按下时开始拖拽；
   - PRESSING：面板跟随手指，并记录速度样本；
   - RELEASED：按速度矢量决定展开/收起。
   滑块等可点击子控件会自行消费事件（不冒泡），不会触发面板拖拽。 */
static void notif_panel_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    uint16_t ty = point.y;

    switch (code) {
    case LV_EVENT_PRESSED: {
        bool in_top_grab    = (ty < NOTIF_TRIGGER_H);
        bool in_bottom_grab = (ty >= EXAMPLE_LCD_V_RES - NOTIF_TRIGGER_H);
        if (!dragging && (in_top_grab || in_bottom_grab)) {
            dragging = true;
            drag_press_y = ty;
            drag_panel_start_y = lv_obj_get_y(notif_bar);
            v_idx = 0;
            v_cnt = 0;
            lv_anim_del(notif_bar, notif_anim_cb);  /* 停止进行中的动画，避免抢占 */
        }
        break;
    }
    case LV_EVENT_PRESSING: {
        if (!dragging) break;
        int32_t new_y = drag_panel_start_y + (int32_t)(ty - drag_press_y);
        if (new_y > PANEL_OPEN_Y) new_y = PANEL_OPEN_Y;
        if (new_y < PANEL_CLOSED_Y) new_y = PANEL_CLOSED_Y;
        lv_obj_set_y(notif_bar, new_y);

        v_t[v_idx] = lv_tick_get();
        v_y[v_idx] = ty;
        v_idx = (v_idx + 1) % VEL_SAMPLES;
        if (v_cnt < VEL_SAMPLES) v_cnt++;
        break;
    }
    case LV_EVENT_RELEASED: {
        if (!dragging) break;
        dragging = false;

        int32_t vel = 0;
        if (v_cnt >= 2) {
            uint8_t last = (v_idx - 1 + VEL_SAMPLES) % VEL_SAMPLES;
            uint8_t span = (v_cnt > VEL_SAMPLES) ? VEL_SAMPLES : v_cnt;
            uint8_t old  = (v_idx + VEL_SAMPLES - span) % VEL_SAMPLES;
            int32_t dt   = v_t[last] - v_t[old];
            if (dt > 0) {
                vel = (v_y[last] - v_y[old]) * 1000 / dt;   /* px/s，正值=向下 */
            }
        }

        int32_t cur_y = lv_obj_get_y(notif_bar);
        bool open;
        if (vel > VEL_THRESH)        open = true;                    /* 快速下滑 -> 展开 */
        else if (vel < -VEL_THRESH)  open = false;                   /* 快速上滑 -> 收起 */
        else                         open = (cur_y > PANEL_CLOSED_Y + NOTIF_BAR_HEIGHT / 2); /* 超过一半 -> 展开 */

        notif_animate_to(open ? PANEL_OPEN_Y : PANEL_CLOSED_Y);
        break;
    }
    default:
        break;
    }
}

/* 给对象挂载通知栏拖拽事件（状态栏、通知栏共用） */
static void notif_attach_events(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_RELEASED, NULL);
}

/* 亮度滑块事件回调：滑块值 0~255 直接映射到屏幕亮度。
   写入配置存储（NVS 持久化 + 通知其它 UI 同步），再应用硬件亮度。
   回调运行在 LVGL 任务上下文（已持锁），display_set_brightness 安全。 */
static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t brightness = (uint8_t)lv_slider_get_value(slider);
    config_store_set_brightness(brightness);
    esp_err_t ret = display_set_brightness(brightness);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set brightness %u failed: %s", brightness, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Set brightness %u", brightness);
    }
}

/* 配置变更回调：其它界面（如播放设置页）修改亮度后，同步本页滑块位置 */
static void notif_config_changed_cb(void *ctx)
{
    (void)ctx;
    if (brightness_slider != NULL) {
        lv_slider_set_value(brightness_slider,
                            (int32_t)config_store_get_brightness(), LV_ANIM_OFF);
    }
}

/* 事件穿透：该对象永远不参与命中测试，触摸事件穿透到下层控件 */
static void pass_through_hit_test_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    if (info != NULL) {
        info->res = false;
    }
}

/* 周期刷新电池电压与电量百分比 */
static void battery_timer_cb(lv_timer_t *timer)
{
    char buf[24];

    float vbat_mv = battery_read_voltage_mv();

    if (vbat_mv < 0) {
        lv_label_set_text(bat_value_label, "--.- V");
        lv_bar_set_value(bat_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(bat_pct_label, "--%");
        return;
    }

    snprintf(buf, sizeof(buf), "%.2f V", vbat_mv / 1000.0f);
    lv_label_set_text(bat_value_label, buf);

    /* 估算电量百分比 */
    float pct = (vbat_mv - BAT_EMPTY_MV) / (float)(BAT_FULL_MV - BAT_EMPTY_MV) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bat_bar, (int)pct, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    lv_label_set_text(bat_pct_label, buf);
}

void notification_panel_create(lv_obj_t *status_bar)
{
    lv_obj_t *scr = lv_scr_act();

    /* 下拉通知栏（全屏，最后创建=绘制在状态栏之上，默认隐藏在屏幕上方） */
    notif_bar = lv_obj_create(scr);
    lv_obj_set_size(notif_bar, EXAMPLE_LCD_H_RES, NOTIF_BAR_HEIGHT);
    lv_obj_set_pos(notif_bar, 0, PANEL_CLOSED_Y);
    lv_obj_set_style_bg_color(notif_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(notif_bar, 2, 0);
    lv_obj_set_style_border_side(notif_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(notif_bar, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(notif_bar, 0, 0);
    /* 内容区铺满全屏：清除默认 padding，禁止滚动与滚动条，避免子对象宽度超出出现左右滚动条 */
    lv_obj_set_style_pad_all(notif_bar, 0, 0);
    lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(notif_bar, LV_SCROLLBAR_MODE_OFF);

    /* 通知栏内容：电池电压（全屏居中布局） */
    lv_obj_t *title = lv_label_create(notif_bar);
    lv_label_set_text(title, LV_SYMBOL_BATTERY_FULL " Battery");
    lv_obj_set_style_text_font(title, &system_smaller_font, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 90);

    bat_value_label = lv_label_create(notif_bar);
    lv_label_set_text(bat_value_label, "--.- V");
    lv_obj_set_style_text_font(bat_value_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(bat_value_label, lv_color_white(), 0);
    lv_obj_align(bat_value_label, LV_ALIGN_CENTER, 0, -60);

    bat_bar = lv_bar_create(notif_bar);
    lv_obj_set_size(bat_bar, 320, 30);
    lv_bar_set_range(bat_bar, 0, 100);
    lv_bar_set_value(bat_bar, 0, LV_ANIM_OFF);
    lv_obj_align(bat_bar, LV_ALIGN_CENTER, 0, 20);

    bat_pct_label = lv_label_create(notif_bar);
    lv_label_set_text(bat_pct_label, "0%");
    lv_obj_set_style_text_font(bat_pct_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(bat_pct_label, lv_color_white(), 0);
    lv_obj_align(bat_pct_label, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *note = lv_label_create(notif_bar);
    lv_label_set_text(note, "VBAT via 10K+10K divider (GPIO1)");
    lv_obj_set_style_text_color(note, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -80);

    /* 底部上拉把手：全宽、与顶部触发区同高，接收上拉收起手势。
       作为 notif_bar 的子对象，随面板一起移动/隐藏。
       宽度自适应父容器内容区，不参与命中测试，只作为手势区与视觉提示。 */
    lv_obj_t *grab_bar = lv_obj_create(notif_bar);
    lv_obj_set_width(grab_bar, lv_pct(100));
    lv_obj_set_height(grab_bar, NOTIF_TRIGGER_H);
    lv_obj_align(grab_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(grab_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(grab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(grab_bar, 1, 0);
    lv_obj_set_style_border_side(grab_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(grab_bar, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(grab_bar, 0, 0);
    lv_obj_clear_flag(grab_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grab_bar, pass_through_hit_test_cb, LV_EVENT_HIT_TEST, NULL);

    lv_obj_t *grab_label = lv_label_create(grab_bar);
    lv_label_set_text(grab_label, LV_SYMBOL_UP " Swipe up to close");
    lv_obj_set_style_text_font(grab_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(grab_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_center(grab_label);

    /* 亮度调节：标签 + 滑块（0~255，初始为配置存储中的亮度）。
       注意：UI 创建时硬件亮度仍为 0（全黑启动），此处直接用配置值
       （默认 DISPLAY_BRIGHTNESS_DEFAULT），与 app_main 点亮后恢复的亮度一致。
       配置变更（如播放设置页修改）经 config_store 回调同步本滑块。 */
    lv_obj_t *brightness_label = lv_label_create(notif_bar);
    lv_label_set_text(brightness_label, LV_SYMBOL_SETTINGS " Brightness");
    lv_obj_set_style_text_font(brightness_label, &system_smaller_font, 0);
    lv_obj_set_style_text_color(brightness_label, lv_color_white(), 0);
    lv_obj_align(brightness_label, LV_ALIGN_BOTTOM_MID, 0, -160);

    brightness_slider = lv_slider_create(notif_bar);
    lv_obj_set_size(brightness_slider, 320, 20);
    lv_slider_set_range(brightness_slider, 0, 255);
    lv_slider_set_value(brightness_slider,
                        (int32_t)config_store_get_brightness(), LV_ANIM_OFF);
    lv_obj_align(brightness_slider, LV_ALIGN_BOTTOM_MID, 0, -120);
    lv_obj_add_event_cb(brightness_slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 订阅配置变更：设置页等其它入口修改亮度时同步本滑块 */
    config_store_register_cb(notif_config_changed_cb, NULL);

    /* 电池周期刷新（约 1s） */
    lv_timer_create(battery_timer_cb, 1000, NULL);

    /* 触摸事件：状态栏（面板收起时下拉）与通知栏（展开时上拉/下拉）共用拖拽事件。
       滑块等可点击子控件会消费自身事件（不冒泡），不会触发面板拖拽。 */
    notif_attach_events(status_bar);
    notif_attach_events(notif_bar);

    ESP_LOGI(TAG, "Notification panel created");
}
