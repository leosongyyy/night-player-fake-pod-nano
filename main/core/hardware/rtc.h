#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCF85063A 实时时钟（I²C0 共享总线）
 *
 * 硬件连接（FakePodNano Pro V1，见 ideas_and_exps/reference_docs/rtc_pcf85063a.md）：
 *   · I²C0：SDA=GPIO48 / SCL=GPIO47 / 400 kHz，7-bit 地址 **0x51**；
 *     与 CST820(0x15)、AXP2101(0x34)、QMI8658(0x6B) 共用总线。
 *   · 本板 **INT / CLKOUT 未接主控**，只能轮询读时间；驱动不依赖中断，
 *     也不做闹钟/定时器中断接入（寄存器级读写接口保留，供将来飞线使用）。
 *   · 芯片没有世纪位：年份固定按 2000~2099 解释。
 *   · 24 小时制固定（Control_1.12_24 = 0，默认值），星期 0=周日…6=周六，
 *     与 Linux struct tm 的 tm_wday 一致。
 *
 * ⚠ 绝不做无条件软件复位（写 0x00=0x58）：那会把备用电源保住的走时清成
 *   2000-01-01。只有确认"时间无效"时才由上层决定是否校时。
 */

#define RTC_I2C_ADDR            0x51   /* 7-bit 地址 1010 001（datasheet Table 38） */
#define RTC_I2C_TIMEOUT_MS      100

/* 寄存器地址（00h~11h，共 18 个；高位未用，地址自增、11h 后回绕 00h） */
#define RTC_REG_CONTROL_1       0x00
#define RTC_REG_CONTROL_2       0x01
#define RTC_REG_OFFSET          0x02
#define RTC_REG_RAM_BYTE        0x03
#define RTC_REG_SECONDS         0x04
#define RTC_REG_MINUTES         0x05
#define RTC_REG_HOURS           0x06
#define RTC_REG_DAYS            0x07
#define RTC_REG_WEEKDAYS        0x08
#define RTC_REG_MONTHS          0x09
#define RTC_REG_YEARS           0x0A
#define RTC_REG_SECOND_ALARM    0x0B
#define RTC_REG_MINUTE_ALARM    0x0C
#define RTC_REG_HOUR_ALARM      0x0D
#define RTC_REG_DAY_ALARM       0x0E
#define RTC_REG_WEEKDAY_ALARM   0x0F
#define RTC_REG_TIMER_VALUE     0x10
#define RTC_REG_TIMER_MODE      0x11

/* Control_1（00h）位定义 —— datasheet Table 5 */
#define RTC_C1_EXT_TEST         (1u << 7)   /* 外部时钟测试（默认 0） */
#define RTC_C1_STOP             (1u << 5)   /* 1 = 停走时（写时间时用） */
#define RTC_C1_SR               (1u << 4)   /* 软件复位（读恒 0；会清时间，勿用） */
#define RTC_C1_CIE              (1u << 2)   /* 校正中断使能 */
#define RTC_C1_12_24            (1u << 1)   /* 1 = 12 小时制（本项目固定 0 = 24h） */
#define RTC_C1_CAP_SEL          (1u << 0)   /* 0 = 7pF / 1 = 12.5pF（保持默认） */

/* Control_2（01h）位定义 —— datasheet Table 8 */
#define RTC_C2_AIE              (1u << 7)
#define RTC_C2_AF               (1u << 6)   /* 写 0 清除，写 1 不变 */
#define RTC_C2_MI               (1u << 5)
#define RTC_C2_HMI              (1u << 4)
#define RTC_C2_TF               (1u << 3)   /* 写 0 清除 */
#define RTC_C2_COF_MASK         0x07u       /* CLKOUT 频率：[2:0]，111 = 关闭 */

/* Seconds（04h）bit7：振荡停止标志，1 = 时间可能无效 */
#define RTC_SEC_OS              (1u << 7)

/** 时间结构体（语义对齐 Linux struct tm，只保留 RTC 能表达的字段） */
typedef struct {
    uint16_t year;      /* 公元年，如 2026（芯片只有 00~99，固定按 2000+ 解释） */
    uint8_t  month;     /* 1~12 */
    uint8_t  day;       /* 1~31 */
    uint8_t  hour;      /* 0~23（固定 24 小时制） */
    uint8_t  minute;    /* 0~59 */
    uint8_t  second;    /* 0~59 */
    uint8_t  weekday;   /* 0 = 周日 … 6 = 周六（与 tm_wday 一致） */
} rtc_time_t;

