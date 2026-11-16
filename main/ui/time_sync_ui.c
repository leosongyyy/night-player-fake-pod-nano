#include "ui/time_sync_ui.h"

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/network/wifi_time_sync.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/power_service.h"   /* 两个角色都有 power_service（里世界 2026-11 起） */
#include "ui/time_date_popup.h"
#include "ui/toast.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "time_sync_ui";

/* ------------------------------------------------------------------ */
/* 布局与配色                                                           */
/* ------------------------------------------------------------------ */

#define TS_SCREEN_W   EXAMPLE_LCD_H_RES
#define TS_SCREEN_H   EXAMPLE_LCD_V_RES

#define TS_BG         lv_color_hex(0x000000)
#define TS_TEXT       lv_color_white()
#define TS_DIM        lv_palette_main(LV_PALETTE_GREY)
#define TS_ACCENT     lv_color_hex(0xFFC000)
#define TS_ERROR      lv_color_hex(0xFF6B6B)
#define TS_OK         lv_color_hex(0x7DE08A)

/* 同步任务的栈必须放**内部 RAM**（WiFi 初始化会写 NVS/PHY 校准数据，
   PSRAM 栈任务直接触发关缓存断言）。8KB 足够（WiFi 示例通常 4KB）。 */
#define TS_TASK_STACK  (8 * 1024)
#define TS_TASK_PRIO   5
#define TS_TASK_CORE   1

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_root = NULL;      /* 全屏黑底 */
static lv_obj_t *s_status = NULL;    /* 当前阶段/结果 */
static lv_obj_t *s_detail = NULL;    /* SSID / NTP 服务器 / 失败提示 */
static lv_obj_t *s_bar = NULL;       /* 进度条 */
static lv_obj_t *s_percent = NULL;   /* 百分比文字 */
static lv_obj_t *s_btn = NULL;       /* 取消 / 关闭 */
static lv_obj_t *s_btn_label = NULL;

static volatile bool s_visible = false;
static volatile bool s_running = false;   /* 同步任务还在跑 */
static volatile bool s_finished = false;  /* 已出结果（按钮变成"关闭"） */
static volatile bool s_cancel = false;    /* 取消标志（同步任务轮询） */
static lv_timer_t *s_auto_close_timer = NULL;  /* 结果展示到期后自动结束（一次性） */

/* 结束去向：对时固件注册了 done 回调（写完结果要重启回播放器），
   播放器角色不注册（关闭界面回原界面）。 */
static time_sync_ui_done_cb_t s_done_cb = NULL;
static wifi_time_sync_result_t s_last_result = WIFI_TIME_SYNC_OK;

/* 抑制自动息屏：网络对时最长可达一分多钟，期间没有触摸活动，必须拦住
   "无操作自动息屏"（息屏后再过 10 分钟就是自动关机）。2026-11 起里世界也
   跑 power_service（自动息屏 + 超时关机），所以两个角色都要真正生效。 */
static void hold_awake(bool hold)
{
    power_service_set_hold_awake(hold);
}

/* 前向声明：按钮回调在 sync_ui_create 里注册，定义在后面 */
static void cancel_btn_cb(lv_event_t *e);

/* ------------------------------------------------------------------ */
/* 控件构建                                                             */
/* ------------------------------------------------------------------ */

static void button_style(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, TS_ACCENT, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
}

static void sync_ui_create(void)
{
    /* 全屏不透明覆盖层 → 挂**屏幕层**（当前屏幕），不挂 lv_layer_top()：
       LVGL 的覆盖判定 lv_refr_get_top_obj() 只搜索屏幕的子对象树，顶层对象
       不在其中 → 每个绘制块都要把下层再画一遍、再无条件画顶层。
       本界面在屏幕上盖满整屏，放进屏幕树后它自己就是覆盖对象，下层被跳过
       （同类事故见 DONE_PLANS.md 第 38 点）。 */
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_root, TS_SCREEN_W, TS_SCREEN_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, TS_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "WiFi对时");
    lv_obj_set_style_text_font(title, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title, TS_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *subtitle = lv_label_create(s_root);
    lv_label_set_text(subtitle, "读取 /sdcard/.ecov/wifi.txt");
    lv_obj_set_style_text_font(subtitle, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(subtitle, TS_DIM, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "准备对时…");
    lv_obj_set_style_text_font(s_status, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_status, TS_TEXT, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 400);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 132);

    s_detail = lv_label_create(s_root);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_font(s_detail, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_detail, TS_DIM, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail, 400);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 224);

    s_bar = lv_bar_create(s_root);
    lv_obj_set_size(s_bar, 340, 14);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 300);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, TS_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 7, LV_PART_INDICATOR);

    s_percent = lv_label_create(s_root);
    lv_label_set_text(s_percent, "0%");
    lv_obj_set_style_text_font(s_percent, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_percent, TS_DIM, 0);
    lv_obj_align(s_percent, LV_ALIGN_TOP_MID, 0, 322);

    s_btn = lv_obj_create(s_root);
    lv_obj_set_size(s_btn, 200, 52);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    button_style(s_btn);

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, "取消");
    lv_obj_set_style_text_font(s_btn_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_btn_label, TS_TEXT, 0);
    lv_obj_center(s_btn_label);

    /* 事件只注册一次：按钮常驻复用，start 里重复注册会叠加回调 */
    lv_obj_add_event_cb(s_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "sync ui created on screen layer");
}

