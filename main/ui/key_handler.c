#include "ui/key_handler.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/button_service.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/screen_sleep.h"
#include "core/utils/sleep_timer.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "key_handler";

/* 唤醒防循环：deepsleep 唤醒 = 芯片重启，若唤醒时按键仍被按住，boot 后
   button_service 去抖仍会报告事件 → 立即再次休眠。开机 1.2 秒内忽略
   按键事件（长按由 button_service 的"开机初始按压"机制另行抑制）。 */
#define DEEP_SLEEP_IGNORE_MS  1200

/* 音量按键步进（单位：档，音量范围 0~30）：
   - 短按 ±1（0=静音，1~30 线性映射 20%~80%，每档约 2 个百分点）；
   - B 键长按一次性 -10 档（B 键可安全长按）。 */
#define VOL_STEP_SHORT   1
#define VOL_STEP_LONG_B  10

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
   按当前息屏状态分发两套按键映射（见 key_handler.h 注释）。 */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;

    /* 防循环：刚开机/唤醒 1.2 秒内忽略按键事件（唤醒时用户可能仍按着按键） */
    if (esp_timer_get_time() < (int64_t)DEEP_SLEEP_IGNORE_MS * 1000) {
        ESP_LOGI(TAG, "button event %d ignored within %d ms after boot",
                 (int)event, DEEP_SLEEP_IGNORE_MS);
        return;
    }

    /* 任何按键事件都算一次用户活动（刷新自动息屏计时） */
    screen_sleep_note_activity();

    const bool sleeping = screen_sleep_is_sleeping();
    switch (event) {
    case BUTTON_EVENT_A_SHORT:
        if (sleeping) {
            /* 息屏：播放/暂停（黑屏盲操作，不亮屏） */
            player_controller_toggle();
        } else {
            /* 亮屏：音量 + */
            key_adjust_volume(VOL_STEP_SHORT);
        }
        break;
    case BUTTON_EVENT_B_SHORT:
        if (sleeping) {
            /* 息屏：下一曲 */
            player_controller_next();
        } else {
            /* 亮屏：音量 - */
            key_adjust_volume(-VOL_STEP_SHORT);
        }
        break;
    case BUTTON_EVENT_A_LONG:
        if (sleeping) {
            /* 息屏：先亮屏（恢复屏幕/LVGL/亮度），再走"等待断电"伪关机，
               让用户看到保存/关机提示画面（计划第 6 点要求） */
            screen_sleep_exit();
        }
        /* A 键（电源键，与 3 秒关机芯片复用）长按：固件先保存配置并停外设，
           定格提示后 deepsleep；继续按住满 3s 即硬件断电（真正关机） */
        fake_shutdown_wait_power_off();
        break;
    case BUTTON_EVENT_B_LONG:
        if (sleeping) {
            /* 息屏：上一曲（B 键 GPIO0 可安全长按） */
            player_controller_prev();
        } else {
            /* 亮屏：音量一次性 -10 */
            key_adjust_volume(-VOL_STEP_LONG_B);
        }
        break;
    case BUTTON_EVENT_BOTH:
        /* 双键同时按下：息屏/亮屏切换（计划第 5 点：双按不再是伪关机） */
        screen_sleep_toggle();
        break;
    default:
        break;
    }
}

void key_handler_init(void)
{
    button_service_register_cb(on_button_event, NULL);
    ESP_LOGI(TAG, "key handler ready: "
                  "screen on [A=vol+, B=vol-, A long=power off, B long=vol-10, both=screen off]; "
                  "screen off [A=play/pause, B=next, B long=prev, A long=power off, both=screen on]");
}
