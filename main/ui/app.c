#include "ui/app.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/notification_panel.h"
#include "ui/now_playing.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app";

/* 顶部状态栏高度（同时也是通知栏下拉触发区高度），统一见 UI_STATUS_BAR_H */
#define STATUS_BAR_H  UI_STATUS_BAR_H

/* 状态栏电量/音量刷新周期（ms）：电压 0.01V 精度，每 5 秒更新一次 */
#define STATUS_BATTERY_REFRESH_MS  5000

/* 配置变更通知派发周期（ms）：config_store 的 set_xxx 只置"待通知"标志，
   由本文件创建的 lv_timer 在 LVGL 任务里统一派发（原因见 config_store.h
   的变更回调说明与 app_ui_create 注释）。100ms 足够"按下即见"，又不会因为
   高频改配置（拖音量）反复重绘。 */
#define CONFIG_NOTIFY_POLL_MS      100

/* 当前页面名称（与 main_ui 的四页滑动 tab 下标一一对应） */
static const char *const PAGE_NAMES[] = {
    "目录 / 歌单",   /* tab 0 */
    "播放列表",      /* tab 1 */
    "正在播放",      /* tab 2 */
    "播放设置",      /* tab 3 */
};

static lv_obj_t *s_status_bar     = NULL;  /* 状态栏容器（透明隐藏时保留事件） */
static lv_obj_t *page_title_label = NULL;   /* 状态栏中央：当前页面名称 */
static lv_obj_t *vol_label        = NULL;   /* 状态栏左侧：音量图标+数值（单 label） */
static lv_obj_t *bat_label        = NULL;   /* 状态栏右侧：电池图标+电压（单 label） */

/* 状态栏图标与数值字体：统一用 LVGL 内置 Montserrat 20。
   - 图标：LVGL 内置字体含 LV_SYMBOL 图标字形（系统精简字体不含 PUA 区图标）；
   - 数值：音量/电压只有数字、字母、小数点（非中文），无需系统字体，
     用内置字体即可（比系统字体更小、更紧凑，字号比 16px 更清晰）。
   状态栏中央的页面名称（中文）仍用 UI_SYSTEM_FONT。 */
#define STATUS_SMALL_FONT  (&lv_font_montserrat_20)

/* 状态栏背景不透明度渐变时长（ms）：与播放页封面渐显/栏恢复同步 */
#define STATUS_BAR_FADE_MS  300

/* 状态栏背景不透明度动画回调 */
static void status_bg_opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/* 电量图标按百分比分级（对应 LV_SYMBOL_BATTERY_* 五档） */
static const char *status_battery_icon(float pct)
{
    if (pct >= 90.0f) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60.0f) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40.0f) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15.0f) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* 状态栏音量图标固定用"中音量"图标：界面只看后面的数字档位，
   图标不随音量变化（避免每按一下图标跳变） */
#define VOLUME_ICON_FIXED  LV_SYMBOL_VOLUME_MID

/* 独立刷新状态栏音量（label 已创建时）：按键调节音量后由 config_store
   变更回调即时同步，不必等 5 秒周期刷新。
   读 config_store 的配置值（set_volume 先更新内存值再通知回调，必为新值）；
   不读 audio_get_volume_level()——按键调音量时 audio_set_volume_level 与
   config 回调的先后顺序在不同调用路径下不保证，读配置值最稳。
   ⚠ 调用方必须持有 LVGL 锁（config 回调在按键任务上下文也可能触发）。 */
static void status_refresh_volume(void)
{
    if (vol_label == NULL) {
        return;
    }
    uint8_t vol = config_store_get_volume();
    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), VOLUME_ICON_FIXED " %u", (unsigned)vol);
    lv_label_set_text(vol_label, vol_buf);
}

/* 配置变更回调：音量（按键调节）变化后立即刷新状态栏音量显示。
   ⚠ 本回调由 ui/app.c 的派发定时器在 LVGL 任务上下文里调用（config_store 的
   set_xxx 只置标志，不再在调用者线程里同步回调），可以安全操作 LVGL。 */
static void app_config_changed_cb(void *ctx)
{
    (void)ctx;
    status_refresh_volume();
}

