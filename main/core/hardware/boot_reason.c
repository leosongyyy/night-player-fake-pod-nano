#include "core/hardware/boot_reason.h"

#include <inttypes.h>
#include <string.h>

#include "core/hardware/axp2101.h"
#include "esp_log.h"

static const char *TAG = "boot_reason";

static boot_reason_t s_reason = {
    .reset_reason = ESP_RST_UNKNOWN,
    .wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED,
    .source = BOOT_POWER_SRC_UNKNOWN,
};

const char *boot_reason_reset_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT_PIN";
    case ESP_RST_SW:         return "SW_RESET";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "OTHER";
    }
}

const char *boot_reason_wakeup_name(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED(not deep sleep)";
    case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:       return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART0:     return "UART0";
    case ESP_SLEEP_WAKEUP_UART1:     return "UART1";
    case ESP_SLEEP_WAKEUP_UART2:     return "UART2";
    case ESP_SLEEP_WAKEUP_BT:        return "BT";
    default:                         return "OTHER";
    }
}

const char *boot_reason_source_name(boot_power_source_t src)
{
    switch (src) {
    case BOOT_POWER_SRC_KEY:     return "PWRON/key";
    case BOOT_POWER_SRC_VBUS:    return "VBUS/charger";
    case BOOT_POWER_SRC_BATTERY: return "battery-insert";
    case BOOT_POWER_SRC_IRQ:     return "IRQ-pin";
    case BOOT_POWER_SRC_EN:      return "EN-mode";
    case BOOT_POWER_SRC_OTHER:   return "other";
    case BOOT_POWER_SRC_UNKNOWN:
    default:                     return "unknown";
    }
}

/* 由 0x20 的位组合归并出"主要开机来源"：按键优先于 VBUS（两者同时置位说明
   用户也按了电源键，按正常开机处理），其余来源按 VBUS > 电池 > IRQ > EN 排。 */
static boot_power_source_t boot_source_from_pwron(uint8_t pwron_status)
{
    if (pwron_status & 0x01u) {          /* bit0 PWRON 长按到 ONLEVEL */
        return BOOT_POWER_SRC_KEY;
    }
    if (pwron_status & 0x04u) {          /* bit2 VBUS 插入 */
        return BOOT_POWER_SRC_VBUS;
    }
    if (pwron_status & 0x10u) {          /* bit4 电池插入 */
        return BOOT_POWER_SRC_BATTERY;
    }
    if (pwron_status & 0x02u) {          /* bit1 IRQ 拉低 */
        return BOOT_POWER_SRC_IRQ;
    }
    if (pwron_status & 0x20u) {          /* bit5 EN 模式 */
        return BOOT_POWER_SRC_EN;
    }
    if (pwron_status != 0) {
        return BOOT_POWER_SRC_OTHER;
    }
    return BOOT_POWER_SRC_UNKNOWN;
}

/* 把 esp_sleep_get_wakeup_causes() 的位图归并成"一个"可读原因：
   未从 sleep 唤醒时位图只有 BIT(ESP_SLEEP_WAKEUP_UNDEFINED)，即 UNDEFINED；
   否则取最低置位（与旧版 esp_sleep_get_wakeup_cause 的语义一致）。 */
static esp_sleep_wakeup_cause_t wakeup_cause_from_bitmap(uint32_t causes)
{
    if (causes == 0) {
        return ESP_SLEEP_WAKEUP_UNDEFINED;
    }
    for (int bit = 0; bit < 32; bit++) {
        if (causes & (1u << bit)) {
            return (esp_sleep_wakeup_cause_t)bit;
        }
    }
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}

