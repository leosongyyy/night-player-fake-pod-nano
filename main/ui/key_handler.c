#include "ui/key_handler.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/button_service.h"
#include "core/hardware/bt/bt_service.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/power_service.h"
#include "core/utils/quiet_mode.h"
#include "core/utils/sleep_timer.h"
#if CONFIG_CASSETTE_UI
#include "ui/cassette/cassette_fallback.h"
#include "ui/cassette/cassette_nav.h"
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "key_handler";

/* 开机/唤醒后的按键保护窗口（ms）：AXP2101 开机瞬间可能带着按键残留，
   同时避免"刚开机就触发长按关机"的循环。 */
#define DEEP_SLEEP_IGNORE_MS  1200

/* 音量按键步进（单位：档，音量范围 0~30）：
   - 短按 ±1（0=静音，1~30 线性映射 20%~80%，每档约 2 个百分点）；
   - 下键长按一次性 -10 档（下键可安全长按）。 */
#define VOL_STEP_SHORT      1
#define VOL_STEP_LONG_DOWN  10

/* 上键（电源键）长按阈值（ms）：与 S3 一致，息屏 900 / 亮屏 1200。
   ⚠ 必须明显小于 PMU 硬件 OFFLEVEL（实测 4s）与固件保存耗时之和，
     否则可能来不及保存就被硬件断电。 */
#define KEY_UP_LONG_PRESS_ON_MS   1200
#define KEY_UP_LONG_PRESS_OFF_MS  900

/* 上键长按阈值随屏幕状态变化：由 button_service 在按下沿调用一次取定 */
static uint32_t up_long_press_ms_provider(void)
{
    return power_service_is_screen_off() ? KEY_UP_LONG_PRESS_OFF_MS
                                         : KEY_UP_LONG_PRESS_ON_MS;
}

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
    if (audio_bt_profile_active()) {
        /* 蓝牙设备配置优先：写回该设备的音量记忆，不污染配置槽位 */
        bt_service_note_volume((uint8_t)vol);
    } else {
        config_store_set_volume((uint8_t)vol);     /* 后持久化 + 通知回调（持锁安全） */
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "volume level -> %d", (int)vol);
}

/* 按键事件回调（button_service 任务上下文）：
   恢复 S3 时代的两套映射（上键=PMU 电源键，下键=GPIO61，见 key_handler.h）。 */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;

    /* 防残留：刚开机 1.2 秒内忽略按键事件（长按开机后未及时松手） */
    if (esp_timer_get_time() < (int64_t)DEEP_SLEEP_IGNORE_MS * 1000) {
        ESP_LOGI(TAG, "button event %d ignored within %d ms after boot",
                 (int)event, DEEP_SLEEP_IGNORE_MS);
        return;
    }

    /* 安静模式（蓝牙伪关机）：屏幕/LVGL 已挂起，只响应长按电源键 → 重启。
       短按/双键一律忽略，避免误亮屏或误触发播放控制。 */
    if (quiet_mode_is_active()) {
        if (event == BUTTON_EVENT_UP_LONG) {
            quiet_mode_request_reboot();
        }
        return;
    }

#if CONFIG_CASSETTE_UI
    /* 磁带模式：亮屏短按优先用于模块导航（P0-2），消费则直接返回，
       不执行原 vol± / vol-10。UP_LONG=关机、BOTH=息屏以及息屏下的
       播放/切歌映射由 cas_nav_on_key() 显式放行，安全语义不变。
       ⚠ 本回调运行在 button_service 任务，导航会操作 LVGL 对象，
         必须像 key_adjust_volume() 一样持锁（F13：只有一个按键回调，
         不能再注册第二个，只能在这里前置转发）。 */
    if (cassette_ui_is_active()) {
        bool cas_consumed = false;
        if (lvgl_port_lock(1000)) {
            cas_consumed = cas_nav_on_key(event);
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "cassette nav skipped: LVGL lock timeout");
        }
        if (cas_consumed) {
            power_service_note_activity();
            return;
        }
    }
#endif

    /* 任何按键事件都算一次用户活动（刷新自动息屏计时） */
    power_service_note_activity();

    const bool sleeping = power_service_is_screen_off();
    switch (event) {
    case BUTTON_EVENT_UP_SHORT:
        if (sleeping) {
            /* 息屏：播放/暂停（黑屏盲操作，不亮屏） */
            player_controller_toggle();
            sleep_timer_note_activity();
        } else {
            /* 亮屏：音量 +1 */
            key_adjust_volume(VOL_STEP_SHORT);
        }
        break;
    case BUTTON_EVENT_DOWN_SHORT:
        if (sleeping) {
            /* 息屏：下一曲 */
            player_controller_next();
            sleep_timer_note_activity();
        } else {
            /* 亮屏：音量 -1 */
            key_adjust_volume(-VOL_STEP_SHORT);
        }
        break;
    case BUTTON_EVENT_UP_LONG:
        /* 上键长按：关机（不论亮屏/息屏）。fake_shutdown_user_off 会按"是否在
           充电"分流：插电 → 保存现场后重启进充电界面（PMU 不断电，继续执行
           限充与电量检测）；没插电 → 原来的 PMU 真断电。息屏时会先亮屏显示提示。 */
        ESP_LOGW(TAG, "UP key long press: shutdown");
        fake_shutdown_user_off();
        break;
    case BUTTON_EVENT_DOWN_LONG:
        if (sleeping) {
            /* 息屏：上一曲 */
            player_controller_prev();
            sleep_timer_note_activity();
        } else {
            /* 亮屏：音量一次性 -10 */
            key_adjust_volume(-VOL_STEP_LONG_DOWN);
        }
        break;
    case BUTTON_EVENT_BOTH:
        /* 双键同时按下：息屏/亮屏切换 */
        power_service_screen_toggle();
        break;
    default:
        break;
    }
}

void key_handler_init(void)
{
    button_service_register_cb(on_button_event, NULL);
    /* 上键长按阈值随屏幕状态变化：息屏 900ms / 亮屏 1200ms */
    button_service_set_up_long_press_provider(up_long_press_ms_provider);
    ESP_LOGI(TAG, "key handler ready: "
                  "screen on [UP=vol+1, DOWN=vol-1, DOWN long=vol-10, "
                  "UP long=shutdown, both=screen off]; "
                  "screen off [UP=play/pause, DOWN=next, DOWN long=prev, "
                  "UP long=shutdown, both=screen on]");
}
