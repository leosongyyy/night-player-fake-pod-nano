#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AXP2101 单节锂电 PMU（电源管理 + 充电 + 电量计）寄存器驱动
 *
 * 硬件连接（FakePodNano Pro V1，见 ideas_and_exps/reference_docs/pmu_axp2101.md）：
 *   · I²C0：SDA=GPIO48 / SCL=GPIO47 / 400 kHz，7-bit 地址 **0x34**；
 *     与 CST820(0x15)、PCF85063A(0x51)、QMI8658(0x6B) 共用总线，
 *     总线句柄由 touch_init() 建立并经 touch_get_i2c_bus() 暴露。
 *   · IRQ：GPIO49，开漏低有效，下降沿触发（本驱动只把"电源键"事件接到中断，
 *     其余充电/插拔事件由上层轮询状态寄存器，见 axp2101_read_status）。
 *   · 上方电源键接芯片 PWRON，主控没有该键的 GPIO。
 *
 * ⚠ 本文件只做"寄存器 ↔ 语义"，不依赖 LVGL、不建业务任务（按键事件任务除外，
 *   由 CONFIG_PMU_KEY_IRQ 控制），也不碰任何电源轨寄存器（0x80-0x86 / 0x90-0x9A
 *   是板厂 EFUSE 默认值，写错会直接黑屏/不开机）。
 */

#define AXP2101_I2C_ADDR          0x34   /* 7-bit 从地址（datasheet 8-bit 0x68/0x69） */
#define AXP2101_I2C_TIMEOUT_MS    100    /* 单次 I²C 事务超时 */
#define AXP2101_INT_GPIO          GPIO_NUM_49  /* PMU IRQ，开漏低有效 */

/* ------------------------------ 寄存器地址 ------------------------------ */
/* 完整表见 reference_docs/pmu_axp2101.md §4；这里只列本驱动用到的。 */
#define AXP2101_REG_STATUS1       0x00   /* VBUS/BATFET/电池在位等 */
#define AXP2101_REG_STATUS2       0x01   /* 电流方向 + 充电阶段 */
#define AXP2101_REG_COMMON_CFG    0x10   /* [0] 软关机 [1] PMU 重启 [2] PWRON 16s 强制断 */
#define AXP2101_REG_GAUGE_CFG     0x18   /* [3] 电量计使能（默认 1，只读确认） */
#define AXP2101_REG_LOW_BAT_TH    0x1A   /* 低电告警阈值 L1/L2 */
#define AXP2101_REG_PWRON_STATUS  0x20   /* 本次开机来源（只读） */
#define AXP2101_REG_PWROFF_STATUS 0x21   /* 上次关机原因（只读） */
#define AXP2101_REG_PWROFF_EN     0x22   /* [1] PWRON 长按关机使能 [0] 0=关机/1=重启 */
#define AXP2101_REG_KEY_LEVEL     0x27   /* [5:4] IRQLEVEL [3:2] OFFLEVEL [1:0] ONLEVEL */
#define AXP2101_REG_ADC_EN        0x30   /* ADC 通道使能（[0] VBAT 默认开） */
#define AXP2101_REG_VBAT_H        0x34   /* 电池电压高字节（读它会锁存 ADC） */
#define AXP2101_REG_VBAT_L        0x35   /* 电池电压低字节（必须先读高字节） */
#define AXP2101_REG_TS_H          0x36   /* 电池温度 TS 高字节 */
#define AXP2101_REG_TS_L          0x37
#define AXP2101_REG_VBUS_H        0x38   /* VBUS 电压高字节 */
#define AXP2101_REG_VBUS_L        0x39
#define AXP2101_REG_VSYS_H        0x3A   /* 系统电压高字节 */
#define AXP2101_REG_VSYS_L        0x3B
#define AXP2101_REG_TDIE_H        0x3C   /* 结温高字节 */
#define AXP2101_REG_TDIE_L        0x3D
#define AXP2101_REG_IRQ_EN0       0x40   /* 中断使能：电量/温度 */
#define AXP2101_REG_IRQ_EN1       0x41   /* 中断使能：插拔/电源键 */
#define AXP2101_REG_IRQ_EN2       0x42   /* 中断使能：系统/充电 */
#define AXP2101_REG_IRQ_ST0       0x48   /* 中断状态（RW1C：写 1 清除） */
#define AXP2101_REG_IRQ_ST1       0x49
#define AXP2101_REG_IRQ_ST2       0x4A
#define AXP2101_REG_TS_CTRL       0x50   /* [4] TS 作为固定输入、不参与充电判定 */
#define AXP2101_REG_IPRE          0x61   /* 预充电流 */
#define AXP2101_REG_ICC           0x62   /* [4:0] 恒流充电电流 */
#define AXP2101_REG_ITERM         0x63   /* [4] 截止使能 [3:0] 截止电流档位 */
#define AXP2101_REG_CV            0x64   /* [2:0] 恒压充电电压 */
#define AXP2101_REG_CHG_TIMER     0x67   /* 充电安全定时器 */
#define AXP2101_REG_SOC           0xA4   /* 电量百分比 0~100（只读，E-Gauge） */

