#include "core/hardware/battery.h"

#include "core/hardware/touch.h"   /* touch_get_i2c_bus()：I²C0 总线属于触摸驱动 */
#include "esp_log.h"

static const char *TAG = "battery";

/* 百分比融合阈值：gauge 与电压估算偏差超过该值就认为 E-Gauge 不可信
   （参考文档 §6.3 建议 8%） */
#define BATT_FUSE_MAX_DIFF   8

/* 充电中未充满时的显示上限：E-Gauge 常在 CV 阶段就报 100%，直接上屏会让
   用户以为已经充满（充到 100% 后还有一段恒压涓流），只有阶段 DONE 才给 100 */
#define BATT_CHG_MAX_DISPLAY 99

void battery_init(void)
{
    esp_err_t err = axp2101_init(touch_get_i2c_bus());
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "battery: AXP2101 ready (voltage + gauge + charge status)");
    } else {
        /* 不致命：电池信息不可用时设备仍要能开机放歌，UI 显示 "--%" */
        ESP_LOGW(TAG, "battery: AXP2101 unavailable (%s), percent/voltage disabled",
                 esp_err_to_name(err));
    }
}

bool battery_is_available(void)
{
    return axp2101_is_ready();
}

float battery_read_voltage_mv(void)
{
    uint16_t mv = 0;
    if (axp2101_read_vbat_mv(&mv) != ESP_OK) {
        return -1.0f;
    }
    return (float)mv;
}

esp_err_t battery_read_info(battery_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    info->available = false;
    info->percent = 0xFF;

    if (!axp2101_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    axp2101_status_t st;
    esp_err_t err = axp2101_read_status(&st);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read PMU status failed: %s", esp_err_to_name(err));
        return err;
    }

    bool used_voltage = false;
    uint8_t percent = battery_fuse_percent(st.soc_percent, st.vbat_mv, st.charging,
                                           st.charge_stage, &used_voltage);

    info->available            = true;
    info->vbus_present         = st.vbus_present;
    info->battery_present      = st.battery_present;
    info->charging             = st.charging;
    info->vbat_mv              = st.vbat_mv;
    info->vsys_mv              = st.vsys_mv;
    info->gauge_percent        = st.soc_percent;
    info->percent              = percent;
    info->percent_from_voltage = used_voltage;
    info->charge_stage         = st.charge_stage;
    info->current_dir          = st.current_dir;

    ESP_LOGD(TAG, "vbat=%umV vsys=%umV gauge=%u%% -> %u%%%s (%s/%s)",
             st.vbat_mv, st.vsys_mv, st.soc_percent, percent,
             used_voltage ? " [voltage fallback]" : "",
             axp2101_current_dir_name(st.current_dir),
             axp2101_charge_stage_name(st.charge_stage));
    return ESP_OK;
}

uint8_t battery_read_percent(void)
{
    battery_info_t info;
    if (battery_read_info(&info) != ESP_OK) {
        return 0xFF;
    }
    return info.percent;
}

bool battery_is_charging(void)
{
    if (!axp2101_is_ready()) {
        return false;
    }
    axp2101_status_t st;
    if (axp2101_read_status(&st) != ESP_OK) {
        return false;
    }
    return st.charging;
}

uint8_t battery_fuse_percent(uint8_t gauge, uint16_t vbat_mv, bool charging,
                             axp2101_charge_stage_t stage, bool *used_voltage)
{
    if (used_voltage != NULL) {
        *used_voltage = false;
    }

    const uint8_t v_soc = axp2101_soc_from_mv(vbat_mv);
    const bool gauge_ok = (gauge <= 100);

    if (charging) {
        /* 充电中电压被抬高、且 CV 阶段电压几乎不动，此时电压法毫无意义，
           以电量计为主。充满（阶段 DONE）才允许显示 100%。 */
        if (stage == AXP2101_CHG_DONE) {
            return 100;
        }
        if (!gauge_ok) {
            /* 电量计也读不到时只能给电压兜底，但充电中电压偏高会虚高，
               所以再压到 99% 以内，避免"充电中直接显示 100" */
            if (used_voltage != NULL) {
                *used_voltage = true;
            }
            return (v_soc > BATT_CHG_MAX_DISPLAY) ? BATT_CHG_MAX_DISPLAY : v_soc;
        }
        return (gauge > BATT_CHG_MAX_DISPLAY) ? BATT_CHG_MAX_DISPLAY : gauge;
    }

    if (!gauge_ok) {
        if (used_voltage != NULL) {
            *used_voltage = true;
        }
        return v_soc;
    }

    const int diff = (int)gauge - (int)v_soc;
    const bool gauge_endpoint_suspect =
        ((gauge == 0 && vbat_mv > 3600) || (gauge == 100 && vbat_mv < 4000));

    if (diff > BATT_FUSE_MAX_DIFF || diff < -BATT_FUSE_MAX_DIFF || gauge_endpoint_suspect) {
        /* 偏差过大 / 端点明显对不上电压：电量计不可信，改用 OCV 电压表 */
        if (used_voltage != NULL) {
            *used_voltage = true;
        }
        return v_soc;
    }

    /* 两者都在可信区间：0.6×gauge + 0.4×电压（四舍五入） */
    unsigned mixed = (unsigned)gauge * 6u + (unsigned)v_soc * 4u;
    return (uint8_t)((mixed + 5u) / 10u);
}
