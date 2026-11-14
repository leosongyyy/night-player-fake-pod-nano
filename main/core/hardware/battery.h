#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/hardware/axp2101.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电池信息（应用层）：AXP2101 电压/电量计 + 充放电状态
 *
 * 数据来源（FakePodNano Pro V1）：
 *   · 电池电压：AXP2101 的 14-bit ADC（0x34/0x35，1 mV/LSB，无分压电阻误差）；
 *   · 电量百分比：E-Gauge 的 0xA4（0~100）+ 电压兜底融合（见 battery_fuse_percent）；
 *   · 充放电状态：0x01[6:5] 电流方向 + 0x01[2:0] 充电阶段；
 *   · 是否插 USB：0x00[5] VBUS 良好。
 *
 * ⚠ E-Gauge 没有为本板 600 mAh 电芯做过模型标定，陌生电池上 0xA4 可能严重
 *   偏离（参考文档 §6.2），所以**不能直接把 0xA4 当显示值**：偏差大、或
 *   充电状态特殊时改走 OCV 电压表（axp2101_soc_from_mv）。
 * ⚠ S3 时代的"10K+10K 分压 + ADC 校准"实现已删除（S31 没有那条分压链路）。
 */

/* 电量百分比估算区间（锂电池，单位 mV），保留给低电策略/UI 颜色分档使用 */
#define BAT_EMPTY_MV      3000
#define BAT_FULL_MV       4200

/* 临界低电压（mV）：低于此值视为"电池电压过低"（约对应 OCV 表 4%） */
#define BATTERY_CRITICAL_LOW_MV 3450

/** 一次完整电池快照（UI 状态栏/控制中心一次读取即可） */
typedef struct {
    bool     available;             /* AXP2101 是否就绪 */
    bool     vbus_present;          /* 是否插着 USB/VBUS */
    bool     battery_present;       /* 电池是否在位（0x00[3]） */
    bool     charging;              /* 是否正在充电（方向或阶段） */
    uint16_t vbat_mv;               /* 电池电压（mV） */
    uint16_t vsys_mv;               /* 系统电压（mV，诊断用；0=读失败） */
    uint8_t  gauge_percent;         /* 电量计原值 0xA4；0xFF = 不可用 */
    uint8_t  percent;               /* 融合后的展示值 0~100；0xFF = 不可用 */
    bool     percent_from_voltage;  /* true = 本次展示值来自电压兜底（gauge 不可信） */
    axp2101_charge_stage_t  charge_stage;   /* 充电阶段 */
    axp2101_current_dir_t   current_dir;    /* 电流方向 */
} battery_info_t;

/**
 * @brief 初始化电池模块（内部初始化 AXP2101，复用触摸建立的 I²C0 总线）
 *
 * 必须在 touch_init() 之后调用。AXP2101 不在/总线异常时只打印警告，
 * battery_is_available() 返回 false，UI 显示 "--%"。
 */
void battery_init(void);

/** @brief AXP2101 是否可用（init 成功且芯片应答过） */
bool battery_is_available(void);

/**
 * @brief 读取电池电压
 *
 * @return 电压值（mV）；PMU 不可用/读取失败返回 -1.0f
 */
float battery_read_voltage_mv(void);

/**
 * @brief 读取融合后的电量百分比（0~100）
 *
 * @return 百分比；PMU 不可用/读取失败返回 0xFF
 */
uint8_t battery_read_percent(void);

/** @brief 是否正在充电（PMU 不可用时返回 false） */
bool battery_is_charging(void);

/** @brief 读取完整电池快照（含电压/电量/充电状态） */
esp_err_t battery_read_info(battery_info_t *info);
/* ⚠ 不提供电池温度：本板 TS 脚是固定电阻（无 NTC），读数恒定无意义，
   设置页已移除「电池温度」一项（见 axp2101_read_ts_mv 的说明）。 */

/**
 * @brief 应用/取消"电池健康保护"（充电到约 4.0V 后软件停充）
 *
 * 开启：CV 保持 **4.2V**，但在 VBAT ≥ 4.0V 时写 `0x18[1]=0` 停充；
 *       放电到 VBAT ≤ 3.9V 时自动恢复充电（见 battery_health_protection_tick）。
 *       ≈ 75~80% 电量，充电/恢复仍由本模块与 PMU 配合完成。
 * 关闭：恢复充电，CV 4.2V，标准满电。
 *
 * ⚠ **不能改成"把 CV 降到 4.0V"**：当电池电压高于 CV 时 PMU 会断开 BATFET
 *   （充电完成保护），此时拔掉充电器 VSYS 无来源 → DCDC 欠压保护硬断电。
 *   真机已复现（电池 4.17V、CV 4.0V，拔电后 0x21=0x20 DCDC_UV）。
 * ⚠ PMU 每次上电都会恢复 EFUSE 默认，所以开机时必须按 NVS 配置重新下发一次。
 *
 * @param on true=4.0V 限充；false=标准满电
 * @return ESP_OK / PMU 错误
 */
esp_err_t battery_set_health_protection(bool on);

/**
 * @brief 电池健康保护的周期检查（建议 1s 调一次）
 *
 * 开通时：≥4.0V 停充、≤3.9V 恢复；关闭时兜底确保充电已恢复。
 * 只在保护开启/停充状态下才读 I²C，其余时候几乎零开销。
 * ⚠ 可从任意任务调用（内部互斥 + I²C 不写 flash）；正常使用时由
 *   `power_service` 的 1s 守护任务调用，充电界面由 `chg_mon` 调用。
 */
void battery_health_protection_tick(void);

/**
 * @brief 当前是否处于"保护性停止充电"状态（保护开启 且 已到 4.0V 停充）
 *
 * 充电专用界面用它把大环画成**完整一圈**表示"充电已停止"（而不是按实际
 * 电量画弧），同时环中心仍显示实际电量。保护关闭/未停充时返回 false。
 */
bool battery_health_protection_is_stopped(void);

/* ---------------------------- 纯函数（可自检） -------------------------- */

/**
 * @brief 融合 E-Gauge 百分比与电压估算（参考文档 §6.3 的判据层）
 *
 * 规则：
 *   · 充电中：以 gauge 为主，未充满时上限锁 99%，阶段 DONE 才给 100%；
 *   · 放电/待机：gauge 与电压估算偏差 ≤8% 时按 0.6×gauge + 0.4×电压加权；
 *     偏差 >8%，或 gauge 处于 0/100 而电压明显不在两端 → 视为电量计不可信，
 *     只用电压表；
 *   · gauge 越界（>100）直接走电压表。
 *
 * @param gauge      电量计原值（0xFF 表示不可用）
 * @param vbat_mv    电池电压（mV）
 * @param charging   是否充电中
 * @param stage      充电阶段（仅充电时用于判断"已充满"）
 * @param used_voltage 输出：本次是否走了电压兜底（可为 NULL）
 * @return 0~100 的展示百分比
 */
uint8_t battery_fuse_percent(uint8_t gauge, uint16_t vbat_mv, bool charging,
                             axp2101_charge_stage_t stage, bool *used_voltage);

#ifdef __cplusplus
}
#endif
