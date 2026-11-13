#include "core/hardware/button_service.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* ESP32-S31 / FakePodNano Pro V1：唯一独立按键（下方按键）。
   来源：ideas_and_exps/reference_docs/hardware_v1.md §5。
   按键接 GND，低电平按下；启用内部上拉（文档说明可配置上拉）。 */
#define BUTTON_PIN            GPIO_NUM_61

/* 长按阈值（ms）：600ms（沿用 S3 上 B 键的手感，亮屏=音量-10 / 息屏=上一曲）。
   ⚠ S3 的 A 键（电源键，3 秒硬断电窗口）已由 AXP2101 接管，不再有
   "阈值必须明显小于 3 秒"的约束，这里只保留单键的日常手感。 */
#define BUTTON_LONG_PRESS_MS  600

/* 轮询周期（ms） */
#define BUTTON_POLL_MS        5
/* 软件去抖：连续稳定采样次数达到该值才确认电平变化（总延迟 <= 轮询周期*次数） */
#define BUTTON_DEBOUNCE_SAMPLES 3

/* 轮询任务参数：栈 4KB，执行按键分发回调（key_handler 里只有音量/播放控制，
   深链已委托 deep_worker），固定 Core0 不影响 Core1 音频解码。 */
#define BUTTON_TASK_STACK   (4 * 1024)
#define BUTTON_TASK_PRIO    4
#define BUTTON_TASK_CORE    0

/* 按键运行状态 */
static bool     s_raw_level = true;   /* 最近一次原始采样（true=高=抬起） */
static uint8_t  s_stable_cnt = 0;     /* 当前原始电平已连续稳定的采样次数 */
static bool     s_level = true;       /* 去抖后的稳定电平（true=高=抬起） */
static bool     s_pressed = false;    /* 当前是否处于按下状态 */
static bool     s_initial_hold = false;   /* 开机时已被按住（首次抬起前不触发长按） */
static bool     s_long_reported = false;  /* 本次按压是否已上报过长按 */
static int64_t  s_press_start_us = 0;     /* 本次按下开始时刻（esp_timer，us） */

/* 用户事件回调（可选，默认只打印日志） */
static button_event_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

/* 打印日志并转发用户回调 */
static void button_report(button_event_t event)
{
    switch (event) {
    case BUTTON_EVENT_SHORT:
        ESP_LOGI(TAG, "key (GPIO%d) short press", BUTTON_PIN);
        break;
    case BUTTON_EVENT_LONG:
        ESP_LOGI(TAG, "key (GPIO%d) long press (%u ms)", BUTTON_PIN,
                 (unsigned)BUTTON_LONG_PRESS_MS);
        break;
    default:
        break;
    }

    if (s_cb != NULL) {
        s_cb(event, s_cb_ctx);
    }
}

/* 轮询扫描：去抖 + 长按判定 + 单击判定 */
static void button_scan(void)
{
    const bool raw = gpio_get_level(BUTTON_PIN) == 1;   /* 高电平 = 抬起 */

    /* 软件去抖：同一原始电平需连续稳定采样多次才生效 */
    if (raw == s_raw_level) {
        if (s_stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
            s_stable_cnt++;
        }
    } else {
        s_raw_level = raw;
        s_stable_cnt = 0;
    }
    if (s_stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
        return;   /* 仍处于抖动中，忽略本次采样 */
    }
    if (s_level == raw) {
        /* 去抖后的电平未变化：若处于按住状态且已达长按阈值，上报一次长按。
           ⚠ 用按下时刻取定的阈值语义：当前只有固定 600ms，直接比较。 */
        if (s_pressed && !s_long_reported && !s_initial_hold
            && esp_timer_get_time() - s_press_start_us
               >= (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
            s_long_reported = true;
            button_report(BUTTON_EVENT_LONG);
        }
        return;
    }
    s_level = raw;

    if (!raw) {
        /* ---- 按下（低电平） ---- */
        s_pressed = true;
        s_long_reported = false;
        if (s_initial_hold) {
            /* 开机初始按压（长按开机/唤醒后未及时松手）：不启动长按计时，
               等首次抬起后恢复正常检测 */
            s_press_start_us = 0;
        } else {
            s_press_start_us = esp_timer_get_time();
        }
    } else {
        /* ---- 抬起 ---- */
        s_pressed = false;
        s_initial_hold = false;   /* 首次抬起后解除开机按压抑制 */
        const bool was_long = s_long_reported;
        s_long_reported = false;
        if (!was_long) {
            button_report(BUTTON_EVENT_SHORT);
        }
    }
}

/* 按键轮询任务 */
static void button_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        button_scan();
    }
}

void button_service_init(void)
{
    /* GPIO 配置：输入 + 内部上拉，按键接 GND */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 以当前实际电平为去抖基线，但确认电平初始视为"抬起"：
       若开机时按键已被按住，去抖后仍能正确触发按下事件 */
    s_raw_level = gpio_get_level(BUTTON_PIN) == 1;
    s_stable_cnt = 0;
    s_level = true;
    s_pressed = false;
    s_initial_hold = (gpio_get_level(BUTTON_PIN) == 0);
    s_long_reported = false;
    s_press_start_us = 0;

    xTaskCreatePinnedToCore(button_task, "button_svc",
                            BUTTON_TASK_STACK, NULL,
                            BUTTON_TASK_PRIO, NULL, BUTTON_TASK_CORE);

    ESP_LOGI(TAG, "Button service started: key=GPIO%d (internal pullup, active low)",
             BUTTON_PIN);
}

void button_service_register_cb(button_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