/* IRQ_EN1 / IRQ_ST1（0x41 / 0x49）里的电源键位 */
#define AXP2101_IRQ_KEY_SHORT     (1u << 3)   /* 短按 */
#define AXP2101_IRQ_KEY_LONG      (1u << 2)   /* 长按（IRQLEVEL 分界） */
#define AXP2101_IRQ_KEY_DOWN      (1u << 1)   /* 下降沿（按下） */
#define AXP2101_IRQ_KEY_UP        (1u << 0)   /* 上升沿（松开） */

/* 充电阶段（0x01[2:0]） */
typedef enum {
    AXP2101_CHG_TRICKLE      = 0,   /* 涓流（电池 < 2V 左右） */
    AXP2101_CHG_PRECHARGE    = 1,   /* 预充 */
    AXP2101_CHG_CC           = 2,   /* 恒流 */
    AXP2101_CHG_CV           = 3,   /* 恒压 */
    AXP2101_CHG_DONE         = 4,   /* 充电完成 */
    AXP2101_CHG_NOT_CHARGING = 5,   /* 未充电（≠ 放电，放电看电流方向） */
} axp2101_charge_stage_t;

/* 电流方向（0x01[6:5]）：3 = 保留 */
typedef enum {
    AXP2101_CURRENT_STANDBY     = 0,
    AXP2101_CURRENT_CHARGING    = 1,
    AXP2101_CURRENT_DISCHARGING = 2,
} axp2101_current_dir_t;

/**
 * @brief 恒压充电目标电压档位（0x64[2:0]，编码值直接就是寄存器值）
 *
 * ⚠ 本板是 4.2V 满电的单节锂电：**只能**用 4.0 / 4.1 / 4.2V 三档。
 *   4.35V(100b) / 4.4V(101b) 是高压电芯专用，000b（手册标 5.0V）更是错误档，
 *   写错没有 BMS 兜底会直接过充。
 * ⚠ 每次 PMU 上电（POR/System Reset）都会回 EFUSE 默认（本板实测 4.2V），
 *   所以"电池健康保护"之类的设置必须在开机时重新下发。
 */
typedef enum {
    AXP2101_CV_4V0 = 1,   /* 001b = 4.0 V（⚠ 低于电池电压会让 PMU 断开 BATFET，慎用） */
    AXP2101_CV_4V1 = 2,   /* 010b = 4.1 V（约 85~90%） */
    AXP2101_CV_4V2 = 3,   /* 011b = 4.2 V（本板标准满电，出厂默认） */
} axp2101_cv_t;

