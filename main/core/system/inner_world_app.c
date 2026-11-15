#include "core/system/inner_world_app.h"

#include "core/fonts/system_font.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/storage/config_store.h"
#include "core/system/fw_switch.h"
#include "core/ui/lvgl_port.h"
#include "ui/inner_world_ui.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "inner_world_app";

/* 里世界界面的最低亮度：用户配置的亮度可能很低（最低 16），看不清就等于
   没有反馈。只抬亮这一屏，**不改**用户配置（回表世界后仍是原亮度）。 */
#define INNER_WORLD_MIN_BRIGHTNESS  96

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

    /* ① 第一件事：把下次启动改回表世界（= 擦 otadata）。
          ⚠ 必须早于任何可能耗时/失败的初始化：之后崩溃、断电、看门狗复位、
            镜像后续损坏，设备都回表世界，绝不会卡在里世界反复重启。
          ⚠ 擦 otadata **不影响当前运行的里世界**——它只决定下次启动选哪个分区，
            所以「回到表世界」按钮只需要 esp_restart()。 */
    esp_err_t err = fw_switch_to_player();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "回切表世界失败（%s）：仍继续显示里世界，但下次启动可能又进本固件",
                 esp_err_to_name(err));
    }

    /* ② 最小初始化（顺序：config_store → display → LVGL → 字体 → 触摸 →
          PMU/RTC（都挂 I²C0，总线由 touch_init 建立）→ SD）。
          SD 在这里先以 FATFS 挂载：WiFi 对时要读 /sdcard/.ecov/wifi.txt；
          进 USB 传歌时 sd_card_raw_init() 会把它卸载掉。（卡不存在不致命） */
    ESP_LOGI(TAG, "init: config_store");
    config_store_init();

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
    sd_card_init();   /* 失败不阻塞：列表会显示「无 SD 卡」，对时/传歌会提示 */

    /* ③ 点亮屏幕：先渲染"黑底 + 里世界界面"，再给亮度（与表世界开机同一套
          防白屏顺序：面板此刻亮度仍是 0）。 */
    uint8_t brightness = config_store_get_brightness();
    if (brightness < INNER_WORLD_MIN_BRIGHTNESS) {
        brightness = INNER_WORLD_MIN_BRIGHTNESS;
    }

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

    /* ④ app_main 到此返回：LVGL 任务继续渲染，触摸事件驱动功能行与
          「回到表世界」按钮。功能结束后（对时回列表 / USB 传歌保持到重启）
          都不需要本函数再做任何事。 */
    ESP_LOGI(TAG, "里世界就绪：WiFi对时 / USB传歌 / 回到表世界");
}