/* ------------------------------------------------------------------ */
/* 行为                                                                */
/* ------------------------------------------------------------------ */

static void sync_ui_close(void)
{
    /* 取消尚未触发的自动关闭，避免它把下一次对时界面关掉 */
    if (s_auto_close_timer != NULL) {
        lv_timer_del(s_auto_close_timer);
        s_auto_close_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    s_visible = false;
    s_finished = false;
    s_cancel = false;
    /* 恢复自动息屏 */
    hold_awake(false);
    ESP_LOGI(TAG, "sync ui closed");
}

/* 结束去向：注册了回调（里世界）→ 交给回调（它把界面收掉、回功能列表）；
   否则关闭界面回原界面（播放器角色的旧行为）。 */
static void finish_now(void)
{
    if (s_done_cb != NULL) {
        time_sync_ui_done_cb_t cb = s_done_cb;
        /* ⚠ 不清空注册：里世界里对时可以反复做（每次结束都回功能列表）。
           回调执行完 s_finished=true，不会重复进入这里。 */
        s_running = false;
        s_finished = true;
        s_visible = false;
        ESP_LOGI(TAG, "handing over result %d to done callback",
                 (int)s_last_result);
        cb(s_last_result);
        return;
    }
    sync_ui_close();
}

/* 结果展示到期的定时器（一次性：repeat_count=1，LVGL 回调后自动删除） */
static void finish_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_auto_close_timer = NULL;
    finish_now();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_finished) {
        finish_now();
        return;
    }
    if (!s_cancel) {
        s_cancel = true;
        lv_label_set_text(s_status, "正在取消…");
        lv_label_set_text(s_detail, "正在断开 WiFi，请稍候");
        lv_label_set_text(s_btn_label, "取消中…");
        lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        ESP_LOGI(TAG, "cancel requested");
    }
}

/* 同步任务完成后的收尾（LVGL 上下文，持锁） */
static void sync_ui_finish(wifi_time_sync_result_t result)
{
    s_running = false;
    s_finished = true;
    s_last_result = result;
    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_btn_label, "关闭");

    if (result == WIFI_TIME_SYNC_OK) {
        lv_obj_set_style_text_color(s_status, TS_OK, 0);
        lv_label_set_text(s_status, "对时成功");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, TS_OK, LV_PART_INDICATOR);
        lv_label_set_text(s_percent, "100%");

        char detail[96];
        rtc_time_t rt;
        if (rtc_is_ready() && rtc_get_time(&rt) == ESP_OK) {
            char ds[16];
            char ts[16];
            time_date_format_date(&rt, ds, sizeof(ds));
            time_date_format_time(&rt, ts, sizeof(ts));
            char tz[16];
            time_date_format_tz(config_store_get_tz_offset_min(), tz, sizeof(tz));
            snprintf(detail, sizeof(detail), "%s %s · %s", ds, ts, tz);
        } else {
            snprintf(detail, sizeof(detail), "时间已写入 RTC");
        }
        lv_label_set_text(s_detail, detail);
        time_date_popup_notify_changed();
    } else {
        lv_obj_set_style_text_color(s_status,
                                    (result == WIFI_TIME_SYNC_CANCELLED)
                                        ? TS_DIM : TS_ERROR,
                                    0);
        lv_label_set_text(s_status, wifi_time_sync_result_text(result));
        lv_obj_set_style_bg_color(s_bar, TS_ERROR, LV_PART_INDICATOR);
        if (result == WIFI_TIME_SYNC_CANCELLED) {
            lv_label_set_text(s_detail, "未修改时间");
        } else {
            lv_label_set_text(s_detail, "请检查 SD 卡中的 wifi.txt 后重试");
        }
        ESP_LOGW(TAG, "sync finished with result %d", (int)result);
    }

    /* 结果出来后不再需要 hold：立即恢复自动息屏计时（界面仍在，用户可看结果） */
    hold_awake(false);

    /* 自动结束的延时：
       · 成功 2.5s（两种角色都一样，让用户看到"对时成功 + 新时间"）；
       · 失败/取消：对时固件 8s（必须回播放器，用户可能已走开）；
         播放器角色保持旧行为（等用户点「关闭」，不自动关）。 */
    int32_t delay = 0;
    if (result == WIFI_TIME_SYNC_OK) {
        delay = 2500;
    } else if (s_done_cb != NULL) {
        delay = 8000;
    }
    if (delay > 0) {
        lv_timer_t *timer = lv_timer_create(finish_timer_cb, (uint32_t)delay, NULL);
        if (timer != NULL) {
            lv_timer_set_repeat_count(timer, 1);
            s_auto_close_timer = timer;
        }
    }
}

