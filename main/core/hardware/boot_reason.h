#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_sleep.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机原因：主控（ESP32-S31）复位/唤醒原因 + AXP2101 本次开机来源
 *
 * 背景（2026-11，FakePodNano Pro V1）：
 *   · 主控的复位原因由 `esp_reset_reason()` 给出（POWERON / SW / EXT / …），
 *     深度睡眠/浅睡唤醒原因由 `esp_sleep_get_wakeup_causes()` 给出（本项目
 *     当前不用 deep sleep，正常情况下位图只有 UNDEFINED）；
 *   · PMU 的 **本次** 开机来源在 AXP2101 的 `0x20`（PWRON status）：bit0=按
 *     PWRON 键、bit2=插 VBUS、bit4=电池插入…（见
 *     reference_docs/pmu_axp2101.md §3.4）。该寄存器由 PMU 在**自己上电**
 *     时写入；主控 `esp_restart()` 不会复位 PMU，所以 `0x20` 仍保留着"这次
 *     PMU 开机是谁触发的"。
 *
 * 两者组合才能判断"用户是插充电器开机，还是按键开机"：
 *   · 插电开机 + 主控 POWERON 复位 → 进入**充电专用界面**（见 boot_flow）；
 *   · 按键开机（bit0）→ 正常开机；
 *   · 主控 SW 复位（ESP_RST_SW，例如充电界面短按上键后 esp_restart）→ 即使
 *     `0x20` 还写着 VBUS，也**不**再进充电界面。
 *
 * ⚠ 必须在 battery_init()（AXP2101 初始化）之后调用：本模块通过
 *   axp2101_read_boot_info() 读 PMU；PMU 不可用时 pmu_available=false，
 *   此时 boot_reason_is_charger_boot() 恒为 false（无法安全判断）。
 */

/** PMU（0x20）本次开机的触发来源（优先按键/VBUS，取最主要的那个） */
typedef enum {
    BOOT_POWER_SRC_UNKNOWN = 0,   /* PMU 不可用 / 没有可识别来源 */
    BOOT_POWER_SRC_KEY,           /* 按住 PWRON 超过 ONLEVEL */
    BOOT_POWER_SRC_VBUS,          /* 插入 VBUS（充电器） */
    BOOT_POWER_SRC_BATTERY,       /* 电池插入且电压正常 */
    BOOT_POWER_SRC_IRQ,           /* IRQ 脚拉低 */
    BOOT_POWER_SRC_EN,            /* EN 模式（PWRON 恒高） */
    BOOT_POWER_SRC_OTHER,         /* 其它/未定义组合 */
} boot_power_source_t;

/** 一次开机原因的完整快照（boot_reason_init 时采样一次，之后只读） */
typedef struct {
    esp_reset_reason_t       reset_reason;   /* 主控复位原因 */
    esp_sleep_wakeup_cause_t wakeup_cause;   /* 深度睡眠唤醒原因（本项目恒 UNDEFINED） */
    uint32_t                 wakeup_causes;  /* 唤醒来源位图（esp_sleep_get_wakeup_causes） */
    bool     pmu_available;                  /* AXP2101 是否可读 */
    uint8_t  pwron_status;                   /* 0x20 原始值 */
    uint8_t  pwr_off_status;                 /* 0x21 上次关机原因原始值 */
    bool     pmu_key;                        /* 0x20[0] PWRON 按键开机 */
    bool     pmu_vbus;                       /* 0x20[2] VBUS 插入开机 */
    bool     pmu_battery;                    /* 0x20[4] 电池插入开机 */
    bool     pmu_irq;                        /* 0x20[1] IRQ 脚拉低开机 */
    bool     pmu_en;                         /* 0x20[5] EN 模式 */
    boot_power_source_t source;              /* 归并后的主要开机来源 */
} boot_reason_t;

/**
 * @brief 采样并打印本次开机原因（在 battery_init() 之后调用一次）
 *
 * 只读，不改变任何硬件状态。日志包含：主控复位原因、深度睡眠唤醒原因、
 * PMU 0x20/0x21 原始值与解析结果，以及"是否进入充电专用界面"的判定结论。
 */
void boot_reason_init(void);

/** @brief 取开机原因快照（boot_reason_init 之前调用返回全 0/UNKNOWN） */
const boot_reason_t *boot_reason_get(void);

/**
 * @brief 本次是否属于"插充电器开机"（应进入充电专用界面）
 *
 * 判定（全部满足才为 true）：
 *   1. AXP2101 可读，且 `0x20` 的 bit2（VBUS 插入开机）置位；
 *   2. `0x20` 的 bit0（PWRON 按键开机）**没有**置位——用户同时按了键就不算
 *      "纯插电开机"，按正常开机处理，避免被锁在充电界面；
 *   3. 主控复位原因是"上电类"复位：`ESP_RST_POWERON`（电源上电）或
 *      `ESP_RST_EXT`（外部复位脚释放；本板 PMU 的 PWROK 接在主控复位/EN 上，
 *      实测可能报这个）。**不是** `ESP_RST_SW`（esp_restart/充电界面短按上键
 *      后重启）、`ESP_RST_DEEPSLEEP`（GPIO/定时器唤醒）或 panic/WDT 复位；
 *   4. 深度睡眠唤醒位图里没有除 `UNDEFINED` 以外的来源（不是 deep/light
 *      sleep 唤醒）。
 */
bool boot_reason_is_charger_boot(void);

/** @brief 复位原因的可读名字（日志用；未知返回 "OTHER"） */
const char *boot_reason_reset_name(esp_reset_reason_t reason);

/** @brief 唤醒原因的可读名字（日志用；未知返回 "OTHER"） */
const char *boot_reason_wakeup_name(esp_sleep_wakeup_cause_t cause);

/** @brief 开机来源的可读名字（日志用） */
const char *boot_reason_source_name(boot_power_source_t source);

#ifdef __cplusplus
}
#endif
