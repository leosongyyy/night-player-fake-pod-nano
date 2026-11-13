#include "core/hardware/button_service.h"

#include "core/hardware/axp2101.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* ESP32-S31 / FakePodNano Pro V1：下方独立按键（下键）。
   接 GND，低电平按下；启用内部上拉（文档说明可配置上拉）。
   上键（电源键）不在主控 GPIO 上，由 AXP2101 提供"按住"状态。 */
#define BUTTON_DOWN_PIN       GPIO_NUM_61

/* 上键长按默认阈值（ms）：实际由 key_handler 注册的 provider 按屏幕状态给
   （亮屏 1200 / 息屏 900，与 S3 一致）；本值只在 provider 未注册时兜底。 */
#define BUTTON_UP_LONG_PRESS_MS    1200
/* 下键长按阈值（ms）：亮屏=音量-10、息屏=上一曲，600ms 响应较快 */
#define BUTTON_DOWN_LONG_PRESS_MS  600

/* 轮询周期（ms） */
#define BUTTON_POLL_MS        5
/* 软件去抖：连续稳定采样次数达到该值才确认电平变化（总延迟 <= 轮询周期*次数） */
#define BUTTON_DEBOUNCE_SAMPLES 3

/* 轮询任务参数：栈 4KB（执行按键分发回调；深链已委托 deep_worker），
   Core0、优先级 4，不影响 Core1 音频。 */
#define BUTTON_TASK_STACK   (4 * 1024)
#define BUTTON_TASK_PRIO    4
#define BUTTON_TASK_CORE    0

/* 按键索引 */
enum {
    BTN_IDX_UP = 0,     /* 上键 = PMU PWRON */
    BTN_IDX_DOWN,       /* 下键 = GPIO61 */
    BTN_IDX_MAX,
};

/* 单个按键的运行状态（与 S3 的 button_service 同一套算法） */
typedef struct {
    uint32_t long_press_ms;        /* 本键默认长按阈值 */
    uint32_t active_long_press_ms; /* 本次按压实际使用的阈值（按下时刻取定） */
    bool     raw_pressed;          /* 最近一次原始采样（true=按下） */
    uint8_t  stable_cnt;           /* 当前原始状态已连续稳定的采样次数 */
    bool     pressed;              /* 去抖后的稳定状态 */
    bool     initial_hold;         /* 开机时已被按住（首次抬起前不触发长按） */
    bool     long_reported;        /* 本次按压是否已上报过长按 */
    int64_t  press_start_us;       /* 本次按下开始时刻（esp_timer，us） */
} key_state_t;

static key_state_t s_keys[BTN_IDX_MAX] = {
    [BTN_IDX_UP]   = { .long_press_ms = BUTTON_UP_LONG_PRESS_MS },
    [BTN_IDX_DOWN] = { .long_press_ms = BUTTON_DOWN_LONG_PRESS_MS },
};

/* 交互状态：一次交互 = 从第一个键按下到所有键全部抬起。
   s_any_pressed  是否有键处于按下状态
   s_simultaneous 本次交互期间是否发生过双键同时按下（按下时间存在重叠） */
static bool s_any_pressed = false;
static bool s_simultaneous = false;

/* 用户事件回调 */
static button_event_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

/* 上键长按阈值的动态提供者（按下沿调用一次） */
static button_long_press_provider_t s_up_long_press_provider = NULL;

/* 读取某个键的原始"按下"状态 */
static bool button_raw_pressed(int idx)
{
    if (idx == BTN_IDX_UP) {
        /* 上键在 PMU 上：由 AXP2101 的 0x49 按下/抬起边沿事件维护 */
        return axp2101_is_power_key_pressed();
    }
    return gpio_get_level(BUTTON_DOWN_PIN) == 0;   /* 低电平 = 按下 */
}

/* 打印日志并转发用户回调 */
static void button_report(button_event_t event)
{
    switch (event) {
    case BUTTON_EVENT_UP_SHORT:
        ESP_LOGI(TAG, "UP key (PMU PWRON) short press");
        break;
    case BUTTON_EVENT_UP_LONG:
        ESP_LOGI(TAG, "UP key (PMU PWRON) long press (%u ms)",
                 (unsigned)s_keys[BTN_IDX_UP].active_long_press_ms);
        break;
    case BUTTON_EVENT_DOWN_SHORT:
        ESP_LOGI(TAG, "DOWN key (GPIO%d) short press", BUTTON_DOWN_PIN);
        break;
    case BUTTON_EVENT_DOWN_LONG:
        ESP_LOGI(TAG, "DOWN key (GPIO%d) long press (%u ms)", BUTTON_DOWN_PIN,
                 (unsigned)BUTTON_DOWN_LONG_PRESS_MS);
        break;
    case BUTTON_EVENT_BOTH:
        ESP_LOGI(TAG, "both keys pressed together");
        break;
    default:
        break;
    }

    if (s_cb != NULL) {
        s_cb(event, s_cb_ctx);
    }
}