void boot_reason_init(void)
{
    memset(&s_reason, 0, sizeof(s_reason));
    s_reason.reset_reason = esp_reset_reason();
    /* IDF 6.1 起 esp_sleep_get_wakeup_cause() 被标记 deprecated（只返回一个来源），
       改用返回位图的新接口，再归并成一个可读原因。 */
    s_reason.wakeup_causes = esp_sleep_get_wakeup_causes();
    s_reason.wakeup_cause  = wakeup_cause_from_bitmap(s_reason.wakeup_causes);
    s_reason.pmu_available = axp2101_is_ready();

    if (s_reason.pmu_available) {
        axp2101_boot_info_t info;
        if (axp2101_read_boot_info(&info) == ESP_OK) {
            s_reason.pwron_status   = info.pwron_status;
            s_reason.pwr_off_status = info.pwr_off_status;
            s_reason.pmu_key     = (info.pwron_status & 0x01u) != 0;
            s_reason.pmu_vbus    = (info.pwron_status & 0x04u) != 0;
            s_reason.pmu_battery = (info.pwron_status & 0x10u) != 0;
            s_reason.pmu_irq     = (info.pwron_status & 0x02u) != 0;
            s_reason.pmu_en      = (info.pwron_status & 0x20u) != 0;
            s_reason.source      = boot_source_from_pwron(info.pwron_status);
        } else {
            ESP_LOGW(TAG, "read PMU boot info failed");
            s_reason.pmu_available = false;
        }
    }

    ESP_LOGI(TAG, "chip reset reason: %s (%d)",
             boot_reason_reset_name(s_reason.reset_reason), (int)s_reason.reset_reason);
    ESP_LOGI(TAG, "chip wakeup cause: %s (%d), bitmap=0x%08" PRIx32,
             boot_reason_wakeup_name(s_reason.wakeup_cause), (int)s_reason.wakeup_cause,
             s_reason.wakeup_causes);

    if (s_reason.pmu_available) {
        ESP_LOGI(TAG, "PMU PWRON status=0x%02X => source=%s "
                      "[key=%d vbus=%d batt=%d irq=%d en=%d]",
                 s_reason.pwron_status, boot_reason_source_name(s_reason.source),
                 (int)s_reason.pmu_key, (int)s_reason.pmu_vbus,
                 (int)s_reason.pmu_battery, (int)s_reason.pmu_irq,
                 (int)s_reason.pmu_en);
        ESP_LOGI(TAG, "PMU last PWROFF status=0x%02X "
                      "[soft=%d keylong=%d vsys_uv=%d vbus_ov=%d]",
                 s_reason.pwr_off_status,
                 (int)((s_reason.pwr_off_status >> 1) & 1u),
                 (int)(s_reason.pwr_off_status & 1u),
                 (int)((s_reason.pwr_off_status >> 3) & 1u),
                 (int)((s_reason.pwr_off_status >> 4) & 1u));
    } else {
        ESP_LOGW(TAG, "PMU unavailable: cannot determine key/charger boot source");
    }

    ESP_LOGI(TAG, "charge-only boot: %s",
             boot_reason_is_charger_boot()
                 ? "YES (PMU VBUS power-on + chip power-on reset) -> charging UI"
                 : "NO (normal boot)");
}

const boot_reason_t *boot_reason_get(void)
{
    return &s_reason;
}

bool boot_reason_is_charger_boot(void)
{
    if (!s_reason.pmu_available) {
        return false;
    }
    /* 只认"纯插电开机"：VBUS 是 PMU 的开机来源，且用户没有同时按电源键 */
    if (!s_reason.pmu_vbus || s_reason.pmu_key) {
        return false;
    }
    /* 主控必须是"上电类"复位：ESP_RST_POWERON（电源上电）或 ESP_RST_EXT
       （外部复位脚释放——本板 PMU 的 PWROK 接在主控复位/EN 上，实际可能报
       这个）。esp_restart()（SW 复位）、deep/light sleep 唤醒、panic/WDT 都
       不算——充电界面里"短按上键进入系统"正是靠 esp_restart 让这里变成
       false，从而跳过充电界面。 */
    const bool power_on_like = (s_reason.reset_reason == ESP_RST_POWERON)
                               || (s_reason.reset_reason == ESP_RST_EXT);
    if (!power_on_like) {
        return false;
    }
    /* 不能是 sleep 唤醒：未从 sleep 唤醒时位图只有 BIT(UNDEFINED) */
    if (s_reason.wakeup_causes != (1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        return false;
    }
    return true;
}
