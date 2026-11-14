#include "core/hardware/battery.h"

#include "core/hardware/touch.h"   /* touch_get_i2c_bus()：I²C0 总线属于触摸驱动 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";

/* 百分比融合阈值：gauge 与电压估算偏差超过该值就认为 E-Gauge 不可信
   （参考文档 §6.3 建议 8%） */
#define BATT_FUSE_MAX_DIFF   8

/* 充电中未充满时的显示上限：E-Gauge 常在 CV 阶段就报 100%，直接上屏会让
   用户以为已经充满（充到 100% 后还有一段恒压涓流），只有阶段 DONE 才给 100 */
#define BATT_CHG_MAX_DISPLAY 99

/* 电池健康保护（4.0V 限充）阈值：充到 4.0V 停、放电到 3.9V 恢复。
   ⚠ 为什么不用 CV=4.0V：PMU 在 VBAT **高于** CV 时会断开 BATFET（充电完成保护），
     此时拔掉充电器 VSYS 没有来源 → DCDC 欠压保护硬断电（真机 0x21=0x20 已证实）。
     保持 CV=4.2V + 软件停充（0x18[1]=0），PMU 不会进入"完成"态，BATFET 常闭，
     正常使用拔电不会关机。 */
#define BATT_CARE_STOP_MV    4000u
#define BATT_CARE_RESUME_MV  3900u

static bool              s_care_on = false;         /* 保护功能是否开启 */
static bool              s_charge_stopped = false;  /* 是否由保护功能停充 */
static SemaphoreHandle_t s_care_mux = NULL;         /* 保护状态互斥（LVGL/电源任务都会碰） */

void battery_init(void)
{
    /* 保护状态的互斥锁：设置页（LVGL 任务）切换与电源任务 tick 可能并发 */
    if (s_care_mux == NULL) {
        s_care_mux = xSemaphoreCreateMutex();
    }

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

/* 设置/取消软件停充；成功才更新缓存状态 */
static esp_err_t care_set_stopped(bool stop)
{
    esp_err_t err = axp2101_set_charge_enable(!stop);
    if (err == ESP_OK) {
        s_charge_stopped = stop;
    }
    return err;
}

esp_err_t battery_set_health_protection(bool on)
{
    if (!axp2101_is_ready()) {
        ESP_LOGW(TAG, "battery health protection: PMU unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_care_mux != NULL) {
        xSemaphoreTake(s_care_mux, portMAX_DELAY);
    }

    s_care_on = on;
    /* CV 始终保持在 4.2V：PMU 的充电目标电压不能低于当前电池电压，
       限充由软件写 0x18[1]=0 完成（见 BATT_CARE_STOP_MV 注释）。 */
    esp_err_t err = axp2101_set_charge_voltage(AXP2101_CV_4V2);
    uint16_t mv = 0;
    if (err == ESP_OK && axp2101_read_vbat_mv(&mv) == ESP_OK) {
        err = care_set_stopped(on && mv >= BATT_CARE_STOP_MV);
    } else if (err == ESP_OK) {
        /* 读不到电压：保守起见恢复充电（关闭保护时也必须恢复充电） */
        err = care_set_stopped(false);
    }

    if (s_care_mux != NULL) {
        xSemaphoreGive(s_care_mux);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "battery health protection %s (CV 4.2V, stop at %u mV, now %s)",
                 on ? "ON" : "OFF", (unsigned)BATT_CARE_STOP_MV,
                 s_charge_stopped ? "charging stopped" : "charging enabled");
    }
    return err;
}

bool battery_health_protection_is_stopped(void)
{
    /* 两个 bool 的读取无需加锁（人类操作频率，最坏情况是晚 1s 反映） */
    return s_care_on && s_charge_stopped;
}

void battery_health_protection_tick(void)
{
    if (!axp2101_is_ready() || s_care_mux == NULL) {
        return;
    }
    /* 快速路径：保护关闭且没有被停充过，就不用每秒读 I²C */
    if (!s_care_on && !s_charge_stopped) {
        return;
    }
    if (xSemaphoreTake(s_care_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    uint16_t mv = 0;
    if (axp2101_read_vbat_mv(&mv) == ESP_OK) {
        if (s_care_on) {
            if (!s_charge_stopped && mv >= BATT_CARE_STOP_MV) {
                ESP_LOGI(TAG, "battery care: %u mV >= %u mV -> stop charging",
                         (unsigned)mv, (unsigned)BATT_CARE_STOP_MV);
                care_set_stopped(true);
            } else if (s_charge_stopped && mv <= BATT_CARE_RESUME_MV) {
                ESP_LOGI(TAG, "battery care: %u mV <= %u mV -> resume charging",
                         (unsigned)mv, (unsigned)BATT_CARE_RESUME_MV);
                care_set_stopped(false);
            }
        } else if (s_charge_stopped) {
            /* 保护已关闭但充电仍停着（并发/异常）：恢复充电 */
            care_set_stopped(false);
        }
    }

    xSemaphoreGive(s_care_mux);
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
