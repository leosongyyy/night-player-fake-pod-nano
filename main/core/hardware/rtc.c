#include "core/hardware/rtc.h"

#include <string.h>

#include "core/hardware/touch.h"   /* touch_get_i2c_bus()：I²C0 总线由触摸驱动创建 */
#include "esp_log.h"

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_ready = false;

/* ------------------------------------------------------------------ */
/* 底层寄存器访问                                                      */
/* ------------------------------------------------------------------ */

esp_err_t rtc_write_reg(uint8_t reg, uint8_t val)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 指针 + 数据一次事务（datasheet Figure 22） */
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), RTC_I2C_TIMEOUT_MS);
}

esp_err_t rtc_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 写指针 + repeated START + 读 1 字节（datasheet Figure 23） */
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, RTC_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_dev == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 新驱动一次事务完成"写指针 + repeated START + 连续读"，芯片地址自增 */
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, RTC_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    if (s_dev == NULL || data == NULL || len == 0 || len > 7) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 指针 + N 字节一次事务（写时间必须一次写完 7 字节，见 datasheet §7.4） */
    uint8_t buf[8];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, RTC_I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* BCD 与字段校验                                                      */
/* ------------------------------------------------------------------ */

uint8_t rtc_bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0Fu) * 10u + (bcd & 0x0Fu));
}

uint8_t rtc_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10u) << 4) | (bin % 10u));
}

/* BCD 位型 + 数值上限检查：两个半字节都必须 ≤9，且换算后的值 ≤ max */
static bool rtc_bcd_ok(uint8_t v, uint8_t max)
{
    if ((v & 0x0Fu) > 9u || ((v >> 4) & 0x0Fu) > 9u) {
        return false;
    }
    return rtc_bcd_to_bin(v) <= max;
}

uint8_t rtc_weekday_from_date(uint16_t year, uint8_t month, uint8_t day)
{
    /* Sakamoto 公式：0 = 周日 … 6 = 周六，与芯片 Weekdays 寄存器一致。
       ⚠ 本设备年份固定 2000~2099，因此"能被 4 整除即闰年"成立
       （2100 不在范围内，不用考虑百年不闰的例外）。 */
    static const uint8_t t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 1 || month > 12) {
        return 0;
    }
    uint16_t y = year;
    if (month < 3) {
        y -= 1;
    }
    uint32_t v = (uint32_t)y + y / 4u - y / 100u + y / 400u + t[month - 1] + day;
    return (uint8_t)(v % 7u);
}

