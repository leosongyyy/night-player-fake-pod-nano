#include "core/utils/fake_shutdown.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/player/player_controller.h"
#include "core/ui/lvgl_port.h"

static const char *TAG = "fake_shutdown";

/* 关机流程互斥标志：防止"长按伪关机进行中"又被双按/睡眠定时器再次触发
   （deepsleep 前的停外设阶段两路并发会重复删除 I2C/SDMMC 驱动而崩溃）。
   多个任务（按键/睡眠定时器）只做"置 true"的写与判读，单字节原子。 */
static volatile bool s_shutting_down = false;

/* 伪关机任务参数：关机流程含 LVGL 渲染（提示画面，lv_refr_now 调用链深）
   与 NVS/SD/I2C 等操作，必须放在独立的高栈任务执行——曾直接在按键任务
   （栈 3KB）里跑导致栈溢出重启（A stack overflow in task button_svc）。 */
#define FAKE_SHUTDOWN_TASK_STACK  (8 * 1024)
#define FAKE_SHUTDOWN_TASK_PRIO   6   /* 高于 LVGL(2)/按键(4)，保证关机流程不被拖延 */
#define FAKE_SHUTDOWN_TASK_CORE   0

/* 全屏提示覆盖层（黑底 + 居中白字）：显示在 LVGL 顶层，覆盖当前界面；
   A 键长按关机时把提示定格为最后一帧，等待硬件断电自然熄灭。 */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_hint_label = NULL;

/* 在 LVGL 顶层显示全屏提示（须持 LVGL 锁调用）：
   - 首次创建黑底覆盖层 + 居中 label，之后复用仅更新文字；
   - lv_refr_now 强制把提示刷到屏幕，保证定格前画面完整。 */
static void fake_shutdown_show_hint(const char *text)
{
    if (s_overlay == NULL) {
        s_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_overlay, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_overlay, 0, 0);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);

        s_hint_label = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(s_hint_label, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(s_hint_label, lv_color_white(), 0);
        lv_obj_center(s_hint_label);
    }
    lv_label_set_text(s_hint_label, text);
    lv_refr_now(NULL);   /* 强制刷出（须持锁），保证定格前提示完整显示 */
}

/* 统一的伪关机流程（进入 deepsleep，不返回）：
   - show_hint:    是否显示两段全屏提示（A 键长按关机时显示）
   - keep_display: 是否保留屏幕画面（true = 不主动关屏，靠断电自然熄灭；
                   false = 主动关屏，普通省电伪关机） */
