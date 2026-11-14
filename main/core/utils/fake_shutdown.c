#include "core/utils/fake_shutdown.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/axp2101.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/power_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "fake_shutdown";

/* ------------------------------------------------------------------ */
/* S31 关机流程（AXP2101 软关机，替代 S3 的 deepsleep 伪关机）           */
/*                                                                     */
/* 顺序：                                                              */
/*   1. 显示「正在关机...」（LVGL 全屏覆盖层，同步刷出一帧）；            */
/*   2. 保存播放现场（歌曲/随机位置/列表名/是否在播）+ NVS 落盘；         */
/*   3. 停止播放、等解码任务退出；                                      */
/*   4. 挂起 LVGL（停止刷屏/触摸/封面任务）；                            */
/*   5. 卸载 SD（FAT 落盘、释放 SDMMC）；                               */
/*   6. 写 AXP2101 0x10[0]=1，PMU 关掉除 RTCLDO 外所有电源 → 真断电。    */
/*                                                                     */
/* ⚠ 顺序不能反：必须在写 0x10 之前完成所有 flash 写（NVS/SD），否则      */
/*   断电会写坏数据；也不能提前 touch_power_off/删 I2C 总线，否则第 6 步  */
/*   的 PMU 写寄存器会失败。                                            */
/* ⚠ 不显示 S3 时代的「您现在可以安全断开电源了」：真关机不需要用户手动   */
/*   断电。断电失败（例如 VBUS 供电下 PMU 不响应）时在屏上提示长按 4 秒   */
/*   由 PMU 硬件强制断电。                                              */
/* ------------------------------------------------------------------ */

/* 写 0x10 后等断电的时间（ms）：PMU 正常会在几十 ms 内掉电；
   2.5s 仍存活视为失败（仍在 deep_worker 的 5s 超时以内）。 */
#define FS_POWER_OFF_WAIT_MS  2500

/* 关机流程互斥标志：长按电源键 / 睡眠定时器 / 低电策略可能同时触发，
   只允许一条流程执行（重复卸载 SD、重复写 NVS 都会出问题）。 */
static volatile bool s_shutting_down = false;

/* 关机提示覆盖层与文字句柄（LVGL 对象只在持锁时访问） */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_msg_label = NULL;

/* 创建/重建全屏覆盖层：纯黑底 + 居中白色「正在关机...」。
   调用方必须持有 LVGL 锁。 */
static void fake_shutdown_ui_build(const char *msg)
{
    if (s_overlay == NULL) {
        s_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_overlay, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_set_style_radius(s_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_overlay, 0, 0);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
    } else {
        lv_obj_clean(s_overlay);
    }

    /* 只留一行居中白字：不再有 S3 时代的「安全断开电源」黄色面板/图标，
       真关机由 PMU 自己完成，界面越简单越好。 */
    s_msg_label = lv_label_create(s_overlay);
    lv_label_set_text(s_msg_label, msg);
    lv_obj_set_style_text_font(s_msg_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_msg_label, lv_color_white(), 0);
    lv_obj_center(s_msg_label);

    /* 立即同步刷出这一帧：AMOLED 是存储型显示，之后即使挂起 LVGL，
       画面也会一直保留到 PMU 断电。 */
    lv_refr_now(NULL);
}

/* 显示/更新关机提示（内部自动加锁） */
static void fake_shutdown_ui_show(const char *msg)
{
    if (lvgl_port_lock(-1)) {
        fake_shutdown_ui_build(msg);
        lvgl_port_unlock();
    }
}

/* 关机执行体（deep_worker 任务上下文，**内部 RAM 栈**，可安全写 flash）。
   arg 可传一条自定义提示文案（const char*），NULL 用默认「正在关机...」。 */