/* 轮询扫描两个按键：去抖 + 长按判定 + 双按判定 */
static void button_scan(void)
{
    for (int i = 0; i < BTN_IDX_MAX; i++) {
        key_state_t *k = &s_keys[i];
        const bool raw = button_raw_pressed(i);

        /* 软件去抖 */
        if (raw == k->raw_pressed) {
            if (k->stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
                k->stable_cnt++;
            }
        } else {
            k->raw_pressed = raw;
            k->stable_cnt = 0;
        }
        if (k->stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
            continue;   /* 仍处于抖动中 */
        }

        if (k->pressed == raw) {
            /* 状态未变化：检查长按。条件：单键按住（另一键未按下、本次交互
               不是双按）、非开机初始按压、按住时长达到本次阈值。 */
            if (k->pressed && !k->long_reported && !k->initial_hold
                && !s_simultaneous && !s_keys[1 - i].pressed
                && esp_timer_get_time() - k->press_start_us
                   >= (int64_t)k->active_long_press_ms * 1000) {
                k->long_reported = true;
                button_report((i == BTN_IDX_UP) ? BUTTON_EVENT_UP_LONG
                                                : BUTTON_EVENT_DOWN_LONG);
            }
            continue;
        }

        /* 状态翻转 */
        k->pressed = raw;

        if (raw) {
            /* ---- 按下 ---- */
            k->long_reported = false;
            k->active_long_press_ms = k->long_press_ms;
            if (i == BTN_IDX_UP && s_up_long_press_provider != NULL) {
                const uint32_t dyn = s_up_long_press_provider();
                if (dyn > 0) {
                    k->active_long_press_ms = dyn;
                }
            }
            if (k->initial_hold) {
                /* 开机初始按压（长按电源键开机后未及时松手）：不启动长按计时 */
                k->press_start_us = 0;
            } else {
                k->press_start_us = esp_timer_get_time();
            }
            if (s_any_pressed) {
                /* 另一键已按住 → 本次交互为双键同时按下 */
                s_simultaneous = true;
            } else {
                s_simultaneous = false;
            }
            s_any_pressed = true;
        } else {
            /* ---- 抬起 ---- */
            k->initial_hold = false;   /* 首次抬起后解除开机按压抑制 */
            const bool was_long = k->long_reported;
            k->long_reported = false;

            if (s_keys[1 - i].pressed) {
                /* 另一键仍按住：本键抬起属于双按交互的一部分，暂不上报 */
                continue;
            }

            /* 所有按键已抬起：本次交互结束 */
            s_any_pressed = false;
            if (s_simultaneous) {
                button_report(BUTTON_EVENT_BOTH);
                s_simultaneous = false;
            } else if (!was_long) {
                button_report((i == BTN_IDX_UP) ? BUTTON_EVENT_UP_SHORT
                                                : BUTTON_EVENT_DOWN_SHORT);
            }
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
    /* 下键 GPIO：输入 + 内部上拉，按键接 GND */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_DOWN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 以当前实际状态为去抖基线，但确认状态初始视为"抬起"：
       若开机时按键已被按住，去抖后仍能正确触发按下事件 */
    for (int i = 0; i < BTN_IDX_MAX; i++) {
        key_state_t *k = &s_keys[i];
        const bool held = button_raw_pressed(i);
        k->raw_pressed = held;
        k->stable_cnt = 0;
        k->pressed = false;
        k->initial_hold = held;   /* 开机已按住：首次抬起前不触发长按 */
        k->long_reported = false;
        k->press_start_us = 0;
        k->active_long_press_ms = k->long_press_ms;
    }

    xTaskCreatePinnedToCore(button_task, "button_svc",
                            BUTTON_TASK_STACK, NULL,
                            BUTTON_TASK_PRIO, NULL, BUTTON_TASK_CORE);

    ESP_LOGI(TAG, "Button service started: UP=PMU PWRON (via AXP2101), "
                  "DOWN=GPIO%d (internal pullup, active low), poll %d ms",
             BUTTON_DOWN_PIN, BUTTON_POLL_MS);
}

void button_service_register_cb(button_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

void button_service_set_up_long_press_provider(button_long_press_provider_t fn)
{
    /* 只保存指针：每次按下沿调用一次取得本次阈值（单字写入，并发安全） */
    s_up_long_press_provider = fn;
}
