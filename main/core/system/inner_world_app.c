#include "core/system/inner_world_app.h"

#include "core/fonts/system_font.h"
#include "core/hardware/battery.h"
#include "core/hardware/button_service.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/storage/config_store.h"
#include "core/system/fw_switch.h"
#include "core/system/usb_msc.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/fake_shutdown.h"
#include "core/utils/performance_monitor.h"
#include "core/utils/power_service.h"
#include "ui/inner_world_ui.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "inner_world_app";

/* 亮度：与表世界一致，直接用用户配置值（config_store 已把下限钳到
   CONFIG_STORE_BRIGHTNESS_MIN=16，不会全黑）。2026-11-16 按用户要求去掉
   原先"里世界至少 96"的抬亮——两套固件亮度必须一致，否则从里世界回表世界
   会突然变暗/变亮。 */

/* ------------------------------------------------------------------ */
/* 触摸就绪回调（触摸异步初始化任务上下文）                              */
/* ------------------------------------------------------------------ */

static void on_touch_ready(esp_lcd_touch_handle_t tp, void *ctx)
{
    (void)ctx;
    lvgl_port_set_touch_handle(tp);
    ESP_LOGI(TAG, "touch ready（可点功能行 / 回到表世界）");
}

/* ------------------------------------------------------------------ */
/* 按键（2026-11 接入，与表世界同一套 button_service）                  */
/* ------------------------------------------------------------------ */
/* 里世界不放歌，按键只承担两件事：
   · 任意键 = 用户活动（刷新自动息屏计时）+ 息屏时亮屏；
   · 上键长按 = 关机，走与表世界完全相同的 fake_shutdown_user_off：
     插着充电器 → 保存后重启进充电界面；没插电 → PMU 真断电。
   ⚠ USB 传歌期间禁止关机：SD 已被 TinyUSB 以裸块设备接管，
     sd_card_deinit() 会在错误状态下动它，且电脑端会看到"卡被拔掉"。 */
