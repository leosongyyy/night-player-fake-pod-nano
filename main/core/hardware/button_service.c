#include "core/hardware/button_service.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* A 键：GPIO48，GPIO-按钮-GND，外部无上拉，需启用内部上拉 */
#define BUTTON_A_PIN        GPIO_NUM_48
/* B 键：GPIO0，按键接 GND，外部已有 10kΩ 上拉，无需内部上拉。
   注意 GPIO0 为 strapping 引脚：复位时若被按住会进入下载模式，属硬件特性。 */
#define BUTTON_B_PIN        GPIO_NUM_0

/* 轮询周期（ms） */
#define BUTTON_POLL_MS      5
/* 软件去抖：连续稳定采样次数达到该值才确认电平变化（总延迟 <= 轮询周期*次数） */
#define BUTTON_DEBOUNCE_SAMPLES 3

/* 轮询任务参数 */
#define BUTTON_TASK_STACK   3072
#define BUTTON_TASK_PRIO    4        /* 高于 LVGL(2)/性能监视(2)，低于音频(8) */
#define BUTTON_TASK_CORE    0        /* 固定 Core0，不影响 Core1 音频解码 */

/* 按键索引 */
enum {
    BTN_IDX_A = 0,
    BTN_IDX_B,
    BTN_IDX_MAX,
};

/* 单个按键的运行状态 */
typedef struct {
    gpio_num_t pin;         /* GPIO 编号 */
    bool       pullup_en;   /* 是否启用内部上拉 */
    bool       raw_level;   /* 最近一次原始采样电平（true=高=抬起） */
    uint8_t    stable_cnt;  /* 当前原始电平已连续稳定的采样次数 */
    bool       level;       /* 去抖后的稳定电平（true=高=抬起） */
    bool       pressed;     /* 当前是否处于按下状态 */
} button_state_t;

static button_state_t s_buttons[BTN_IDX_MAX] = {
    [BTN_IDX_A] = { .pin = BUTTON_A_PIN, .pullup_en = true  },
    [BTN_IDX_B] = { .pin = BUTTON_B_PIN, .pullup_en = false },
};

/* 交互状态：一次交互 = 从第一个键按下到所有键全部抬起。
   s_any_pressed  是否有键处于按下状态
   s_simultaneous 本次交互期间是否发生过双键同时按下（按下时间存在重叠） */
static bool s_any_pressed = false;
static bool s_simultaneous = false;

/* 用户事件回调（可选，默认只打印日志） */
static button_event_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;

/* 打印日志并转发用户回调 */
static void button_report(button_event_t event)
{
    switch (event) {
    case BUTTON_EVENT_A_SHORT:
        ESP_LOGI(TAG, "A key (GPIO%d) short press", BUTTON_A_PIN);
        break;
    case BUTTON_EVENT_B_SHORT:
        ESP_LOGI(TAG, "B key (GPIO%d) short press", BUTTON_B_PIN);
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

/* 轮询扫描两个按键：去抖 + 事件判定 */
static void button_scan(void)
{
    for (int i = 0; i < BTN_IDX_MAX; i++) {
        button_state_t *b = &s_buttons[i];
        const bool raw = gpio_get_level(b->pin) == 1;   /* 高电平 = 抬起 */

        /* 软件去抖：同一原始电平需连续稳定采样多次才生效 */
        if (raw == b->raw_level) {
            if (b->stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
                b->stable_cnt++;
            }
        } else {
            b->raw_level = raw;
            b->stable_cnt = 0;
        }
        if (b->stable_cnt < BUTTON_DEBOUNCE_SAMPLES) {
            continue;   /* 仍处于抖动中，忽略本次采样 */
        }
        if (b->level == raw) {
            continue;   /* 去抖后的电平未变化 */
        }
        b->level = raw;

        if (!raw) {
            /* ---- 按下（低电平） ---- */
            b->pressed = true;
            if (s_any_pressed) {
                /* 已有另一键处于按下状态：本次交互发生双键同时按下 */
                s_simultaneous = true;
            } else {
                /* 本次交互的第一个按下：重置双按标志 */
                s_simultaneous = false;
            }
            s_any_pressed = true;
        } else {
            /* ---- 抬起 ---- */
            b->pressed = false;

            if (s_buttons[1 - i].pressed) {
                /* 另一键仍按住：本键抬起属双按交互的一部分，暂不上报 */
                continue;
            }

            /* 所有按键已抬起：本次交互结束 */
            s_any_pressed = false;
            if (s_simultaneous) {
                /* 双键同时按下：全部抬起后统一上报 */
                button_report(BUTTON_EVENT_BOTH);
                s_simultaneous = false;
            } else {
                /* 单键：抬起即上报单击 */
                button_report((i == BTN_IDX_A) ? BUTTON_EVENT_A_SHORT : BUTTON_EVENT_B_SHORT);
            }
        }
    }
}

/* 按键轮询任务 */
static void button_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        button_scan();
    }
}

void button_service_init(void)
{
    /* 配置 GPIO：输入模式，A 键启用内部上拉，B 键用外部 10k 上拉 */
    for (int i = 0; i < BTN_IDX_MAX; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << s_buttons[i].pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = s_buttons[i].pullup_en ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));

        /* 以当前实际电平为去抖基线，但确认电平初始视为"抬起"：
           若开机时按键已被按住，去抖后仍能正确触发按下事件 */
        s_buttons[i].raw_level = gpio_get_level(s_buttons[i].pin) == 1;
        s_buttons[i].stable_cnt = 0;
        s_buttons[i].level = true;
        s_buttons[i].pressed = false;
    }

    xTaskCreatePinnedToCore(button_task, "button_svc",
                            BUTTON_TASK_STACK, NULL,
                            BUTTON_TASK_PRIO, NULL, BUTTON_TASK_CORE);

    ESP_LOGI(TAG, "Button service started: A=GPIO%d(internal pullup), B=GPIO%d(ext 10k pullup)",
             BUTTON_A_PIN, BUTTON_B_PIN);
}

void button_service_register_cb(button_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
