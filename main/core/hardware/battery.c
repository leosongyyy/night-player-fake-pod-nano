#include "core/hardware/battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// 电池电压采集：VBAT 通过两个 10K 电阻串联到 GND，GPIO1 接在两电阻中间 ///////////////////////////////
//////////////////// ADC 测得的电压 = VBAT / 2，因此 VBAT = ADC电压 * 2  ///////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define BAT_ADC_PIN       (GPIO_NUM_1)   // ESP32-S3: GPIO1 = ADC1 通道0
#define BAT_ADC_UNIT      (ADC_UNIT_1)
#define BAT_ADC_CHANNEL   (ADC_CHANNEL_0)
#define BAT_ADC_ATTEN     (ADC_ATTEN_DB_12)
#define BAT_DIVIDER_RATIO 2.0f           // 两个 10K 分压比，恢复真实电池电压

static adc_oneshot_unit_handle_t bat_adc_handle = NULL;
static adc_cali_handle_t bat_adc_cali_handle = NULL;
static bool bat_adc_calibrated = false;

void battery_init(void)
{
    ESP_LOGI(TAG, "Initialize ADC for battery voltage (GPIO%d)", BAT_ADC_PIN);

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &bat_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(bat_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));

    adc_cali_handle_t cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cf_cfg, &cali) == ESP_OK) {
        bat_adc_calibrated = true;
        bat_adc_cali_handle = cali;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!bat_adc_calibrated) {
        adc_cali_line_fitting_config_t lf_cfg = {
            .unit_id = BAT_ADC_UNIT,
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&lf_cfg, &cali) == ESP_OK) {
            bat_adc_calibrated = true;
            bat_adc_cali_handle = cali;
        }
    }
#endif
    if (!bat_adc_calibrated) {
        ESP_LOGW(TAG, "ADC calibration failed, will use rough raw->voltage conversion");
    } else {
        ESP_LOGI(TAG, "ADC calibration ready");
    }
}

/* 返回真实电池电压（mV）。读取失败返回 -1 */
float battery_read_voltage_mv(void)
{
    int raw = 0;
    if (adc_oneshot_read(bat_adc_handle, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return -1.0f;
    }

    int adc_mv = 0;
    if (bat_adc_calibrated) {
        adc_cali_raw_to_voltage(bat_adc_cali_handle, raw, &adc_mv);
    } else {
        adc_mv = (int)((float)raw * 3100.0f / 4095.0f);
    }
    return adc_mv * BAT_DIVIDER_RATIO;
}