/**
 * @brief 初始化 PCF85063A（复用触摸驱动建立的 I²C0 总线）
 *
 * 只做：建设备句柄 → probe → 关闭 CLKOUT（本板悬空，省电/降 EMI）→
 * 检查 STOP/12_24 位 → 读一次时间并判定有效性 →
 * **若时间无效（OS=1 / 出厂默认 / 字段非法）则用"项目诞生日"做首次校时**
 * （仓库首个提交的时间 2026-08-30 13:45:41 +0800，
 * CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH，默认开）。
 *
 * ⚠ 不做软件复位、不覆盖有效时间：备用电源保住的走时不能因为开机而丢掉。
 * ⚠ 必须在 touch_init() 之后调用（总线句柄由触摸驱动创建）。
 *   失败不致命：没有 RTC 也要能开机放歌。
 *
 * @return ESP_OK / ESP_ERR_INVALID_STATE（总线未就绪）/ ESP_ERR_NOT_FOUND（芯片无应答）
 */
esp_err_t rtc_init(void);

/** @brief 驱动是否已就绪（init 成功且芯片应答过） */
bool rtc_is_ready(void);

/**
 * @brief 一次 burst 读 7 字节（04h~0Ah）解析时间
 *
 * ⚠ 必须一次事务读完：读写 04h~0Ah 期间芯片冻结计数，积压的进位请求最多
 *   只服务 1 次，整个访问要在 1 秒内完成；分多次读会出现"分/秒来自不同时刻"。
 */
esp_err_t rtc_get_time(rtc_time_t *out);

/** @brief 写时间：STOP=1 → 一次事务写 7 字节 → STOP=0（清 OS 位） */
esp_err_t rtc_set_time(const rtc_time_t *t);

/**
 * @brief 时间有效性：OS=0 且字段合理且年份 ≥ 2024
 *
 * 年份下限用于排除"2000-01-01 00:00:00"这类出厂默认（芯片没有出厂标记）。
 */
esp_err_t rtc_is_valid(bool *valid);

/** @brief 单独读 / 清 OS（振荡停止）标志 */
esp_err_t rtc_get_osf(bool *osf);
esp_err_t rtc_clear_osf(void);

/**
 * @brief 若时间无效则做首次校时（写入"项目诞生日"）
 *
 * 判据：rtc_is_valid() == false（OS=1 / 出厂默认 / 字段非法）。
 * 有效时**什么都不写**，直接返回 ESP_OK。
 *
 * @param did_write 输出：本次是否真的写入了时间（可为 NULL）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（驱动未就绪）/ I²C 错误 /
 *         ESP_ERR_NOT_SUPPORTED（CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH=n）
 */
esp_err_t rtc_init_time_if_invalid(bool *did_write);

/** @brief 寄存器级读写（alarm / timer / offset / RAM_byte 等） */
esp_err_t rtc_read_reg(uint8_t reg, uint8_t *val);
esp_err_t rtc_write_reg(uint8_t reg, uint8_t val);

/** @brief 写 Offset（频率微调）：offset_lsb 为 -64~+63，fast_mode = MODE 位 */
esp_err_t rtc_set_offset(int offset_lsb, bool fast_mode);

/** @brief 与 Linux struct tm 互转（tm_mon 0 基、tm_year 自 1900、tm_wday 0=周日） */
void rtc_time_to_tm(const rtc_time_t *t, struct tm *out);
void rtc_time_from_tm(const struct tm *in, rtc_time_t *t);

/* ---------------------------- 纯函数（可自检） -------------------------- */

/** @brief BCD ↔ 十进制（秒/分/时/日/月/年与全部 alarm 是 BCD；Timer/星期不是） */
uint8_t rtc_bcd_to_bin(uint8_t bcd);
uint8_t rtc_bin_to_bcd(uint8_t bin);

/** @brief 由公历日期算星期（Sakamoto 公式；0=周日，与芯片寄存器一致） */
uint8_t rtc_weekday_from_date(uint16_t year, uint8_t month, uint8_t day);

/** @brief 字段范围 + 当月天数（含闰年）校验；月份 1~12、年份 2000~2099 */
bool rtc_time_is_sane(const rtc_time_t *t);

/** @brief 从 7 字节原始寄存器值（04h~0Ah）解析时间；BCD/范围非法返回错误 */
esp_err_t rtc_decode_time(const uint8_t raw[7], rtc_time_t *out);

#ifdef __cplusplus
}
#endif
