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
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/battery_logger.h"
#include "core/utils/deep_worker.h"
#include "core/utils/performance_monitor.h"
#include "core/utils/power_service.h"
#include "core/utils/sleep_timer.h"
#include "ui/boot_flow.h"
#include "ui/key_handler.h"

static const char *TAG = "app_main";

/* ------------------------------------------------------------------ */
/* 按键事件：亮屏/息屏两套映射由 ui/key_handler 统一分发                 */
/* （亮屏：A=音量+ B=音量- A长按=伪关机 B长按=音量-10 双按=息屏；          */
/*   息屏：A=播放/暂停 B=下一曲 B长按=上一曲 A长按=亮屏+伪关机 双按=亮屏）  */
/* ------------------------------------------------------------------ */

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

    // 用户配置存储（NVS）：尽早初始化，开机恢复音量/亮度/循环模式等
    ESP_LOGI(TAG, "Initialize config store (NVS)");
    config_store_init();

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

    /* 应用用户音量配置（NVS 持久化，档位 0~30；audio_init 内部先按默认档初始化 DAC） */
    audio_set_volume_level(config_store_get_volume());

    ESP_LOGI(TAG, "Initialize pldata partition (播放列表数据)");
    if (pldata_init() != ESP_OK) {
        ESP_LOGW(TAG, "pldata partition unavailable (播放列表持久化不可用)");
    }

    ESP_LOGI(TAG, "Initialize button service (A=GPIO48, B=GPIO0)");
    button_service_init();
    /* 按键分发（亮屏/息屏两套映射）在 LVGL 初始化后统一注册（见下） */

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

        // 完整点亮（恢复用户配置亮度，默认 255）：与首帧渲染在同一个 LVGL
        // 锁内连续执行。⚠ 亮度命令与 LVGL 刷屏共用 SPI 总线，必须在持锁时
        // 调用；锁外执行会被更高优先级的 LVGL 任务抢占并发刷屏，导致画面
        // 定格/花屏。
        uint8_t boot_brightness = config_store_get_brightness();
        ESP_LOGI(TAG, "Turn on display (restore brightness %d)", boot_brightness);
        display_set_brightness(boot_brightness);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "App started");

    // 深路径执行器（常驻大栈任务，Core0）：息屏/亮屏/伪关机的深调用链
    // （esp_pm_configure + AMOLED 重初始化 + lv_refr_now 全屏重刷 + 停外设）
    // 集中到该任务执行，button/power_service 等业务任务只做浅投递（见
    // core/utils/deep_worker.h）。必须在任何投递方（睡眠定时器/息屏/按键）
    // 可能动作之前初始化。
    deep_worker_init();

    // 睡眠定时器（默认关闭，由播放设置页开启；到点自动伪关机）
    sleep_timer_init();

    // 开机默认开启睡眠定时（config_store 的 sleep_def_on，默认关）：
    // 开启时用默认时长（sleep_default_min，默认 45 分钟）自动启动定时
    if (config_store_get_sleep_def_on()) {
        uint32_t def_min = config_store_get_sleep_default_min();
        if (def_min > 0) {
            sleep_timer_set(def_min);
            ESP_LOGI(TAG, "boot: sleep timer auto started (%u min)",
                     (unsigned)def_min);
        }
    }

    // 按键分发（亮屏：音量±/伪关机/息屏；息屏：播放控制/亮屏）。
    // 放在 LVGL 初始化之后，注册 button_service 事件回调。
    key_handler_init();

    // 电源服务（power_service）：esp_pm 固定频率（亮屏 240MHz/息屏 80MHz）+
    // 触摸活动订阅 + 无操作超时自动息屏（默认 30 秒，设置页可调）+ 自动关机
    // 策略（息屏暂停超时 / 电池电压持续过低，见 Kconfig）。需在 LVGL 之后。
    power_service_init();

    // 电池电压采样日志（开发者选项：设置 → 关于 → 开发者选项，NVS 运行时
    // 开关，默认关）：开启时独立任务按所选间隔（默认 5 分钟）把
    // "运行时长 + 平均电压"写入 SD 卡；此处读取开机时的 NVS 值决定是否建
    // 任务（修改需重启生效）。需在 sd_card_init 之后（SD 未挂载时任务自行跳过）。
    battery_logger_init();

    // 启动性能监视（内存 + 双核 CPU 使用率），Kconfig CONFIG_PERFORMANCE_MONITOR 控制
#ifdef CONFIG_PERFORMANCE_MONITOR
    startPerformanceMonitor();
#endif
}