/** 一次状态读取的结果（电压类字段单位 mV） */
typedef struct {
    uint8_t  raw_status1;               /* 0x00 原始值，便于日志/诊断 */
    uint8_t  raw_status2;               /* 0x01 原始值 */
    bool     vbus_present;              /* 0x00[5] VBUS 良好（插了适配器） */
    bool     battery_present;           /* 0x00[3] 电池在位 */
    bool     batfet_closed;             /* 0x00[4]==0 表示 BATFET 导通（电池供电） */
    bool     gauge_enabled;             /* 0x18[3] 电量计使能（只读确认，默认 1） */
    uint16_t vbat_mv;                   /* 0x34/0x35，1 mV/LSB */
    uint16_t vsys_mv;                   /* 0x3A/0x3B，1 mV/LSB（诊断；0=读失败） */
    uint8_t  soc_percent;               /* 0xA4；0xFF = 电量计不可用/读数越界 */
    axp2101_current_dir_t  current_dir; /* 0x01[6:5] */
    axp2101_charge_stage_t charge_stage;/* 0x01[2:0] */
    bool     charging;                  /* 方向为充电或阶段在预充/CC/CV/DONE */
} axp2101_status_t;

/** 开机/关机原因（只读，供开机流程区分"按键开机 / 插 USB 开机"） */
typedef struct {
    uint8_t pwron_status;    /* 0x20：bit0 PWRON、bit2 VBUS、bit4 电池插入… */
    uint8_t pwr_off_status;  /* 0x21：bit1 软件关机、bit0 PWRON 长按、bit3 VSYS 欠压… */
} axp2101_boot_info_t;

/** 电源键事件（来源：0x49 状态位，写 1 清除） */
typedef enum {
    AXP2101_KEY_SHORT = 0,   /* 短按（< IRQLEVEL） */
    AXP2101_KEY_LONG,        /* 长按（≥ IRQLEVEL） */
    AXP2101_KEY_DOWN,        /* 按下（下降沿） */
    AXP2101_KEY_UP,          /* 松开（上升沿） */
} axp2101_key_event_t;

/* ------------------------------- 初始化 -------------------------------- */

/**
 * @brief 初始化 AXP2101（复用触摸驱动建立的 I²C0 总线）
 *
 * 流程：建设备句柄 → probe → 读状态/开机原因 → 清中断状态 →
 *       （可选）下发充电参数 → （可选）配置 GPIO49 电源键中断 + 事件任务。
 *
 * ⚠ 不写任何电源轨寄存器，不修改 BATFET/PWROFF 使能等有"变砖"风险的配置。
 * ⚠ 每次开机（POR）AXP2101 寄存器都会回默认值，所以必须调用本函数；
 *   失败（芯片不在/总线异常）只返回错误，不 abort——电池信息不可用时
 *   设备仍要能放歌。
 *
 * @param bus touch_get_i2c_bus() 返回的 I²C0 总线句柄；NULL 返回 ESP_ERR_INVALID_ARG
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND / I²C 错误
 */
esp_err_t axp2101_init(i2c_master_bus_handle_t bus);

/** @brief 驱动是否已就绪（init 成功且芯片应答过） */
bool axp2101_is_ready(void);

/* ------------------------------- 状态读取 ------------------------------ */

/** @brief 读电池电压（先读 0x34 锁存、再读 0x35，1 mV/LSB） */
esp_err_t axp2101_read_vbat_mv(uint16_t *mv);

/** @brief 读系统电压 VSYS（0x3A/0x3B，1 mV/LSB） */
esp_err_t axp2101_read_vsys_mv(uint16_t *mv);

/** @brief 读 VBUS 电压（0x38/0x39，1 mV/LSB） */
esp_err_t axp2101_read_vbus_mv(uint16_t *mv);

/**
 * @brief 读电池温度 TS 引脚电压（0x36/0x37，**0.5 mV/LSB**，换算成 mV 返回）
 *
 * ⚠ 本板 TS 脚接的是**固定电阻**（原理图确认，没有 NTC），读到的电压恒定，
 *   换算出的"温度"没有物理意义——界面不显示电池温度，本接口只用于诊断/
 *   诊断。若以后换用带 NTC 的电池，可直接接上 axp2101_ts_mv_to_celsius()。
 */
esp_err_t axp2101_read_ts_mv(uint16_t *mv);

/** @brief 读 E-Gauge 电量百分比（0xA4，0~100；越界视为不可用返回 ESP_ERR_INVALID_RESPONSE） */
esp_err_t axp2101_read_soc(uint8_t *percent);

