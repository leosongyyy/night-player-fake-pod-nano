#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/battery.h"
#include "core/hardware/button_service.h"
#include "core/hardware/display.h"
#include "core/hardware/pldata.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/performance_monitor.h"
#include "ui/boot_flow.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/* 按键事件回调（按钮任务上下文）                                        */
/*                                                                     */
/* 唤醒源：B 键 GPIO0（RTC GPIO，外部 10k 上拉），按下为低电平唤醒。      */
/* A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒（见 fake_shutdown）。*/
/* ⚠ GPIO0 是 strapping 引脚：若唤醒后仍按住会进入下载模式。             */
/* ------------------------------------------------------------------ */

/* 唤醒防循环：deepsleep 唤醒 = 芯片重启，若唤醒时按键仍被按住，boot 后
   button_service 去抖仍会报告双按 → 立即再次休眠。开机 1.2 秒内忽略双按。 */
#define DEEP_SLEEP_IGNORE_MS  1200

/* 按键事件回调（按钮任务上下文）：
   双键同时按下 → 进入伪关机（关尽外设 + deepsleep） */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;
    if (event != BUTTON_EVENT_BOTH) {
        return;
    }
    /* 防循环：刚开机/唤醒 1.2 秒内忽略双按（唤醒时用户可能仍按着按键） */
    if (esp_timer_get_time() < (int64_t)DEEP_SLEEP_IGNORE_MS * 1000) {
        ESP_LOGI(TAG, "both keys ignored within %d ms after boot", DEEP_SLEEP_IGNORE_MS);
        return;
    }
    fake_shutdown_enter();
}

/* 触摸初始化完成回调（触摸异步初始化任务上下文）：
   把就绪的触摸句柄交给 LVGL 输入设备，触摸输入即刻生效 */
static void on_touch_ready(esp_lcd_touch_handle_t tp, void *ctx)
{
    (void)ctx;
    lvgl_port_set_touch_handle(tp);
    ESP_LOGI(TAG, "Touch ready, LVGL touch input enabled");
}

void app_main(void)
{
    // 检查唤醒原因（deep sleep 调试：确认是否为按键唤醒，排查误唤醒）
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "boot: cold start (not from deep sleep)");
    } else if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "boot: woken by EXT1 (B key press)");
    } else {
        ESP_LOGW(TAG, "boot: woken by unexpected source %d", (int)wakeup_cause);
    }

    // 硬件初始化
    ESP_LOGI(TAG, "Initialize display");
    esp_lcd_panel_handle_t panel_handle = display_init();

    ESP_LOGI(TAG, "Initialize touch");
    esp_lcd_touch_handle_t tp = touch_init();   // 异步初始化：立即返回（就绪前为 NULL）
    /* 触摸就绪回调：把句柄交给 LVGL 输入设备（就绪前触摸回调不给数据） */
    touch_register_ready_cb(on_touch_ready, NULL);

    ESP_LOGI(TAG, "Initialize battery ADC");
    battery_init();

    ESP_LOGI(TAG, "Initialize audio (I2S + CS43131)");
    audio_init();

    ESP_LOGI(TAG, "Initialize pldata partition (播放列表数据)");
    if (pldata_init() != ESP_OK) {
        ESP_LOGW(TAG, "pldata partition unavailable (播放列表持久化不可用)");
    }

    ESP_LOGI(TAG, "Initialize button service (A=GPIO48, B=GPIO0)");
    button_service_init();
    /* 双键同时按下 → 深度休眠（见 on_button_event） */
    button_service_register_cb(on_button_event, NULL);

    ESP_LOGI(TAG, "Initialize SD card (SDMMC)");
    sd_card_init();   // 挂载失败不阻塞，由 boot_flow 依据 sd_card_is_mounted() 决定流程

    // LVGL 移植层（显示驱动注册 + tick + LVGL 任务 + 触摸输入设备）
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_init(panel_handle, tp);

    // 开机流程：电池电压 / SD 卡 / 播放列表数据检查，决定进入哪个界面。
    // （LVGL API 非线程安全，需持锁操作；本函数只创建首屏并启动后台任务，不阻塞）
    ESP_LOGI(TAG, "Run boot flow");
    if (lvgl_port_lock(-1)) {
        boot_flow_run();
        // 强制同步渲染首帧（此时屏幕仍为全黑亮度 0），确保点亮前首帧已
        // 提交到屏幕，避免出现"点亮后空白/半帧刷新"的窗口
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }

    // 点亮屏幕：display_init() 以亮度 0（全黑）初始化避免开机白屏闪烁，
    // LVGL 首帧渲染完成后再恢复默认亮度
    ESP_LOGI(TAG, "Turn on display (restore brightness %d)", DISPLAY_BRIGHTNESS_DEFAULT);
    display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);

    ESP_LOGI(TAG, "App started");

    // 启动性能监视（内存 + 双核 CPU 使用率）
    startPerformanceMonitor();
}
