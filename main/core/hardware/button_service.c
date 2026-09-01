#include "core/hardware/button_service.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

/* A 键：GPIO48，GPIO-按钮-GND，外部无上拉，需启用内部上拉。
   ⚠ A 键与成品机电源侧的 3 秒关机芯片复用同一按键：长按超过 3 秒由硬件
   直接断电/开机；固件在长按 2 秒（BUTTON_LONG_PRESS_MS）时主动伪关机。 */
#define BUTTON_A_PIN        GPIO_NUM_48
/* B 键：GPIO0，按键接 GND，外部已有 10kΩ 上拉，无需内部上拉。
   注意 GPIO0 为 strapping 引脚：复位时若被按住会进入下载模式，属硬件特性。
   GPIO0 是 RTC GPIO，deepsleep 后按下可唤醒（取消关机）。 */
#define BUTTON_B_PIN        GPIO_NUM_0

/* A 键长按阈值（ms）：单键按住超过该时长即上报长按事件（无需等待抬起）。
   与电源侧关机芯片的 3 秒硬断电配合：固件在阈值时刻主动保存配置并伪关机，
   剩余窗口（3s - 阈值）足够完成 NVS 写入与停外设。
   经 Kconfig CONFIG_BUTTON_LONG_PRESS_MS 配置（默认 1200ms）：
   曾用 2000ms 实测偶发"按了却没看到提示"——用户按满 3s 时提示只显示
   约 1 秒甚至一闪而过（伪关机流程本身还要耗时），降到 1.2s 后提示显示
   窗口约 1.8s，更容易观察。 */
#ifdef CONFIG_BUTTON_LONG_PRESS_MS
#define BUTTON_A_LONG_PRESS_MS  CONFIG_BUTTON_LONG_PRESS_MS
#else
#define BUTTON_A_LONG_PRESS_MS  1200
#endif

/* B 键长按阈值（ms）：B 键（GPIO0）可安全长按，亮屏=音量-10、息屏=上一曲，
   判定缩短到 600ms 提升响应速度（用户要求 2026-09-03）。
   ⚠ 与双按判定不冲突：长按判定要求"另一键未按下"，双按判定以"两键按下
   时间重叠"为准，机制互斥。 */
#define BUTTON_B_LONG_PRESS_MS  600

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
    uint32_t   long_press_ms; /* 本键长按阈值（ms），A/B 可不同（A=1200，B=600） */
    bool       raw_level;   /* 最近一次原始采样电平（true=高=抬起） */
    uint8_t    stable_cnt;  /* 当前原始电平已连续稳定的采样次数 */
    bool       level;       /* 去抖后的稳定电平（true=高=抬起） */
    bool       pressed;     /* 当前是否处于按下状态 */
    bool       initial_hold;   /* 开机时已被按住（首次抬起前不触发长按） */
    bool       long_reported;  /* 本次按压是否已上报过长按（抬起不再报单击） */
    int64_t    press_start_us; /* 本次按下开始时刻（esp_timer，us） */
} button_state_t;

static button_state_t s_buttons[BTN_IDX_MAX] = {
    [BTN_IDX_A] = { .pin = BUTTON_A_PIN, .pullup_en = true,
                    .long_press_ms = BUTTON_A_LONG_PRESS_MS },
    [BTN_IDX_B] = { .pin = BUTTON_B_PIN, .pullup_en = false,
                    .long_press_ms = BUTTON_B_LONG_PRESS_MS },
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
    case BUTTON_EVENT_A_LONG:
        ESP_LOGI(TAG, "A key (GPIO%d) long press (%u ms)",
                 BUTTON_A_PIN, (unsigned)BUTTON_A_LONG_PRESS_MS);
        break;
    case BUTTON_EVENT_B_LONG:
        ESP_LOGI(TAG, "B key (GPIO%d) long press (%u ms)",
                 BUTTON_B_PIN, (unsigned)BUTTON_B_LONG_PRESS_MS);
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

/* 轮询扫描两个按键：去抖 + 长按判定 + 事件判定 */
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
            /* 去抖后的电平未变化：若处于"按住"状态，检查是否达到长按阈值。
               长按条件：单键按住（另一键未按下，双按交互不触发长按）、
               非开机初始按压、按住时长 >= 阈值。上报一次后不再重复。 */
            if (b->pressed && !b->long_reported && !b->initial_hold
                && !s_buttons[1 - i].pressed
                && esp_timer_get_time() - b->press_start_us
                   >= (int64_t)b->long_press_ms * 1000) {
                b->long_reported = true;
                button_report((i == BTN_IDX_A) ? BUTTON_EVENT_A_LONG
                                               : BUTTON_EVENT_B_LONG);
            }
            continue;
        }
        b->level = raw;

        if (!raw) {
            /* ---- 按下（低电平） ---- */
            b->pressed = true;
            b->long_reported = false;
            if (b->initial_hold) {
                /* 开机初始按压（长按 3s 开机后未及时松手）：不启动长按计时，
                   等首次抬起后恢复正常检测 */
                b->press_start_us = 0;
            } else {
                b->press_start_us = esp_timer_get_time();
            }
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
            b->initial_hold = false;   /* 首次抬起后解除开机按压抑制 */
            const bool was_long = b->long_reported;
            b->long_reported = false;

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
            } else if (!was_long) {
                /* 单键且未上报过长按：抬起即上报单击；
                   已上报过长按（如 A 键长按 2s 关机）的抬起不再补报单击 */
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
        /* 开机时已被按住（如长按电源键 3s 开机后未及时松手）：标记为初始
           按压，首次抬起前不触发长按，避免"开机即误判长按关机" */
        s_buttons[i].initial_hold   = (gpio_get_level(s_buttons[i].pin) == 0);
        s_buttons[i].long_reported  = false;
        s_buttons[i].press_start_us = 0;
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