static void fake_shutdown_run(void *arg)
{
    const char *msg = (arg != NULL) ? (const char *)arg : "正在关机...";

    /* 1. 提示并同步刷出 */
    fake_shutdown_ui_show(msg);
    ESP_LOGI(TAG, "shutdown: UI shown \"%s\", saving data", msg);

    /* 2. 保存现场：先存播放位置（内部会写一次配置槽位 blob），再补写
          "延迟落盘"的亮度/音量（顺序同旧版：反过来会多写一次 4KB 页）。 */
    player_controller_save_last_position();
    config_store_flush_pending(true);

    /* 3. 停止播放并等待解码任务退出（释放文件句柄/I2S，避免卸载 SD 时
          还有任务在读卡） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        for (int waited = 0;
             waited < 1000 && mp3_player_get_state() != MP3_STATE_IDLE;
             waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (mp3_player_get_state() != MP3_STATE_IDLE) {
            ESP_LOGW(TAG, "mp3 player did not stop in 1s, continue shutdown");
        }
    }

    /* 4. 挂起 LVGL：停止刷屏/触摸轮询/封面任务，保证卸载 SD 时没有别的
          任务还在读卡；屏幕保留最后一帧「正在关机...」。 */
    lvgl_port_suspend();

    /* 5. 卸载 SD（FAT 落盘 + 释放 SDMMC 主机） */
    esp_err_t sd_err = sd_card_deinit();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "sd_card_deinit failed: %s", esp_err_to_name(sd_err));
    }

    /* 6. 发送 PMU 软关机命令：0x10[0]=1，PMU 关掉除 RTCLDO 外所有电源。
          ⚠ 此时不能再调用 touch_power_off()/删 I2C 总线——PMU 还在 I²C0 上。 */
    ESP_LOGW(TAG, "shutdown: sending PMU power-off (0x10[0]=1)");
    esp_err_t err = axp2101_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMU power-off write failed: %s", esp_err_to_name(err));
    }

    /* 7. PMU 正常会在几十 ms 内断电；仍存活说明关机失败，恢复 LVGL 并把
          失败原因显示出来，用户可长按电源键 4 秒由 PMU 硬件强制断电。 */
    vTaskDelay(pdMS_TO_TICKS(FS_POWER_OFF_WAIT_MS));
    ESP_LOGE(TAG, "PMU power-off did not cut power (write %s); still running",
             esp_err_to_name(err));

    lvgl_port_resume();
    fake_shutdown_ui_show("关机失败，请长按电源键 4 秒");

    /* 允许用户再次长按重试（例如拔掉 USB 后再试一次） */
    s_shutting_down = false;
}

/* 统一入口：先亮屏（若已息屏），再委托 deep_worker 执行关机链。
   msg 为关机提示文案（NULL = 默认「正在关机...」）。 */
static void fake_shutdown_start_msg(const char *reason, const char *msg)
{
    if (s_shutting_down) {
        ESP_LOGW(TAG, "shutdown already in progress, ignore (%s)", reason);
        return;
    }
    s_shutting_down = true;
    ESP_LOGW(TAG, "=== shutdown requested: %s ===", reason);

    /* 息屏时先亮屏，否则用户看不到「正在关机...」（显示 RAM 内容虽然在，
       但面板处于 SLEEP IN，不重新初始化不会亮）。
       ⚠ 必须在 deep_worker_call 之前做：power_service_screen_on() 自己会
       投递 deep_worker，在 deep_worker 执行体里再调用会死锁（禁止嵌套）。 */
    if (power_service_is_screen_off()) {
        ESP_LOGI(TAG, "screen off, waking up to show shutdown message");
        power_service_screen_on();
    }

    /* 关机链调用链深（LVGL 全屏刷 + NVS 写 + SD 卸载），统一交给
       deep_worker 的内部 12KB 栈执行；本调用方只做浅投递。
       ⚠ msg 是字符串字面量（静态存储期），在 deep_worker 执行期间一直有效。 */
    if (!deep_worker_call(fake_shutdown_run, (void *)msg)) {
        ESP_LOGE(TAG, "deep_worker unavailable, cannot run shutdown chain");
        s_shutting_down = false;
    }
}

static void fake_shutdown_start(const char *reason)
{
    fake_shutdown_start_msg(reason, NULL);
}

void fake_shutdown_enter(void)
{
    /* 普通关机（睡眠定时器/自动关机策略调用） */
    fake_shutdown_start("auto/regular");
}

void fake_shutdown_wait_power_off(void)
{
    /* 电源键长按关机（PMU 按键事件调用）：走同一条「正在关机...」流程 */
    fake_shutdown_start("power key long press");
}

void fake_shutdown_charger_unplugged(void)
{
    /* 充电专用界面拔掉充电器：同一条流程，文案说明原因 */
    fake_shutdown_start_msg("charger unplugged (charge-only boot)",
                            "充电器已断开，正在关机...");
}
