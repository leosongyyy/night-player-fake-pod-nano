#include "core/utils/sleep_timer.h"

#include "core/utils/fake_shutdown.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sleep_timer";

/* 任务参数：Core1 低优先级后台任务。到点触发伪关机（内部会等待播放停止
   并关外设），放独立任务里不影响 LVGL/UI 线程。 */
#define SLEEP_CHECK_MS   10000   /* 每 10s 检查一次到期时间，精度足够 */
#define SLEEP_TASK_STACK 2048
#define SLEEP_TASK_PRIO  4
#define SLEEP_TASK_CORE  1

/* 当前定时状态：s_minutes=0 表示关闭；s_deadline_us 为绝对到期时刻。
   仅设置页（LVGL 任务）写入、本任务读取，volatile 保证可见性。 */
static volatile uint32_t s_minutes = 0;
static volatile int64_t  s_deadline_us = 0;

/* 倒计时任务：到点调用伪关机（不返回） */
static void sleep_timer_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SLEEP_CHECK_MS));
        if (s_minutes == 0) {
            continue;
        }
        if (esp_timer_get_time() >= s_deadline_us) {
            ESP_LOGI(TAG, "sleep timer expired (%u min), enter fake shutdown",
                     (unsigned)s_minutes);
            s_minutes = 0;
            fake_shutdown_enter();   /* 进入深度休眠，不返回 */
        }
    }
}

void sleep_timer_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    BaseType_t ret = xTaskCreatePinnedToCore(sleep_timer_task, "sleep_timer",
                                             SLEEP_TASK_STACK, NULL,
                                             SLEEP_TASK_PRIO, NULL,
                                             SLEEP_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create sleep timer task failed");
        return;
    }
    inited = true;
    ESP_LOGI(TAG, "sleep timer ready (off by default)");
}

void sleep_timer_set(uint32_t minutes)
{
    if (minutes == 0) {
        s_minutes = 0;
        ESP_LOGI(TAG, "sleep timer cancelled");
        return;
    }
    s_minutes = minutes;
    s_deadline_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000LL;
    ESP_LOGI(TAG, "sleep timer set: %u min", (unsigned)minutes);
}

uint32_t sleep_timer_get_minutes(void)
{
    return s_minutes;
}

uint32_t sleep_timer_get_remaining_s(void)
{
    if (s_minutes == 0) {
        return 0;
    }
    int64_t remain = s_deadline_us - esp_timer_get_time();
    if (remain <= 0) {
        return 0;   /* 已到点（任务即将触发关机），UI 显示 0 即可 */
    }
    /* 向上取整到秒，避免显示 14:00 时实际只剩 13:59.9 的误导 */
    return (uint32_t)((remain + 999999) / 1000000);
}

bool sleep_timer_is_active(void)
{
    return s_minutes > 0;
}
