#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/battery.h"
#include "core/hardware/button_service.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/performance_monitor.h"
#include "ui/app.h"
#include "ui/main_ui.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/* 深度休眠测试（双键同时按下进入，测"能关尽关"的耗电）                   */
/*                                                                     */
/* 唤醒源：B 键 GPIO0（RTC GPIO，外部 10k 上拉），按下为低电平唤醒。      */
/* A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒（见下）。         */
/* ⚠ GPIO0 是 strapping 引脚：若唤醒后仍按住会进入下载模式。             */
/* ------------------------------------------------------------------ */

/* 唤醒防循环：deepsleep 唤醒 = 芯片重启，若唤醒时按键仍被按住，boot 后
   button_service 去抖仍会报告双按 → 立即再次休眠。开机 1.2 秒内忽略双按。 */
#define DEEP_SLEEP_IGNORE_MS  1200

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "=== both keys pressed: enter deep sleep (power test) ===");

    /* 1. 停止播放并等待解码任务退出（释放文件句柄与 I2S） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_IDLE && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
    }

    /* 2. 挂起 LVGL 任务（停止刷屏与触摸轮询，避免后面删除 I2C 驱动后报错） */
    lvgl_port_suspend();

    /* 3. 复位 CS43131（GPIO41 拉低硬件复位：全部电源域含晶振一起掉电，
           最彻底关机且状态确定；唤醒后 audio_init → cs43131_init 会重新
           走完整复位+配置流程恢复） */
    cs43131_reset_for_sleep();

    /* 4. 关闭显示屏（AMOLED 显示关断） */
    display_power_off();

    /* 5. 关闭触摸控制器并释放 I2C 总线驱动 */
    touch_power_off();

    /* 6. 卸载 SD 卡并释放 SDMMC 主机 */
    sd_card_deinit();

    /* 7. 配置 deep sleep 唤醒：B 键（GPIO0，RTC GPIO）按下（低电平）唤醒。
       关键约束（ESP32-S3）：
       - deep sleep GPIO 唤醒只支持 RTC GPIO（GPIO0~21，SOC_RTCIO_PIN_COUNT=22），
         A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒；
       - esp_sleep_enable_gpio_wakeup 仅支持 light sleep（IDF 文档明确），
         此前误用导致进入 deepsleep 后快速误唤醒；
       - RTC 外设关闭时内部上拉不工作，需外部电阻——GPIO0 的板载 10k 上拉
         正好满足，平时高电平、按下拉低触发唤醒。 */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW));

    ESP_LOGI(TAG, "deep sleep: display/touch/DAC off, press B key (GPIO0) to wake");
    vTaskDelay(pdMS_TO_TICKS(100));   /* 等待日志刷出 */
    esp_deep_sleep_start();
}

/* 按键事件回调（按钮任务上下文）：
   双键同时按下 → 进入深度休眠（测试"能关尽关"的耗电） */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;
    if (event != BUTTON_EVENT_BOTH) {
        return;
    }
    /* 防循环：刚开机/唤醒 3 秒内忽略双按（唤醒时用户可能仍按着按键） */
    if (esp_timer_get_time() < (int64_t)DEEP_SLEEP_IGNORE_MS * 1000) {
        ESP_LOGI(TAG, "both keys ignored within %d ms after boot", DEEP_SLEEP_IGNORE_MS);
        return;
    }
    enter_deep_sleep();
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

    ESP_LOGI(TAG, "Initialize button service (A=GPIO48, B=GPIO0)");
    button_service_init();
    /* 双键同时按下 → 深度休眠测试（见 on_button_event） */
    button_service_register_cb(on_button_event, NULL);

    ESP_LOGI(TAG, "Initialize SD card (SDMMC)");
    if (sd_card_init() == ESP_OK) {
        /* 开机自动播放当前曲目（44.1kHz，Core1 后台任务，不阻塞 UI 初始化） */
        mp3_player_play(main_ui_get_current_track());
    }

    // LVGL 移植层（显示驱动注册 + tick + LVGL 任务 + 触摸输入设备）
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_init(panel_handle, tp);

    // 创建 UI（LVGL API 非线程安全，需持锁操作）
    ESP_LOGI(TAG, "Create UI");
    if (lvgl_port_lock(-1)) {
        app_ui_create();
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
