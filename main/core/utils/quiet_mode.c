#include "core/utils/quiet_mode.h"

#include <string.h>

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/bt_audio.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/axp2101.h"
#include "core/hardware/battery.h"
#include "core/hardware/sd_card.h"
#include "core/storage/bt_store.h"
#include "core/hardware/bt/bt_service.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/power_service.h"
#include "core/utils/task_psram.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "quiet_mode";

/* 最长持续时间：12 小时（超过一定断电，防止整晚/整天留在伪关机态） */
#define QUIET_MAX_MS        (12u * 60u * 60u * 1000u)

/* 电池保护阈值（%）：低于此值一定断电（充电中/VBUS 在位不触发） */
#define QUIET_BAT_MIN_PCT   15

/* 监控周期（ms）：链路断开/电量检查；2 秒接近"立即"，几乎不耗电 */
#define QUIET_CHECK_MS      2000

/* 监控任务栈（PSRAM）：只读 I2C + 写 PMU，不碰 flash */
#define QUIET_TASK_STACK    (3 * 1024)
#define QUIET_TASK_PRIO     3
#define QUIET_TASK_CORE     1

static volatile bool s_active = false;
static volatile int64_t s_enter_us = 0;
static TaskHandle_t s_monitor = NULL;
static volatile bool s_powering_off = false;

/* PMU 真断电；失败则继续监控（下次循环重试） */
static void quiet_power_off(const char *reason)
{
    if (s_powering_off) {
        return;
    }
    s_powering_off = true;
    ESP_LOGW(TAG, "quiet mode: %s -> PMU power off", reason);
    esp_err_t err = axp2101_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMU power off failed: %s", esp_err_to_name(err));
        s_powering_off = false;
    }
    /* PMU 正常会在几十 ms 内断电；存活则等下一轮重试 */
}

/* 监控任务：链路断开 / 12h / 低电 → 断电 */
static void quiet_monitor_task(void *arg)
{
    (void)arg;
    battery_info_t binfo;
    int64_t last_log_us = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(QUIET_CHECK_MS));

        /* 1. 蓝牙断开：立即断电（避免耳机断连提示音/闪灯持续） */
        if (!bt_service_is_connected()) {
            quiet_power_off("bluetooth link lost");
            continue;
        }

        /* 2. 12 小时上限 */
        int64_t elapsed_us = esp_timer_get_time() - s_enter_us;
        if (elapsed_us >= (int64_t)QUIET_MAX_MS * 1000) {
            quiet_power_off("12 hour limit reached");
            continue;
        }

        /* 3. 电池 < 15% 且未充电 → 必须断电（过放保护优先） */
        if (battery_read_info(&binfo) == ESP_OK && binfo.available &&
            binfo.percent != 0xFF && binfo.percent < QUIET_BAT_MIN_PCT &&
            !binfo.charging && !binfo.vbus_present) {
            ESP_LOGW(TAG, "quiet mode: battery %u%% < %d%%, power off",
                     (unsigned)binfo.percent, QUIET_BAT_MIN_PCT);
            quiet_power_off("battery low");
            continue;
        }

        /* 每条 10 分钟打印一次存活信息（串口方便排查） */
        if (elapsed_us - last_log_us >= 10LL * 60 * 1000000) {
            last_log_us = elapsed_us;
            ESP_LOGI(TAG, "quiet mode alive: %d min, battery %u%%, bt %s",
                     (int)(elapsed_us / 60000000),
                     (unsigned)(binfo.available ? binfo.percent : 0),
                     bt_service_is_connected() ? "connected" : "lost");
        }
    }
}

/* 进入安静模式的重活（deep_worker 内部栈上下文，可以写 flash） */
static void quiet_enter_run(void *arg)
{
    (void)arg;

    /* 1. 停播放并等解码任务退出（睡眠定时器已渐降，这里确保 I2S/DAC 静止） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        for (int waited = 0;
             waited < 1000 && mp3_player_get_state() != MP3_STATE_IDLE;
             waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* 2. 只挂起 A2DP 媒体流，**不断开蓝牙链路**（这是安静模式的核心） */
    bt_audio_request_stream(false);

    /* 3. 优雅关断 DAC/耳放（省电、防爆音）；蓝牙链路不受影响 */
    audio_shutdown();

    /* 4. 保存所有延迟落盘的配置 */
    bt_store_flush_pending(true);

    /* 5. 卸载 SD（释放 SDMMC，降低待机电流；重启时会重新挂载） */
    sd_card_deinit();

    /* 6. 电源服务进入"安静"模式：固定 80MHz，跳过低电/自动息屏策略
          （低电/超时由本模块的监控任务接管） */
    power_service_set_quiet_mode(true);

    s_enter_us = esp_timer_get_time();
    s_active = true;

    /* 7. 启动监控任务（PSRAM 栈，失败则降级到内部 RAM） */
    if (s_monitor == NULL) {
        if (task_create_psram_stack(quiet_monitor_task, "quiet_mon",
                                    QUIET_TASK_STACK, NULL,
                                    QUIET_TASK_PRIO, &s_monitor,
                                    QUIET_TASK_CORE) != pdPASS) {
            ESP_LOGE(TAG, "create quiet monitor task failed");
        }
    }

    ESP_LOGW(TAG, "=== QUIET MODE ACTIVE (BT kept alive, screen off) ===");
}

void quiet_mode_init(void)
{
    /* 无资源需要预先分配：进入时再创建监控任务 */
}

bool quiet_mode_try_enter(void)
{
    if (s_active) {
        return true;
    }
    if (!bt_service_is_connected()) {
        return false;
    }
    if (!bt_service_connected_quiet_mode()) {
        ESP_LOGI(TAG, "connected device has quiet mode disabled, normal shutdown");
        return false;
    }

    ESP_LOGW(TAG, "entering quiet mode (sleep timer + BT device switch on)");
    s_active = true;   /* 先占位，防止重复进入 */

    /* 息屏：power_service_screen_off 内部投递 deep_worker（不能与后面的
       deep_worker_call 嵌套，故分两次顺序调用） */
    power_service_screen_off();

    /* 其余重活：停播放/关 DAC/卸 SD/启监控 */
    if (!deep_worker_call(quiet_enter_run, NULL)) {
        ESP_LOGW(TAG, "deep_worker unavailable, run quiet chain inline");
        quiet_enter_run(NULL);
    }
    return true;
}

bool quiet_mode_is_active(void)
{
    return s_active;
}

void quiet_mode_request_reboot(void)
{
    ESP_LOGW(TAG, "quiet mode: power key long press -> reboot");
    /* 内部 RAM 栈（button_svc）可以写 flash：把待落盘配置写完再重启 */
    bt_store_flush_pending(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
