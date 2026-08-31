#include "core/utils/fake_shutdown.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/hardware/audio/cs43131.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"
#include "core/hardware/sd_card.h"
#include "core/hardware/touch.h"
#include "core/ui/lvgl_port.h"

static const char *TAG = "fake_shutdown";

/* 伪关机：尽量关闭所有外设（关尽），随后进入 deepsleep。
   调用后系统进入深度休眠，被唤醒时整机重启（不会返回本函数）。 */
void fake_shutdown_enter(void)
{
    ESP_LOGI(TAG, "=== fake shutdown: close all peripherals, enter deep sleep ===");

    /* 1. 停止播放并等待解码任务退出（释放文件句柄与 I2S） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_IDLE && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
    }

    /* 2. 挂起 LVGL 任务（停止刷屏与触摸轮询，避免后面删除 I2C 驱动后报错） */
    lvgl_port_suspend();

    /* 3. 复位 CS43131（GPIO41 拉低硬件复位：全部电源域含晶振一起掉电，
           最彻底关机且状态确定；唤醒后 audio_init → cs43131_init 会重新
           走完整复位+配置流程恢复） */
    cs43131_reset_for_sleep();

    /* 4. 关闭显示屏（AMOLED 显示关断） */
    display_power_off();

    /* 5. 关闭触摸控制器并释放 I2C 总线驱动 */
    touch_power_off();

    /* 6. 卸载 SD 卡并释放 SDMMC 主机 */
    sd_card_deinit();

    /* 7. 配置 deep sleep 唤醒：B 键（GPIO0，RTC GPIO）按下（低电平）唤醒。
       关键约束（ESP32-S3）：
       - deep sleep GPIO 唤醒只支持 RTC GPIO（GPIO0~21，SOC_RTCIO_PIN_COUNT=22），
         A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒；
       - esp_sleep_enable_gpio_wakeup 仅支持 light sleep（IDF 文档明确），
         此前误用导致进入 deepsleep 后快速误唤醒；
       - RTC 外设关闭时内部上拉不工作，需外部电阻——GPIO0 的板载 10k 上拉
         正好满足，平时高电平、按下拉低触发唤醒。 */
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ANY_LOW));

    ESP_LOGI(TAG, "deep sleep: display/touch/DAC off, press B key (GPIO0) to wake");
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等待日志刷出 */
    esp_deep_sleep_start();
}