/** @brief 读充放电状态 + 电压 + 电量（一次完整快照） */
esp_err_t axp2101_read_status(axp2101_status_t *st);

/** @brief 读开机来源 / 上次关机原因 */
esp_err_t axp2101_read_boot_info(axp2101_boot_info_t *info);

/** @brief 读单个寄存器（诊断/自检用；不提供通用写，避免误碰电源轨） */
esp_err_t axp2101_read_reg(uint8_t reg, uint8_t *value);

/* --------------------------- 中断 / 电源键 ----------------------------- */

/**
 * @brief 读 0x48~0x4A 三组中断状态并写 1 清除
 *
 * @param status3 输出 3 字节（0x48 / 0x49 / 0x4A），可为 NULL
 * @param had_event 是否有任意状态位被置位，可为 NULL
 */
esp_err_t axp2101_read_irq_status(uint8_t status3[3], bool *had_event);

/** @brief 清除全部中断状态位（初始化时释放可能被拉低的 IRQ 线） */
esp_err_t axp2101_clear_irq_status(void);

/**
 * @brief 电源键当前是否被按住（由 0x49 的按下/抬起边沿事件维护）
 *
 * 供按键服务把 PMU 电源键当成"第五个 GPIO 键"参与双键同时按下判定。
 * ⚠ 只在 CONFIG_PMU_KEY_IRQ 开启时有效；未开启恒返回 false。
 * ⚠ 状态更新在 pmu_irq 任务里（最多滞后约 5~10ms），人手按键时长足够。
 */
bool axp2101_is_power_key_pressed(void);

/* --------------------------- 配置（可选下发） --------------------------- */

/**
 * @brief 配置长按阈值（0x27，读-改-写保留只读位）
 *
 * @param onlevel  关机态长按开机阈值：0=128ms 1=512ms 2=1s 3=2s
 * @param offlevel 开机态长按硬关机阈值：0=4s 1=6s 2=8s 3=10s
 * @param irqlevel 短按/长按 IRQ 分界：0=1s 1=1.5s 2=2s 3=2.5s
 */
esp_err_t axp2101_set_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel);

/**
 * @brief 下发充电参数（0x62 ICC / 0x63 ITERM / 0x64 CV）
 *
 * ⚠ 只改充电相关寄存器，不动 DCDC/LDO 电源轨。默认不调用（Kconfig
 *   CONFIG_PMU_CHARGE_CONFIG 打开时由 init 按 600 mAh 电池整定值下发）。
 *
 * @param icc_ma     恒流充电电流（mA，按 datasheet 编码表就近取档）
 * @param iterm_code 截止电流档位（0x63[3:0]；600mAh 建议 0x01=25mA / 0x02=50mA）
 * @param cv_42v     true = 恒压 4.2V（0x64=011b，600mAh 电芯的满电电压）
 */
esp_err_t axp2101_configure_charge(uint16_t icc_ma, uint8_t iterm_code, bool cv_42v);

/**
 * @brief 设置恒压充电目标电压（0x64[2:0]，读-改-写）
 *
 * ⚠ **"电池健康保护"不要用本函数降到 4.0V**：电池电压高于 CV 时 PMU 会断开
 *   BATFET（充电完成保护），此时拔掉充电器 VSYS 无来源 → DCDC 欠压硬断电
 *   （真机已复现）。电池健康保护走 `axp2101_set_charge_enable(false)` 软件停充，
 *   CV 保持 4.2V。本函数保留给"把满电目标整体改低"（如换 4.1V 电芯）的场景。
 *
 * ⚠ 只接受 4.0 / 4.1 / 4.2V 三档；其它值（4.35/4.4/5.0V）直接返回
 *   ESP_ERR_INVALID_ARG，防止把 4.2V 电芯写坏。
 *
 * @param cv 目标档位（axp2101_cv_t）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（芯片未初始化）/ ESP_ERR_INVALID_ARG
 */
esp_err_t axp2101_set_charge_voltage(axp2101_cv_t cv);

