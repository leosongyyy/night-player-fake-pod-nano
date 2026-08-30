#include "core/hardware/touch.h"
#include "core/hardware/display.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "touch";

#define TOUCH_HOST  I2C_NUM_0

#if EXAMPLE_USE_TOUCH

// 根据TOUCH_IC_CONFIG的配置，选择调用的头文件
#if TOUCH_IC_CONFIG == CST816_ID
#include "esp_lcd_touch_cst816.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CST816_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_cst816

#elif TOUCH_IC_CONFIG == CST820_ID
#include "esp_lcd_touch_cst820.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_cst820

#elif TOUCH_IC_CONFIG == CHSC6417_ID
#include "esp_lcd_touch_chsc6417.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CHSC6417_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_chsc6417
#endif

static esp_lcd_touch_handle_t tp = NULL;
static TaskHandle_t s_touch_init_task = NULL;   /* 触摸异步初始化任务句柄 */
static volatile bool s_touch_ready = false;     /* 触摸初始化是否已完成 */

/* 触摸初始化完成回调（由初始化任务完成后调用，可经 touch_register_ready_cb 注册） */
static touch_ready_cb_t s_ready_cb = NULL;
static void *s_ready_ctx = NULL;

/* 触摸异步初始化任务：执行耗时的控制器完整初始化（含约 400ms 复位等待），
   不阻塞开机流程；完成后通知已注册的回调（如 LVGL 启用触摸输入）。 */
static void touch_init_task(void *arg)
{
    esp_lcd_panel_io_handle_t tp_io_handle = (esp_lcd_panel_io_handle_t)arg;

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES + 20,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller (background)");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c(tp_io_handle, &tp_cfg, &tp));
    s_touch_ready = true;

    ESP_LOGI(TAG, "Touch controller ready");
    if (s_ready_cb != NULL) {
        s_ready_cb(tp, s_ready_ctx);
    }
    vTaskDelete(NULL);
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(TOUCH_HOST, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan completed.");
}

esp_lcd_touch_handle_t touch_init(void)
{
    ESP_LOGI(TAG, "Initialize I2C bus");
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200 * 1000,  // 200kHz：400kHz 在 TXS0102 上长时间工作不可靠；100kHz 实测反而更易触发切换后卡死，故用 200kHz
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_HOST, &i2c_conf));
    // 设置 I2C 超时时间
    ESP_ERROR_CHECK(i2c_set_timeout(TOUCH_HOST, 0x02)); // 0x02: 2ms
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0));

    // Scan I2C devices
    i2c_scan();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = TOUCH_IO_I2C_CONFIG();
    // Attach the TOUCH to the I2C bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle));

    // 异步初始化触摸控制器：控制器完整初始化（含约 400ms 复位等待）放到
    // 后台任务执行，不阻塞后续硬件/UI 初始化。就绪后调用 ready 回调
    //（上层注册，如把句柄交给 LVGL），在此之前触摸回调不给数据。
    BaseType_t ret = xTaskCreate(touch_init_task, "touch_init", 4096, tp_io_handle, 5, &s_touch_init_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch init task");
        return NULL;
    }

    // 初始化进行中：返回 NULL，LVGL 触摸输入设备在就绪前不提供数据
    return NULL;
}

void touch_register_ready_cb(touch_ready_cb_t cb, void *ctx)
{
    s_ready_cb = cb;
    s_ready_ctx = ctx;
    /* 若触摸已完成初始化（极端情况：回调注册晚于任务完成），立即补发一次 */
    if (s_touch_ready && cb != NULL) {
        cb(tp, ctx);
    }
}

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return tp;
}

void touch_power_off(void)
{
    /* 若触摸初始化任务仍在执行（开机极早期触发 deep sleep），先等它完成，
       避免 i2c_driver_delete 与任务内的 I2C 复位/读 ID 操作冲突 */
    if (s_touch_init_task != NULL && !s_touch_ready) {
        ESP_LOGI(TAG, "touch init task still running, waiting...");
        while (!s_touch_ready) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* 触摸控制器复位：RST 拉低进入复位低功耗 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(EXAMPLE_PIN_NUM_TOUCH_RST, 0);

    /* 释放 I2C 总线驱动（CS43131 已关机、触摸已复位，总线不再使用；
       引脚回到高阻输入，外部上拉无电流路径，deepsleep 中不额外耗电。
       唤醒后 touch_init() 会重新安装驱动） */
    i2c_driver_delete(TOUCH_HOST);
    ESP_LOGI(TAG, "touch powered off (RST low, I2C driver released)");
}

#else // !EXAMPLE_USE_TOUCH

esp_lcd_touch_handle_t touch_init(void)
{
    return NULL;
}

void touch_register_ready_cb(touch_ready_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
}

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return NULL;
}

#endif // EXAMPLE_USE_TOUCH