/* 进度回调：在同步任务上下文执行，更新 UI 必须持 LVGL 锁 */
static void sync_progress_cb(wifi_time_sync_stage_t stage, int attempt,
                             int max_attempts, int percent, const char *detail,
                             void *ctx)
{
    (void)ctx;
    if (!lvgl_port_lock(-1)) {
        return;
    }
    if (s_root != NULL && s_visible && !s_finished) {
        char buf[160];
        if (max_attempts > 1 && attempt > 0) {
            snprintf(buf, sizeof(buf), "%s（第 %d/%d 次）",
                     wifi_time_sync_stage_text(stage), attempt, max_attempts);
        } else {
            snprintf(buf, sizeof(buf), "%s", wifi_time_sync_stage_text(stage));
        }
        lv_label_set_text(s_status, buf);
        lv_label_set_text(s_detail, (detail != NULL) ? detail : "");
        lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);
        char pb[8];
        snprintf(pb, sizeof(pb), "%d%%", percent);
        lv_label_set_text(s_percent, pb);
    }
    lvgl_port_unlock();
}

/* 同步任务（内部 RAM 栈）：阻塞跑完整流程，结束后回 LVGL 上下文收尾 */
static void sync_task(void *arg)
{
    (void)arg;
    wifi_time_sync_result_t result =
        wifi_time_sync_run(sync_progress_cb, NULL, &s_cancel);

    if (lvgl_port_lock(-1)) {
        sync_ui_finish(result);
        lvgl_port_unlock();
    } else {
        s_running = false;
        s_finished = true;
    }
    vTaskDelete(NULL);
}

void time_sync_ui_set_done_cb(time_sync_ui_done_cb_t cb)
{
    s_done_cb = cb;
}

void time_sync_ui_dismiss(void)
{
    if (s_root == NULL) {
        return;
    }
    /* 复用关闭路径：隐藏全屏层、清掉自动关闭定时器与取消标志、恢复息屏计时。
       ⚠ 不取消正在跑的任务——本函数只在"结果已经出来、回调方要显示自己的
       界面"时调用（里世界的对时回调）。 */
    sync_ui_close();
    ESP_LOGI(TAG, "sync ui dismissed by caller");
}

bool time_sync_ui_start(void)
{
    if (s_visible || s_running) {
        return true;   /* 已在跑/已显示：当作成功（不会重复起任务） */
    }
    if (s_root == NULL) {
        sync_ui_create();
    }

    /* 重置界面（顺带取消上一次残留的自动关闭定时器） */
    if (s_auto_close_timer != NULL) {
        lv_timer_del(s_auto_close_timer);
        s_auto_close_timer = NULL;
    }
    s_cancel = false;
    s_finished = false;
    s_running = true;
    s_visible = true;
    lv_obj_set_style_text_color(s_status, TS_TEXT, 0);
    lv_label_set_text(s_status, "准备对时…");
    lv_label_set_text(s_detail, "");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, TS_ACCENT, LV_PART_INDICATOR);
    lv_label_set_text(s_percent, "0%");
    lv_label_set_text(s_btn_label, "取消");
    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_move_foreground(s_root);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* 抑制自动息屏：整个流程最长可达一分多钟，中途黑屏会看不到进度/取消 */
    hold_awake(true);

    BaseType_t ok = xTaskCreatePinnedToCore(sync_task, "time_sync",
                                            TS_TASK_STACK, NULL, TS_TASK_PRIO,
                                            NULL, TS_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create time sync task failed");
        s_running = false;
        s_finished = true;
        lv_obj_set_style_text_color(s_status, TS_ERROR, 0);
        lv_label_set_text(s_status, "内存不足，无法开始对时");
        lv_label_set_text(s_detail, "请重启设备后重试");
        lv_label_set_text(s_btn_label, "关闭");
        hold_awake(false);
        ui_toast_show("内存不足，无法开始对时");
        return false;
    }
    ESP_LOGI(TAG, "time sync started");
    return true;
}

bool time_sync_ui_is_visible(void)
{
    return s_visible;
}

void time_sync_ui_cancel(void)
{
    if (!s_visible || s_finished) {
        return;
    }
    cancel_btn_cb(NULL);
}
