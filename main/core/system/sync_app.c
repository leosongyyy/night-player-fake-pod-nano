#include "core/system/sync_app.h"

#include <stdio.h>

#include "core/fonts/system_font.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/network/wifi_time_sync.h"
#include "core/storage/config_store.h"
#include "core/system/fw_switch.h"
#include "core/ui/lvgl_port.h"
#include "ui/time_sync_ui.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "sync_app";

/* 对时界面的最低亮度：用户配置的亮度可能很低（最低 16），对时进度看不清就等于
   没有反馈。只抬亮这一屏，**不改**用户配置（回播放器后仍是原亮度）。 */
#define SYNC_UI_MIN_BRIGHTNESS  96

/* ------------------------------------------------------------------ */
/* 触摸就绪回调（触摸异步初始化任务上下文）                              */
/* ------------------------------------------------------------------ */

static void on_touch_ready(esp_lcd_touch_handle_t tp, void *ctx)
{
    (void)ctx;
    lvgl_port_set_touch_handle(tp);
    ESP_LOGI(TAG, "touch ready（可点「取消」/「关闭」）");
}

/* ------------------------------------------------------------------ */
/* 对时结束：写结果 → 重启回播放器                                       */
/* ------------------------------------------------------------------ */

/* 在 LVGL 任务上下文（持锁）里执行一次：把结果文本交给 NVS，然后重启。
   ⚠ 这里**不**做优雅关机（不停音频/不卸 SD）：对时固件本来就没开这些。 */
static void sync_done(wifi_time_sync_result_t result)
{
    char msg[FW_SYNC_RESULT_MAX];

    if (result == WIFI_TIME_SYNC_OK) {
        /* 带上新的"时:分"，播放器开机 Toast 一眼就能看出对没对上 */
        char ts[16] = "";
        rtc_time_t rt;
        if (rtc_is_ready() && rtc_get_time(&rt) == ESP_OK) {
            snprintf(ts, sizeof(ts), " %02u:%02u", (unsigned)rt.hour,
                     (unsigned)rt.minute);
        }
        snprintf(msg, sizeof(msg), "对时成功%s", ts);
    } else if (result == WIFI_TIME_SYNC_CANCELLED) {
        snprintf(msg, sizeof(msg), "对时已取消");
    } else {
        snprintf(msg, sizeof(msg), "对时失败：%s",
                 wifi_time_sync_result_text(result));
    }
    fw_sync_result_set_text(msg);

    ESP_LOGI(TAG, "对时结束（result=%d），重启回播放器（factory）", (int)result);
    /* 留一点时间让日志出串口、NVS 落盘；面板保持当前画面直到播放器重新初始化 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void sync_app_run(void)
{
    ESP_LOGI(TAG, "=== 对时固件启动：role=%s, running partition=%s ===",
             fw_role_name(), fw_running_partition_label());
    if (!fw_role_is_sync()) {
        /* 构建配置串了（CONFIG_FIRMWARE_ROLE_SYNC 没打开）：不要在这里继续跑，
           否则会变成"没有播放器的固件"。 */
        ESP_LOGE(TAG, "不是对时固件构建（CONFIG_FIRMWARE_ROLE_SYNC=n），回播放器");
        fw_switch_to_player();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    /* ① 第一件事：把下次启动改回播放器（= 擦 otadata）。
          ⚠ 必须早于任何可能耗时/失败的初始化：之后崩溃、断电、看门狗复位、
            镜像后续损坏，设备都回播放器，绝不会卡在这个固件里反复重启。 */
    esp_err_t err = fw_switch_to_player();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "回切播放器失败（%s）：仍继续对时，但下次启动可能又进本固件",
                 esp_err_to_name(err));
    }

    /* ② 最小初始化（顺序：config_store → display → LVGL → 字体 → 触摸 →
          PMU/RTC（都挂 I²C0，总线由 touch_init 建立）→ SD） */
    ESP_LOGI(TAG, "init: config_store");
    config_store_init();

    ESP_LOGI(TAG, "init: display + lvgl port");
    esp_lcd_panel_handle_t panel = display_init();
    lvgl_port_init(panel, NULL);

    ESP_LOGI(TAG, "init: system font");
    system_font_init();

    ESP_LOGI(TAG, "init: touch（异步）");
    touch_init();
    touch_register_ready_cb(on_touch_ready, NULL);

    ESP_LOGI(TAG, "init: PMU / RTC / SD");
    battery_init();   /* AXP2101：电池与充电状态（限充配置由 PMU 自己保持） */
    rtc_init();       /* PCF85063A：对时结果写这里 */
    sd_card_init();   /* /sdcard/.ecov/wifi.txt 在这里；没卡时界面会提示 */

    /* ③ 点亮屏幕：先渲染"黑底 + 对时界面"，再给亮度（与播放器开机同一套
          防白屏顺序：面板此刻亮度仍是 0）。 */
    uint8_t brightness = config_store_get_brightness();
    if (brightness < SYNC_UI_MIN_BRIGHTNESS) {
        brightness = SYNC_UI_MIN_BRIGHTNESS;
    }

    bool started = false;
    if (lvgl_port_lock(-1)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        time_sync_ui_set_done_cb(sync_done);
        started = time_sync_ui_start();
        lv_refr_now(NULL);

        ESP_LOGI(TAG, "turn on display (brightness %u)", (unsigned)brightness);
        display_set_brightness(brightness);
        lvgl_port_unlock();
    }

    if (!started) {
        /* 对时任务没起来（内存不足等）：别把设备留在这个固件里 */
        ESP_LOGE(TAG, "对时界面未能启动，3 秒后回播放器");
        fw_sync_result_set_text("对时失败：内存不足");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return;
    }

    /* ④ app_main 到此返回：对时任务在 Core1 跑，结束时会由 sync_done() 重启。
          LVGL 任务继续渲染进度条。 */
    ESP_LOGI(TAG, "对时固件就绪：等待 WiFi/SNTP 结果…");
}
