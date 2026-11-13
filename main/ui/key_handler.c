#include "ui/key_handler.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/button_service.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/power_service.h"
#include "core/utils/sleep_timer.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "key_handler";

/* 开机/唤醒后的按键保护窗口（ms）：AXP2101 开机瞬间可能带着按键残留，
   同时避免 deep sleep 时代"唤醒时按键仍被按住 → 立即再次关机"的循环。
   当前 S31 不做固件伪关机，但保留该保护以过滤开机抖动。 */
#define DEEP_SLEEP_IGNORE_MS  1200

/* 音量按键步进（单位：档，音量范围 0~30）：
   - 短按 -1（0=静音，1~30 线性映射 20%~80%，每档约 2 个百分点）；
   - 长按一次性 -10 档。 */
#define VOL_STEP_SHORT   1
#define VOL_STEP_LONG    10

/* 调节音量：config_store（持久化 + 通知 UI 回调）+ audio（写 CS43131 硬件）。
   ⚠ 顺序必须是【先 audio_set_volume_level 再 config_store_set_volume】：
   config_store 的变更回调（状态栏音量 label 等）会读取 audio_get_volume_level()，
   若先持久化再写硬件，回调读到的是旧音量，显示滞后到下次周期刷新。
   ⚠ config_store 回调还会操作 LVGL 对象（设置页/通知栏刷新），
   必须持有 LVGL 锁调用（本函数从按键任务上下文执行，不能直接碰 LVGL）。 */
static void key_adjust_volume(int8_t delta)
{
    lvgl_port_lock(-1);
    /* 睡眠定时音量渐降期间手动调音量：用户接管，静默取消渐降
       （保持当前音量继续手动调节，不再自动降低） */
    sleep_timer_fade_cancel();
    int32_t vol = (int32_t)audio_get_volume_level() + delta;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > AUDIO_VOL_LEVEL_MAX) {
        vol = AUDIO_VOL_LEVEL_MAX;
    }
    audio_set_volume_level((uint8_t)vol);          /* 先写硬件（回调读到的即新值） */
    config_store_set_volume((uint8_t)vol);         /* 后持久化 + 通知回调（持锁安全） */
    lvgl_port_unlock();
    ESP_LOGI(TAG, "volume level -> %d", (int)vol);
}

/* 按键事件回调（button_service 任务上下文）：
   S31 只有一个独立按键，按屏幕状态分发两套映射（见 key_handler.h）。 */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;

    /* 防抖/防残留：刚开机/唤醒 1.2 秒内忽略按键事件 */
    if (esp_timer_get_time() < (int64_t)DEEP_SLEEP_IGNORE_MS * 1000) {
        ESP_LOGI(TAG, "button event %d ignored within %d ms after boot",
                 (int)event, DEEP_SLEEP_IGNORE_MS);
        return;
    }

    /* 任何按键事件都算一次用户活动（刷新自动息屏计时） */
    power_service_note_activity();

    const bool sleeping = power_service_is_screen_off();
    switch (event) {
    case BUTTON_EVENT_SHORT:
        if (sleeping) {
            /* 息屏：先亮屏，再下一曲（单键唯一能兼顾"唤醒 + 盲操作"的方案） */
            power_service_screen_on();
            player_controller_next();
            /* 息屏切歌属于"用户动作"：清醒感知加时 */
            sleep_timer_note_activity();
        } else {
            /* 亮屏：音量 -1 */
            key_adjust_volume(-VOL_STEP_SHORT);
        }
        break;
    case BUTTON_EVENT_LONG:
        if (sleeping) {
            /* 息屏：先亮屏，再上一曲 */
            power_service_screen_on();
            player_controller_prev();
            sleep_timer_note_activity();
        } else {
            /* 亮屏：音量一次性 -10 */
            key_adjust_volume(-VOL_STEP_LONG);
        }
        break;
    default:
        break;
    }
}

void key_handler_init(void)
{
    button_service_register_cb(on_button_event, NULL);
    ESP_LOGI(TAG, "key handler ready (single key): "
                  "screen on [short=vol-1, long=vol-10]; "
                  "screen off [short=wake+next, long=wake+prev]");
}