/* 配置变更通知派发定时器（LVGL 任务上下文）：把 config_store 里累积的
   "待通知"标志兑现成一次 UI 刷新（状态栏音量 / 通知栏滑块 / 设置页文字）。 */
static void config_notify_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    config_store_notify_dispatch();
}

/* 状态栏周期刷新：电量（电池图标按百分比分级 + 电压 0.01V）+ 音量（实时跟随）。
   图标与数值拼接为单个 label（图标 + 空格 + 文本）。
   创建后立即手动调用一次填充初始值。 */
static void status_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 电量：电压精确到 0.01V；图标按电量百分比分级 */
    float vbat_mv = battery_read_voltage_mv();
    if (vbat_mv < 0) {
        lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_EMPTY " --.-- V");
    } else {
        float pct = (vbat_mv - BAT_EMPTY_MV) / (float)(BAT_FULL_MV - BAT_EMPTY_MV)
                    * 100.0f;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %.2f V",
                 status_battery_icon(pct), vbat_mv / 1000.0f);
        lv_label_set_text(bat_label, buf);
    }

    /* 音量：当前音量等级（0~100） */
    status_refresh_volume();
}

/* 独立顶部状态栏：左=音量（图标+等级）、中=当前页面名称、右=电量（图标+电压）。
   返回状态栏句柄供通知栏挂载拖拽事件。 */
static lv_obj_t *create_status_bar(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_set_size(status, EXAMPLE_LCD_H_RES, STATUS_BAR_H);
    lv_obj_set_pos(status, 0, 0);
    s_status_bar = status;
    /* 半透明黑背景（75% ≈ 191/255；LVGL 无 75% 预定义档，用数值）：
       下面主界面内容隐约透出，为沉浸式封面做准备；无底部边框线。
       显式去圆角：默认主题的 lv_obj 带圆角，全宽条两端会显示圆角 */
    lv_obj_set_style_bg_color(status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status, (lv_opa_t)191, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);

    /* 左侧：音量（固定图标 + 空格 + 数值，单 label） */
    vol_label = lv_label_create(status);
    lv_label_set_text(vol_label, VOLUME_ICON_FIXED " --");
    lv_obj_set_style_text_font(vol_label, STATUS_SMALL_FONT, 0);
    lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
    lv_obj_align(vol_label, LV_ALIGN_LEFT_MID, 12, 0);

    /* 中间：当前页面名称（默认第 1 页"目录 / 歌单"，随 tab 切换更新） */
    page_title_label = lv_label_create(status);
    lv_label_set_text(page_title_label, PAGE_NAMES[0]);
    lv_obj_set_style_text_font(page_title_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(page_title_label, lv_color_white(), 0);
    lv_obj_align(page_title_label, LV_ALIGN_CENTER, 0, 0);

    /* 右侧：电量（电池图标 + 空格 + 电压，单 label，右对齐） */
    bat_label = lv_label_create(status);
    lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_FULL " --.-- V");
    lv_obj_set_style_text_font(bat_label, STATUS_SMALL_FONT, 0);
    lv_obj_set_style_text_color(bat_label, lv_color_white(), 0);
    lv_obj_align(bat_label, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 周期刷新（每 5 秒）+ 立即填充一次初始值 */
    lv_timer_create(status_refresh_timer_cb, STATUS_BATTERY_REFRESH_MS, NULL);
    status_refresh_timer_cb(NULL);

    /* 订阅配置变更：音量（按键调节等）变化后即时刷新状态栏音量显示。
       回调在调用 set_xxx 的线程上下文执行（按键任务持锁调用，安全）。 */
    config_store_register_cb(app_config_changed_cb, NULL);

    /* 状态栏创建晚于主界面：开机可能已切到非第 0 页（如默认停在正在播放页），
       需同步一次当前页名称（此前 main_ui_switch_tab 时的同步因状态栏未建被忽略） */
    app_status_set_page(main_ui_get_current_tab());

    return status;
}

void app_ui_create(void)
{
    /* 配置变更通知派发定时器（100ms）：config_store 的所有 set_xxx 只置"待通知"
       标志，回调（状态栏音量/通知栏滑块/设置页文字）统一在这里、在 LVGL 任务
       上下文里执行。
       ⚠ 不能改成"在 set_xxx 里同步回调"：set_xxx 会被播放控制器任务
       （player_ctrl）调用，回调里 lv_label_set_text 会在非 LVGL 任务里改对象
       树 → 2026-11 实测卡死在 lv_inv_area 并触发 task_wdt。
       延迟 100ms 只影响"数值标签刷新"，硬件（音量/EQ/亮度）仍由调用方立即应用。 */
    lv_timer_create(config_notify_timer_cb, CONFIG_NOTIFY_POLL_MS, NULL);

    /* 【路径B】"热点对象优先内部 RAM"的窗口已下移到 ui/main_ui.c，只覆盖两个
       滚动列表页（目录 / 播放列表）；状态栏与下拉控制中心照旧走 PSRAM。
       依据（钉更多并不会更快，只是白吃内存余量）：IRAM_AUDIT_LVGL9.md §13。 */
    ESP_LOGI(TAG, "Create main UI");
    main_ui_create();

    ESP_LOGI(TAG, "Create status bar");
    lv_obj_t *status_bar = create_status_bar();

    ESP_LOGI(TAG, "Create notification panel");
    notification_panel_create(status_bar);

    /* 整段 UI 创建完毕，至此才放行首次封面加载：封面解码（song_meta_load +
       esp_jpeg）同样要占内部 RAM，若与上面的"热点对象钉内部 RAM"窗口并行跑，
       钉住量每次开机会抖动约 10KB、帧率随之波动（见
       ideas_and_exps/IRAM_AUDIT_LVGL9.md §13.3）。放行后切歌照常实时加载。 */
    now_playing_enable_cover_load();

    /* ⚠ 全屏设置菜单**不在开机时创建**（懒加载，见 ui/settings_menu.h）：
       它常驻实测约 29KB 内部 RAM、构建约 310ms，改为首次点开设置时创建，
       开机主界面因此提前约 305ms 出现、并多出约 29KB 内部 RAM 与 DMA 余量。
       其层级（屏幕层最上）依然成立——首次打开必定晚于本函数创建的一切。 */
}

void app_status_set_page(uint8_t tab_index)
{
    /* 状态栏由 main_ui 之后创建，tab 事件可能早于状态栏触发，判空保护 */
    if (page_title_label == NULL) {
        return;
    }
    uint8_t count = (uint8_t)(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]));
    if (tab_index >= count) {
        return;
    }
    lv_label_set_text(page_title_label, PAGE_NAMES[tab_index]);
}