static void on_button_event(button_event_t event, void *ctx)
{
    (void)ctx;

    power_service_note_activity();

    switch (event) {
    case BUTTON_EVENT_UP_LONG:
        if (usb_msc_is_active()) {
            ESP_LOGW(TAG, "USB 传歌中，忽略关机请求（请先在电脑上安全弹出）");
            break;
        }
        ESP_LOGW(TAG, "上键长按：关机（插电 → 重启进充电界面 / 未插电 → PMU 断电）");
        fake_shutdown_user_off();
        break;

    case BUTTON_EVENT_BOTH:
        /* 双键：手动息屏/亮屏（与表世界一致） */
        power_service_screen_toggle();
        break;

    default:
        /* 短按/下键长按：只当"用户活动"，息屏时亮屏 */
        if (power_service_is_screen_off()) {
            power_service_screen_on();
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

void inner_world_app_run(void)
{
    ESP_LOGI(TAG, "=== 里世界固件启动：role=%s, running partition=%s ===",
             fw_role_name(), fw_running_partition_label());

    if (!fw_role_is_sync()) {
        /* 构建配置串了（CONFIG_FIRMWARE_ROLE_SYNC 没打开）：不要在这里继续跑，
           否则会变成"没有播放器的固件"。 */
        ESP_LOGE(TAG, "不是里世界固件构建（CONFIG_FIRMWARE_ROLE_SYNC=n），回表世界");
        fw_switch_to_player();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    /* ① 回退纪律（2026-11-16 起由 bootloader 保证，这里只做补清）：
          分区位是**一次性请求**，bootloader 在把控制权交给本固件之前就已经把它
          清 0 了（日志 `[boot] rtc03=0x01 -> ota_0 (inner) (flags kept 0x00)`），
          所以里世界内任何崩溃/断电/看门狗复位，下次启动都会回表世界。
          ⚠ 因此不再在 rtc_init 之前调 fw_switch_to_player()：那时 I²C0 还没建起来，
             只会打一条 "RTC unavailable" 的假警告。真正的清理放在 rtc_init 之后
             （见下面 ② 的 fw_switch_clear_boot_flags）。 */

    /* ② 最小初始化（顺序：config_store → deep_worker → display → LVGL → 字体 →
          触摸 → PMU/RTC（都挂 I²C0，总线由 touch_init 建立）→ SD）。
          SD 在这里先以 FATFS 挂载：WiFi 对时要读 /sdcard/.ecov/wifi.txt；
          进 USB 传歌时 sd_card_raw_init() 会把它卸载掉。（卡不存在不致命）
          ⚠ deep_worker 必须在这一步的**最前面**创建（与表世界同一条纪律：内部
          RAM 在开机末期碎片化，晚创建会静默失败，息屏/关机全部失效）。 */
    ESP_LOGI(TAG, "init: config_store");
    config_store_init();

    ESP_LOGI(TAG, "init: deep_worker（息屏/关机深链执行器）");
    deep_worker_init();

    ESP_LOGI(TAG, "init: display + lvgl port");
    esp_lcd_panel_handle_t panel = display_init();
    lvgl_port_init(panel, NULL);

    ESP_LOGI(TAG, "init: system font");
    system_font_init();

    ESP_LOGI(TAG, "init: touch（异步）");
    touch_init();
    touch_register_ready_cb(on_touch_ready, NULL);

    ESP_LOGI(TAG, "init: PMU / RTC / SD");
    battery_init();   /* AXP2101：电池与充电状态（限充配置由 PMU 自己保持） */
    rtc_init();       /* PCF85063A：WiFi 对时结果写这里 */
    /* 清掉 RTC RAM_byte(0x03) 里的一次性启动标志（分区位 + 充电界面位）：
       · 分区位正常已被 bootloader 消费；
       · bit2（进充电界面）只给表世界用，里世界不实现充电界面，直接清掉，
         避免陈旧标志把下次启动带进充电界面。 */
    fw_switch_clear_boot_flags();
    sd_card_init();   /* 失败不阻塞：列表会显示「无 SD 卡」，对时/传歌会提示 */

    /* ③ 点亮屏幕：先渲染"黑底 + 里世界界面"，再给亮度（与表世界开机同一套
          防白屏顺序：面板此刻亮度仍是 0）。亮度 = 用户配置值（见文件头注释）。 */
    const uint8_t brightness = config_store_get_brightness();

    if (lvgl_port_lock(-1)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        inner_world_ui_create();
        lv_refr_now(NULL);

        ESP_LOGI(TAG, "turn on display (brightness %u)", (unsigned)brightness);
        display_set_brightness(brightness);
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "获取 LVGL 锁失败：里世界界面未创建");
        return;
    }

    /* ④ 按键 + 电源服务（2026-11 接入，与表世界一致）：
          · button_service：上键长按关机、双键息屏、任意键亮屏；
          · power_service：亮屏 320MHz / 息屏 80MHz、无操作自动息屏、
            以及"息屏 + 未充电 + 不在播放（里世界永远不在播放）持续
            CONFIG_PAUSE_AUTO_SHUTDOWN_MIN 分钟 → 关机"。
          ⚠ 这两步必须在界面创建之后（power_service 以初始化时刻为活动基准，
            且要在 LVGL 可用之后才能注册触摸活动回调）。 */
    ESP_LOGI(TAG, "init: button service + power service");
    button_service_init();
    button_service_register_cb(on_button_event, NULL);
    power_service_init();

    /* ⑤ 性能监视（内存 + 双核 CPU + 帧率，串口每 2s）：与表世界同一套代码，
          仅 debug 变体开（CONFIG_PERFORMANCE_MONITOR 由 variant 决定，里世界不再
          单独关掉）。栈在 PSRAM，里世界内部 RAM 本来就富余。
          ⚠ 放在 power_service 之后：监视任务的第一次采样要等 1.2s，
            此时电源状态机已经就绪，读数才有意义。 */
#ifdef CONFIG_PERFORMANCE_MONITOR
    ESP_LOGI(TAG, "init: performance monitor");
    startPerformanceMonitor();
#endif

    /* ⑥ app_main 到此返回：LVGL 任务继续渲染，触摸事件驱动功能行与
          「回到表世界」按钮。功能结束后（对时回列表 / USB 传歌保持到重启）
          都不需要本函数再做任何事。 */
    ESP_LOGI(TAG, "里世界就绪：WiFi对时 / USB传歌 / 回到表世界");
}