/**
 * @brief 软件开/关充电（0x18[1]，读-改-写，保留电量计使能与看门狗位）
 *
 * "电池健康保护"用它做**软件限充**：CV 保持 4.2V（不降低充电目标电压），
 * 充到限充上限（BATTERY_CARE_STOP_MV，当前 4.08V）时关闭充电、降到
 * BATTERY_CARE_RESUME_MV（3.98V）再打开。这样 PMU 不会进入
 * "充到 target 以上/完成"的状态，**BATFET 不会断开**——否则电池高于 CV 时
 * 拔掉充电器会因 VSYS 无来源触发 DCDC 欠压保护而直接断电。
 *
 * ⚠ 只关充电，不断系统供电；`0x18` 的 bit3（电量计，默认 1）与 bit0（看门狗，
 *   必须保持 0）都原样保留。
 */
esp_err_t axp2101_set_charge_enable(bool enable);

/** @brief 当前充电使能位（0x18[1]）；读失败按"已使能"返回 true（保守） */
bool axp2101_is_charge_enabled(void);

/**
 * @brief 软件关机（写 0x10[0]=1，PMU 关掉除 RTCLDO 外全部电源）
 *
 * ⚠ 调用前必须停掉播放/扫描并 flush NVS，否则正在写 flash 时断电会写坏数据。
 * ⚠ 本函数**当前没有被任何代码调用**（断电功能需要单独验证），保留给后续
 *   fake_shutdown 迁移/复用。
 */
esp_err_t axp2101_power_off(void);

/* ---------------------------- 纯函数（可自检） -------------------------- */

/** @brief 由充电电流算 0x62[4:0] 编码（N≤8: 25N mA；N>8: 200+100(N-8)） */
uint8_t axp2101_icc_code_from_ma(uint16_t ma);

/** @brief 由 0x62[4:0] 编码反算充电电流（mA） */
uint16_t axp2101_icc_ma_from_code(uint8_t code);

/** @brief 由电池静置电压估算 SOC（0~100，按 600mAh 电芯 OCV 表线性插值） */
uint8_t axp2101_soc_from_mv(uint16_t mv);

/**
 * @brief 把 TS 引脚电压（mV）换算成电池温度（℃，纯函数，可自检）
 *
 * 依据 AXP2101 datasheet §6.7.4.4 的 Table 6-3：10kΩ NTC（25℃）+ 50µA
 * 恒流源时，TS 电压与温度的对应表（-20℃/3.150V … 25℃/0.500V … 60℃/0.152V）。
 * 本函数按该表**线性插值**换算。
 *
 * ⚠ 本板当前 TS 脚是**固定电阻（无 NTC）**，温度恒定无意义，界面**不显示**
 *   电池温度；本函数保留给以后换带 NTC 的电池/自检换算使用。
 * ⚠ 电压超出表格范围（>3150mV 或 <152mV）时返回 false，调用方应显示"未知"
 *   而不是瞎猜的温度。
 *
 * @param ts_mv  TS 引脚电压（mV）
 * @param out_c  输出温度（℃）；仅返回 true 时有效
 * @return true 换算成功；false 超出可换算范围
 */
bool axp2101_ts_mv_to_celsius(uint16_t ts_mv, int16_t *out_c);

/** @brief 解析 0x00/0x01 两个状态字节（纯函数，不访问总线） */
void axp2101_parse_status(uint8_t status1, uint8_t status2, axp2101_status_t *st);

/** @brief 编码 0x27 三个长按阈值字段（纯函数） */
uint8_t axp2101_encode_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel);

/** @brief 解码 0x27 三个字段（纯函数） */
void axp2101_decode_key_levels(uint8_t reg, uint8_t *onlevel, uint8_t *offlevel,
                               uint8_t *irqlevel);

/** @brief 充电阶段/电流方向的可读名字（日志用） */
const char *axp2101_charge_stage_name(axp2101_charge_stage_t stage);
const char *axp2101_current_dir_name(axp2101_current_dir_t dir);

#ifdef __cplusplus
}
#endif