bool rtc_time_is_sane(const rtc_time_t *t)
{
    if (t == NULL) {
        return false;
    }
    if (t->year < 2000 || t->year > 2099) {
        return false;
    }
    if (t->month < 1 || t->month > 12) {
        return false;
    }
    if (t->hour > 23 || t->minute > 59 || t->second > 59) {
        return false;
    }
    if (t->weekday > 6) {
        return false;
    }

    static const uint8_t days_in_month[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    uint8_t dim = days_in_month[t->month - 1];
    if (t->month == 2 && (t->year % 4u) == 0) {
        dim = 29;   /* 芯片的闰年规则：年数值能被 4 整除（含 00）即闰年 */
    }
    return t->day >= 1 && t->day <= dim;
}

esp_err_t rtc_decode_time(const uint8_t raw[7], rtc_time_t *out)
{
    if (raw == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 各字段的位布局见 reference_docs/rtc_pcf85063a.md §3.6：
       Seconds 04h：bit7=OS，bit6-4 十位、bit3-0 个位；
       Minutes 05h / Hours 06h（24h 制 bit5-4 十位）/ Days 07h（bit5-4 十位）/
       Weekdays 08h（3bit 二进制）/ Months 09h（bit4 十位）/ Years 0Ah。 */
    const uint8_t sec_raw = (uint8_t)(raw[0] & 0x7Fu);
    const uint8_t min_raw = (uint8_t)(raw[1] & 0x7Fu);
    const uint8_t hour_raw = (uint8_t)(raw[2] & 0x3Fu);
    const uint8_t day_raw = (uint8_t)(raw[3] & 0x3Fu);
    const uint8_t wd_raw = (uint8_t)(raw[4] & 0x07u);
    const uint8_t mon_raw = (uint8_t)(raw[5] & 0x1Fu);
    const uint8_t year_raw = raw[6];

    if (!rtc_bcd_ok(sec_raw, 59) || !rtc_bcd_ok(min_raw, 59)
        || !rtc_bcd_ok(hour_raw, 23) || !rtc_bcd_ok(day_raw, 31)
        || !rtc_bcd_ok(mon_raw, 12) || !rtc_bcd_ok(year_raw, 99)
        || wd_raw > 6) {
        return ESP_ERR_INVALID_RESPONSE;   /* BCD 位型/范围非法，数据不可信 */
    }

    out->year    = (uint16_t)(2000u + rtc_bcd_to_bin(year_raw));
    out->month   = rtc_bcd_to_bin(mon_raw);
    out->day     = rtc_bcd_to_bin(day_raw);
    out->hour    = rtc_bcd_to_bin(hour_raw);
    out->minute  = rtc_bcd_to_bin(min_raw);
    out->second  = rtc_bcd_to_bin(sec_raw);
    out->weekday = wd_raw;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 首次校时：用"项目诞生日"兜底                                          */
/* ------------------------------------------------------------------ */

/* 项目诞生日 = git 仓库**首个提交**的时间（06d8b19，2026-08-30 13:45:41 +0800）。
   用途仅限"RTC 从未校时（OS=1）或明显是出厂默认"时的首次初始化：
   比 2000-01-01 或停走的时间有意义，且不会像 __DATE__/__TIME__ 那样随
   每次编译变化。后续应由用户手动校时（暂无校时 UI）覆盖。
   ⚠ 这是**项目**的时间基准，不是固件编译时间。 */
#define RTC_PROJECT_EPOCH_YEAR    2026
#define RTC_PROJECT_EPOCH_MONTH   8
#define RTC_PROJECT_EPOCH_DAY     30
#define RTC_PROJECT_EPOCH_HOUR    13
#define RTC_PROJECT_EPOCH_MINUTE  45
#define RTC_PROJECT_EPOCH_SECOND  41

static bool rtc_project_epoch_time(rtc_time_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->year    = RTC_PROJECT_EPOCH_YEAR;
    out->month   = RTC_PROJECT_EPOCH_MONTH;
    out->day     = RTC_PROJECT_EPOCH_DAY;
    out->hour    = RTC_PROJECT_EPOCH_HOUR;
    out->minute  = RTC_PROJECT_EPOCH_MINUTE;
    out->second  = RTC_PROJECT_EPOCH_SECOND;
    out->weekday = rtc_weekday_from_date(out->year, out->month, out->day);
    return rtc_time_is_sane(out);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

bool rtc_is_ready(void)
{
    return s_ready;
}

esp_err_t rtc_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ⚠ 复用触摸驱动创建的 I²C0 总线，绝不能再 i2c_new_master_bus(I2C_NUM_0)：
       同一端口重复初始化会失败；deinit 重建还会把触摸一起搞死。 */
    i2c_master_bus_handle_t bus = touch_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready (touch_init must run first)");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = 400 * 1000,   /* 必须 > 0，IDF 6.1 硬校验 */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", RTC_I2C_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(bus, RTC_I2C_ADDR, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A not responding at 0x%02x (%s) — RTC unavailable",
                 RTC_I2C_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* 关闭 CLKOUT：复位默认 COF=000 → 悬空的 CLKOUT 脚一直输出 32.768 kHz
       推挽方波，既费电又有 EMI（参考文档 §7.4）。同时把 AF/TF 写 0（清标志）、
       AIE/MI/HMI 保持 0（本板 INT 未接，不需要中断）。 */
    err = rtc_write_reg(RTC_REG_CONTROL_2, RTC_C2_COF_MASK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable CLKOUT failed: %s", esp_err_to_name(err));
    }

    /* 检查走时状态：STOP=1 会让"时间读写都正常、就是不走"（参考文档 §9.6）；
       12_24=1 会把 Hours 字节解释成 12 小时制（bit5 变 AM/PM），本驱动固定
       24h 解析，发现异常只告警、不擅自改写——改写会让小时含义瞬间变化。 */
    uint8_t c1 = 0;
    if (rtc_read_reg(RTC_REG_CONTROL_1, &c1) == ESP_OK) {
        if ((c1 & RTC_C1_12_24) != 0) {
            ESP_LOGW(TAG, "RTC is in 12-hour mode (Control_1.12_24=1); "
                          "driver assumes 24-hour mode, time may be wrong");
        }
        if ((c1 & RTC_C1_STOP) != 0) {
            ESP_LOGW(TAG, "RTC is stopped (STOP=1), clearing STOP to resume ticking");
            rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 & ~RTC_C1_STOP));
        }
    }

    s_ready = true;

    /* 未初始化（振荡停止 OS=1 / 明显是出厂默认 / 字段非法）时做**首次校时**：
       写入"项目诞生日"（仓库首个提交的时间），让时间从 2000-01-01 或停走
       状态恢复成合理值。
       ⚠ 只在无效时写，绝不覆盖备用电源保住的准确走时；也绝不发软件复位。 */
    bool wrote = false;
    esp_err_t init_time_err = rtc_init_time_if_invalid(&wrote);
    if (init_time_err != ESP_OK && init_time_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "RTC first-time init failed: %s", esp_err_to_name(init_time_err));
    }

    /* 读一次时间 + 有效性，便于开机日志确认"电池保住的走时还在不在" */
    rtc_time_t t;
    if (rtc_get_time(&t) == ESP_OK) {
        bool valid = false;
        rtc_is_valid(&valid);
        ESP_LOGI(TAG, "PCF85063A ready: %04u-%02u-%02u %02u:%02u:%02u wd=%u (%s%s)",
                 t.year, t.month, t.day, t.hour, t.minute, t.second, t.weekday,
                 valid ? "valid" : "INVALID/unset",
                 wrote ? ", auto-initialized" : "");
    } else {
        ESP_LOGW(TAG, "PCF85063A ready but time read failed");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 读写时间                                                            */
/* ------------------------------------------------------------------ */

esp_err_t rtc_get_time(rtc_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ⚠ 必须一次读完 7 字节：读写时间寄存器期间芯片冻结计数，积压的自增
       请求最多只保留 1 个，整个访问要在 1 秒内完成（datasheet §7.4）。 */
    uint8_t buf[7] = { 0 };
    esp_err_t err = rtc_read_regs(RTC_REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    return rtc_decode_time(buf, out);
}

esp_err_t rtc_set_time(const rtc_time_t *t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc_time_is_sane(t)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) 置 STOP=1：让分频链停住，写时间时不会被进位打断。
          读-改-写是为了保留 CAP_SEL / 12_24 / CIE 等既有配置。 */
    uint8_t c1 = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_CONTROL_1, &c1);
    if (err != ESP_OK) {
        return err;
    }
    err = rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 | RTC_C1_STOP));
    if (err != ESP_OK) {
        return err;
    }

    /* 2) 一次事务写 7 字节：Seconds..Years。
          Seconds 的 bit7（OS）写 0 —— 这是清除振荡停止标志的唯一手段。
          星期若调用方没填（>6），按日期算一个，避免芯片里存着错误星期。 */
    uint8_t wd = t->weekday;
    if (wd > 6) {
        wd = rtc_weekday_from_date(t->year, t->month, t->day);
    }

    uint8_t buf[7];
    buf[0] = (uint8_t)(rtc_bin_to_bcd(t->second) & 0x7F);   /* OS = 0 */
    buf[1] = (uint8_t)(rtc_bin_to_bcd(t->minute) & 0x7F);
    buf[2] = (uint8_t)(rtc_bin_to_bcd(t->hour) & 0x3F);     /* 24h */
    buf[3] = (uint8_t)(rtc_bin_to_bcd(t->day) & 0x3F);
    buf[4] = (uint8_t)(wd & 0x07);                          /* 3bit 二进制 */
    buf[5] = (uint8_t)(rtc_bin_to_bcd(t->month) & 0x1F);
    buf[6] = rtc_bin_to_bcd((uint8_t)(t->year % 100u));

    err = rtc_write_regs(RTC_REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) {
        /* 失败也要把 STOP 放掉，否则 RTC 永远停走 */
        rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 & ~RTC_C1_STOP));
        return err;
    }

    /* 3) 清 STOP 开始走时。注意：释放后第一次 +1 秒在 0.5078s 之后
          （F0/F1 两级分频器不复位，见 datasheet Table 6），不影响长期走时。 */
    err = rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 & ~RTC_C1_STOP));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "time set: %04u-%02u-%02u %02u:%02u:%02u wd=%u",
                 t->year, t->month, t->day, t->hour, t->minute, t->second, wd);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* 有效性 / OS 标志                                                    */