static void fake_shutdown_common(bool show_hint, bool keep_display)
{
    /* 0. 显示「正在保存配置」，随后保存当前播放位置到 NVS
         （关机后开机自动恢复播放进度）。先保存再做停播/关外设，
         因为停播后播放状态即失效。 */
    if (show_hint) {
        if (lvgl_port_lock(-1)) {
            fake_shutdown_show_hint("正在保存配置");
            lvgl_port_unlock();
        }
    }
    player_controller_save_last_position();

    /* 1. 停止播放并等待解码任务退出（释放文件句柄与 I2S） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_IDLE && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
    }

    /* 2. 保存完成，显示最终提示并定格（A 键长按关机时）：
         提示用户"继续按住上键（A/电源键）满 3s 由关机芯片彻底断电，
         或按下键（B 键）取消关机"；随后挂起 LVGL 定格画面。 */
    if (show_hint) {
        if (lvgl_port_lock(-1)) {
            fake_shutdown_show_hint("继续按上键以彻底关机\n或者按下键取消关机");
            lvgl_port_unlock();
        }
    }

    /* 3. 挂起 LVGL 任务（停止刷屏与触摸轮询，避免后面删除 I2C 驱动后报错）。
         画面定格在最后一帧：AMOLED 为存储型显示，保持供电即持续显示提示。 */
    lvgl_port_suspend();

    /* 4. 复位 CS43131（GPIO41 拉低硬件复位：全部电源域含晶振一起掉电，
           最彻底关机且状态确定；唤醒后 audio_init → cs43131_init 会重新
           走完整复位+配置流程恢复） */
    cs43131_reset_for_sleep();

    /* 5. 关闭显示屏（普通伪关机主动关屏省电）。
         A 键长按关机时不关屏：保留定格提示，等待电源侧关机芯片在长按
         满 3s 时断电，屏幕靠断电自然熄灭（主动关屏会让用户分不清
         是否真的关机）。 */
    if (!keep_display) {
        display_power_off();
    }

    /* 6. 关闭触摸控制器并释放 I2C 总线驱动 */
    touch_power_off();

    /* 7. 卸载 SD 卡并释放 SDMMC 主机 */
    sd_card_deinit();

    /* 8. 配置 deep sleep 唤醒：B 键（GPIO0，RTC GPIO）按下（低电平）唤醒。
        关键约束（ESP32-S3）：
        - deep sleep GPIO 唤醒只支持 RTC GPIO（GPIO0~21，SOC_RTCIO_PIN_COUNT=22），
          A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒——A 键只负责
          长按触发伪关机，真正断电由电源侧关机芯片硬件完成，无需唤醒；
        - esp_sleep_enable_gpio_wakeup 仅支持 light sleep（IDF 文档明确），
          此前误用导致进入 deepsleep 后快速误唤醒；
        - RTC 外设关闭时内部上拉不工作，需外部电阻——GPIO0 的板载 10k 上拉
          正好满足，平时高电平、按下拉低触发唤醒。 */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW));

    ESP_LOGI(TAG, "deep sleep: peripherals off, press B key (GPIO0) to wake");
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等待日志刷出 */
    esp_deep_sleep_start();
}

/* 伪关机任务：arg != NULL = A 键长按关机（显示提示、不关屏）；
   arg == NULL = 普通伪关机（无提示、主动关屏）。进入 deepsleep 不返回。 */
static void fake_shutdown_task(void *arg)
{
    const bool power_key = (arg != NULL);
    fake_shutdown_common(power_key, power_key);
    vTaskDelete(NULL);   /* 理论不可达：deepsleep 不返回 */
}

void fake_shutdown_enter(void)
{
    /* 防御：伪关机流程已在进行（长按关机期间又触发双按/睡眠定时器）时
       直接返回，避免并发关闭外设重复删除驱动 */
    if (s_shutting_down) {
        return;
    }
    s_shutting_down = true;
    ESP_LOGI(TAG, "=== fake shutdown: close all peripherals, enter deep sleep ===");
    /* 独立任务执行（栈 8KB）：不要在按键/睡眠定时器任务内直接跑关机流程 */
    if (xTaskCreatePinnedToCore(fake_shutdown_task, "shutdown",
                                FAKE_SHUTDOWN_TASK_STACK, NULL,
                                FAKE_SHUTDOWN_TASK_PRIO, NULL,
                                FAKE_SHUTDOWN_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create shutdown task failed");
        s_shutting_down = false;   /* 创建失败：复位标志，允许重试 */
    }
}

void fake_shutdown_wait_power_off(void)
{
    if (s_shutting_down) {
        return;
    }
    s_shutting_down = true;
    ESP_LOGI(TAG, "=== power-key shutdown: save, freeze hint, wait hardware power off ===");
    /* 独立任务执行（栈 8KB）：流程含 LVGL 渲染提示，栈需求大 */
    if (xTaskCreatePinnedToCore(fake_shutdown_task, "shutdown",
                                FAKE_SHUTDOWN_TASK_STACK, (void *)1,
                                FAKE_SHUTDOWN_TASK_PRIO, NULL,
                                FAKE_SHUTDOWN_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create shutdown task failed");
        s_shutting_down = false;   /* 创建失败：复位标志，允许重试 */
    }
}
