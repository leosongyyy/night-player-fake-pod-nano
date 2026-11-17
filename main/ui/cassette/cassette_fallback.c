#include "ui/cassette/cassette_fallback.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "core/hardware/axp2101.h"

static const char *TAG = "cas_fallback";

/* NVS 键：专用命名空间，避免与 config_store 的配置槽位/音量等全局键互相污染。
   config_store_init() 已在 main.c 更早调用（内部完成 nvs_flash_init）。 */
#define CAS_NVS_NAMESPACE   "cassette_ui"
#define CAS_NVS_KEY_MODE    "ui_mode"

/* L1 组合键：两键同时按住满 3s **toggle** ui_mode（CASSETTE <-> LEGACY）；
   集中为宏便于真机调手感（U3）。 */
#define CAS_COMBO_HOLD_MS    3000u
#define CAS_COMBO_POLL_MS    20u
#define CAS_DOWN_KEY_GPIO    GPIO_NUM_61   /* 下键：低电平按下，内部上拉 */

static cas_ui_mode_t s_mode = CAS_UI_MODE_CASSETTE;
static bool s_mode_loaded = false;

cas_ui_mode_t cassette_ui_mode_get(void)
{
    if (!s_mode_loaded) {
        nvs_handle_t handle = 0;
        uint8_t raw = (uint8_t)CAS_UI_MODE_CASSETTE;
        esp_err_t err = nvs_open(CAS_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err == ESP_OK) {
            err = nvs_get_u8(handle, CAS_NVS_KEY_MODE, &raw);
            nvs_close(handle);
        }
        if (err == ESP_OK && raw <= (uint8_t)CAS_UI_MODE_LEGACY) {
            s_mode = (cas_ui_mode_t)raw;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ui_mode read failed: %s, use CASSETTE",
                     esp_err_to_name(err));
        }
        s_mode_loaded = true;
    }
    return s_mode;
}

void cassette_ui_mode_set(cas_ui_mode_t mode)
{
    if (mode != CAS_UI_MODE_CASSETTE && mode != CAS_UI_MODE_LEGACY) {
        return;
    }
    s_mode = mode;
    s_mode_loaded = true;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CAS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, CAS_NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(TAG, "ui_mode -> %s (%s)",
             mode == CAS_UI_MODE_CASSETTE ? "CASSETTE" : "LEGACY",
             err == ESP_OK ? "saved" : esp_err_to_name(err));
}

/**
 * A（电源键）在开机早期是否可按住：IRQ 边沿状态 OR 本次开机来源为 PWRON。
 *
 * BUG-02：从关机态按住 A 开机时，PWRON 的按下边沿发生在 AXP2101 初始化
 * `axp2101_clear_irq_status()` 之前并被清掉，IRQ 任务从未收到 KEY_DOWN，
 * 于是 `axp2101_is_power_key_pressed()` 恒为 false，原实现会瞬间返回，
 * 组合键在真机上形同虚设。0x20 PWRON_STATUS 是**锁存的开机来源**（bit0
 * = 按键开机），在 IRQ 使能前就已由 PMU 锁存，不受清 IRQ 影响，可作为
 * 「用户是按住电源键开机」的可靠旁证。
 *
 * 判定口径：B 必须在开机窗口起点就按住（正常开机零延迟），A 则允许
 * 「IRQ 边沿状态」或「按键开机来源」任一成立。窗口内若 A 的 IRQ 边沿
 * 后来被捕获也仍然有效；已按下的 A 中途松开后 0x20 仍保持 bit0=1，
 * 这是本方案为兼容边沿丢失而接受的折中——B 不被松开且窗口仅 3s，
 * 误触风险极低（详见 CASSETTE_UI_FALLBACK.md）。
 */
static bool cas_a_key_held_at_boot(void)
{
    if (axp2101_is_power_key_pressed()) {
        return true;
    }
    axp2101_boot_info_t boot;
    if (axp2101_read_boot_info(&boot) != ESP_OK) {
        return false;
    }
    return (boot.pwron_status & 0x01u) != 0u;   /* 0x20 bit0 = PWRON */
}

void cassette_boot_combo_check(void)
{
    /* 瞬时判定：B 必须在开机窗口起点按住；A 见 cas_a_key_held_at_boot()。
       两者缺一即立即返回，正常开机零延迟，不阻塞 SD/LVGL 初始化。 */
    if (!cas_a_key_held_at_boot()
            || gpio_get_level(CAS_DOWN_KEY_GPIO) != 0) {
        return;
    }

    ESP_LOGW(TAG, "A+B held at boot, waiting %u ms to toggle ui_mode...",
             CAS_COMBO_HOLD_MS);
    const int64_t start_us = esp_timer_get_time();
    const int64_t need_us = (int64_t)CAS_COMBO_HOLD_MS * 1000;

    /* 开机后前 3s 轮询窗口：B 必须持续按住；A 不再作为退出条件
       （开机来源已锁存，IRQ 边沿可能丢失，见上方说明）。 */
    while (esp_timer_get_time() - start_us < need_us) {
        if (gpio_get_level(CAS_DOWN_KEY_GPIO) != 0) {
            ESP_LOGI(TAG, "B released early, keep current ui_mode");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(CAS_COMBO_POLL_MS));
    }

    /* 满 3s：toggle 一次 NVS（开机早期，LVGL 尚未开始本轮渲染）。
       A+B 是双向入口：CASSETTE -> LEGACY，LEGACY -> CASSETTE。 */
    const cas_ui_mode_t cur = cassette_ui_mode_get();
    const cas_ui_mode_t next = (cur == CAS_UI_MODE_CASSETTE)
                                   ? CAS_UI_MODE_LEGACY
                                   : CAS_UI_MODE_CASSETTE;
    ESP_LOGW(TAG, "combo complete: %s -> %s",
             cur == CAS_UI_MODE_CASSETTE ? "CASSETTE" : "LEGACY",
             next == CAS_UI_MODE_CASSETTE ? "CASSETTE" : "LEGACY");
    cassette_ui_mode_set(next);
}

bool cassette_ui_is_active(void)
{
#if defined(CONFIG_CASSETTE_UI) && CONFIG_CASSETTE_UI
    return cassette_ui_mode_get() == CAS_UI_MODE_CASSETTE;
#else
    return false;
#endif
}