/* ------------------------------------------------------------------ */

esp_err_t rtc_get_osf(bool *osf)
{
    if (osf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t sec = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_SECONDS, &sec);
    if (err != ESP_OK) {
        return err;
    }
    *osf = (sec & RTC_SEC_OS) != 0;
    return ESP_OK;
}

esp_err_t rtc_clear_osf(void)
{
    /* 写 0 到 Seconds.bit7 即可清除；其余位保持原值，避免把秒改坏 */
    uint8_t sec = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_SECONDS, &sec);
    if (err != ESP_OK) {
        return err;
    }
    return rtc_write_reg(RTC_REG_SECONDS, (uint8_t)(sec & ~RTC_SEC_OS));
}

esp_err_t rtc_is_valid(bool *valid)
{
    if (valid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *valid = false;

    /* 一次 burst 读完 7 字节：既拿 OS 位又拿时间字段，避免两次事务之间
       发生进位（也少一次"冻结计数"的总线访问）。 */
    uint8_t raw[7] = { 0 };
    esp_err_t err = rtc_read_regs(RTC_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    /* 1) OS 位：1 = 振荡器停过/被中断，时间不可信（datasheet §7.3.1.1） */
    if ((raw[0] & RTC_SEC_OS) != 0) {
        return ESP_OK;   /* 保持 false */
    }

    /* 2) BCD 位型 + 字段范围/当月天数（含闰年） */
    rtc_time_t t;
    if (rtc_decode_time(raw, &t) != ESP_OK) {
        return ESP_OK;   /* 数据非法：保持 false，不向上报错 */
    }
    if (!rtc_time_is_sane(&t)) {
        return ESP_OK;
    }

    /* 3) 年份下限：排除 2000-01-01 这类出厂默认（芯片无出厂标记，只能折衷） */
    *valid = (t.year >= 2024);
    return ESP_OK;
}

esp_err_t rtc_init_time_if_invalid(bool *did_write)
{
    if (did_write != NULL) {
        *did_write = false;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    bool valid = false;
    esp_err_t err = rtc_is_valid(&valid);
    if (err != ESP_OK) {
        return err;
    }
    if (valid) {
        return ESP_OK;   /* 时间有效：绝不覆盖备用电源保住的走时 */
    }

#ifdef CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH
    bool osf = false;
    rtc_get_osf(&osf);

    rtc_time_t epoch;
    if (!rtc_project_epoch_time(&epoch)) {
        return ESP_ERR_INVALID_STATE;
    }
    err = rtc_set_time(&epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC auto-init write failed: %s", esp_err_to_name(err));
        return err;
    }
    if (did_write != NULL) {
        *did_write = true;
    }
    ESP_LOGW(TAG, "RTC uninitialized (OS=%d): set to project epoch "
                  "%04u-%02u-%02u %02u:%02u:%02u",
             osf, epoch.year, epoch.month, epoch.day, epoch.hour, epoch.minute,
             epoch.second);
    return ESP_OK;
#else
    bool osf = false;
    rtc_get_osf(&osf);
    ESP_LOGW(TAG, "RTC uninitialized (OS=%d); auto-init disabled "
                  "(CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH=n)", osf);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t rtc_set_offset(int offset_lsb, bool fast_mode)
{
    if (offset_lsb < -64 || offset_lsb > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Offset（02h）：bit7 = MODE（0 = 每 2 小时校正，1 = 每 4 分钟校正）
                      bit6-0 = OFFSET[6:0]，二进制补码 +63 ~ -64
       每个 LSB：MODE=0 时 4.34 ppm，MODE=1 时 4.069 ppm（datasheet Table 11/12）。
       ⚠ 本板 CLKOUT 未接主控，无法测频，校准只能手工/实测闭环。 */
    uint8_t v = (uint8_t)(offset_lsb & 0x7F);
    if (fast_mode) {
        v |= 0x80;
    }
    return rtc_write_reg(RTC_REG_OFFSET, v);
}

/* ------------------------------------------------------------------ */
/* struct tm 互转                                                      */
/* ------------------------------------------------------------------ */

void rtc_time_to_tm(const rtc_time_t *t, struct tm *out)
{
    if (t == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec   = t->second;
    out->tm_min   = t->minute;
    out->tm_hour  = t->hour;
    out->tm_mday  = t->day;
    out->tm_mon   = (int)t->month - 1;    /* 1~12 → 0~11 */
    out->tm_year  = (int)t->year - 1900;  /* 2026 → 126 */
    out->tm_wday  = t->weekday;           /* 0 = 周日，与 RTC 一致 */
    out->tm_isdst = -1;                   /* 无夏令时概念 */
}

void rtc_time_from_tm(const struct tm *in, rtc_time_t *t)
{
    if (in == NULL || t == NULL) {
        return;
    }
    t->second  = (uint8_t)in->tm_sec;
    t->minute  = (uint8_t)in->tm_min;
    t->hour    = (uint8_t)in->tm_hour;
    t->day     = (uint8_t)in->tm_mday;
    t->month   = (uint8_t)(in->tm_mon + 1);       /* 0~11 → 1~12 */
    t->year    = (uint16_t)(in->tm_year + 1900);  /* 126 → 2026 */
    t->weekday = (uint8_t)in->tm_wday;
}