void app_status_set_ui_visible(bool visible)
{
    if (s_status_bar == NULL) {
        return;
    }
    /* 立即设置前先删除进行中的背景渐变动画，避免动画回调覆盖新值 */
    lv_anim_del(s_status_bar, status_bg_opa_anim_cb);
    if (visible) {
        /* 恢复：半透明黑背景（75%）+ 显示三个内容 label */
        lv_obj_set_style_bg_opa(s_status_bar, (lv_opa_t)191, 0);
        lv_obj_clear_flag(page_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(vol_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bat_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 隐藏：背景完全透明 + 内容 label 隐藏。
           ⚠ 不删除状态栏对象、不清 CLICKABLE：通知栏下拉事件挂载在
           状态栏上，需保留事件接收（供全屏封面时下拉通知栏） */
        lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(page_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(vol_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bat_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void app_status_set_bg_opa(lv_opa_t opa, bool animated)
{
    if (s_status_bar == NULL) {
        return;
    }
    if (!animated) {
        /* 立即设置（滚动开始时直接不透明纯黑，滚动期间省去 alpha 混合） */
        lv_anim_del(s_status_bar, status_bg_opa_anim_cb);
        lv_obj_set_style_bg_opa(s_status_bar, opa, 0);
        return;
    }
    /* 渐变设置（滚动停止后与播放页封面渐显同步恢复半透明） */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_status_bar);
    lv_anim_set_exec_cb(&a, status_bg_opa_anim_cb);
    lv_anim_set_values(&a, (int32_t)lv_obj_get_style_bg_opa(s_status_bar, 0), opa);
    lv_anim_set_time(&a, STATUS_BAR_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}
