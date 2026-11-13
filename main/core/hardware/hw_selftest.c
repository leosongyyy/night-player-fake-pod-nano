#include "core/hardware/hw_selftest.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/axp2101.h"
#include "core/hardware/battery.h"
#include "core/hardware/rtc.h"
#include "core/utils/task_psram.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hwst";

/* ------------------------------------------------------------------ */
/* 说明                                                                */
/* ------------------------------------------------------------------ */
/* AXP2101 / PCF85063A 驱动的代码级自检。测试分两层：
 *   · 纯逻辑测试：不碰硬件，验证编码/解析/插值等纯函数（可在任何板子上跑）；
 *   · 真机测试：只做**只读或可完全恢复**的操作。
 * ⚠ 刻意不测试断电：不调用 axp2101_power_off()、不写 0x10 的软关机位，
 *   不写任何 DCDC/LDO 电源轨寄存器。RTC 只写 RAM_byte（写完恢复原值），
 *   不调用 rtc_set_time()（避免把备用电源保住的走时冲掉）。
 */

static int s_pass = 0;
static int s_fail = 0;
static int s_skip = 0;
static bool s_started = false;

static void test_pass(const char *name)
{
    s_pass++;
    ESP_LOGI(TAG, "[PASS] %s", name);
}

static void test_fail(const char *name, const char *detail)
{
    s_fail++;
    if (detail != NULL && detail[0] != '\0') {
        ESP_LOGE(TAG, "[FAIL] %s (%s)", name, detail);
    } else {
        ESP_LOGE(TAG, "[FAIL] %s", name);
    }
}

static void test_check(const char *name, bool ok)
{
    if (ok) {
        test_pass(name);
    } else {
        test_fail(name, NULL);
    }
}

static void test_skip(const char *name, const char *reason)
{
    s_skip++;
    ESP_LOGW(TAG, "[SKIP] %s (%s)", name, reason);
}

/* ------------------------------------------------------------------ */
/* 一、纯逻辑测试（不访问硬件）                                         */
/* ------------------------------------------------------------------ */

static void test_pmu_icc_encoding(void)
{
    /* 充电电流编码：N≤8 → 25N；N>8 → 200+100(N-8)，注意 0x08→200 与 0x09→300
       之间有 100mA 跳变，不是 +25mA 递推。 */
    bool ok = true;
    ok &= (axp2101_icc_code_from_ma(25) == 1);
    ok &= (axp2101_icc_code_from_ma(50) == 2);
    ok &= (axp2101_icc_code_from_ma(100) == 4);
    ok &= (axp2101_icc_code_from_ma(200) == 8);
    ok &= (axp2101_icc_code_from_ma(300) == 9);
    ok &= (axp2101_icc_code_from_ma(500) == 11);
    ok &= (axp2101_icc_code_from_ma(1000) == 16);
    ok &= (axp2101_icc_ma_from_code(0) == 0);
    ok &= (axp2101_icc_ma_from_code(8) == 200);
    ok &= (axp2101_icc_ma_from_code(9) == 300);
    ok &= (axp2101_icc_ma_from_code(16) == 1000);
    /* 每个合法档位做一次往返（只对"表内精确值"成立） */
    for (uint8_t code = 0; code <= 16; code++) {
        uint16_t ma = axp2101_icc_ma_from_code(code);
        ok &= (axp2101_icc_code_from_ma(ma) == code);
    }
    test_check("pmu_icc_encoding", ok);
}

static void test_pmu_soc_interp(void)
{
    bool ok = true;
    ok &= (axp2101_soc_from_mv(4300) == 100);
    ok &= (axp2101_soc_from_mv(4200) == 100);
    ok &= (axp2101_soc_from_mv(3200) == 0);
    ok &= (axp2101_soc_from_mv(3860) == 50);

    /* 3900mV 落在 3860(50%) 与 3920(60%) 之间 → 应为 56% 左右 */
    uint8_t soc3900 = axp2101_soc_from_mv(3900);
    ok &= (soc3900 >= 55 && soc3900 <= 60);

    /* 电压越低 SOC 不得越高（单调性检查） */
    uint8_t prev = 101;
    for (uint16_t mv = 4200; mv >= 3300; mv -= 50) {
        uint8_t soc = axp2101_soc_from_mv(mv);
        if (soc > prev) {
            ok = false;
        }
        prev = soc;
    }
    test_check("pmu_soc_interp", ok);
}

static void test_pmu_status_decode(void)
{
    axp2101_status_t st;
    bool ok = true;

    /* 插着 USB、电池在位、BATFET 导通、正在 CC 充电 */
    memset(&st, 0, sizeof(st));
    axp2101_parse_status(0x28, (uint8_t)((1u << 5) | AXP2101_CHG_CC), &st);
    ok &= st.vbus_present;
    ok &= st.battery_present;
    ok &= st.batfet_closed;
    ok &= st.charging;
    ok &= (st.current_dir == AXP2101_CURRENT_CHARGING);
    ok &= (st.charge_stage == AXP2101_CHG_CC);

    /* 纯电池放电、未充电 */
    memset(&st, 0, sizeof(st));
    axp2101_parse_status(0x10, (uint8_t)((2u << 5) | AXP2101_CHG_NOT_CHARGING), &st);
    ok &= !st.vbus_present;
    ok &= !st.charging;
    ok &= (st.current_dir == AXP2101_CURRENT_DISCHARGING);
    ok &= (st.charge_stage == AXP2101_CHG_NOT_CHARGING);

    /* 充电完成：方向可能已回待机，但阶段 DONE 必须仍算"充电中"（UI 显示已充满） */
    memset(&st, 0, sizeof(st));
    axp2101_parse_status(0x28, AXP2101_CHG_DONE, &st);
    ok &= st.charging;
    ok &= (st.charge_stage == AXP2101_CHG_DONE);

    /* 保留编码 110b / 111b 按"未充电"处理，不能越界成非法枚举 */
    memset(&st, 0, sizeof(st));
    axp2101_parse_status(0x00, 0x06, &st);
    ok &= (st.charge_stage == AXP2101_CHG_NOT_CHARGING);

    test_check("pmu_status_decode", ok);
}

static void test_pmu_key_levels_encode(void)
{
    /* 0x27：[5:4] IRQLEVEL [3:2] OFFLEVEL [1:0] ONLEVEL，只生成低 6 位 */
    uint8_t reg = axp2101_encode_key_levels(1, 2, 3);
    uint8_t on = 0, off = 0, irq = 0;
    axp2101_decode_key_levels(reg, &on, &off, &irq);
    test_check("pmu_key_levels_encode",
               reg == 0x39 && on == 1 && off == 2 && irq == 3);
}

static void test_rtc_bcd(void)
{
    bool ok = true;
    for (uint8_t v = 0; v <= 59; v++) {
        ok &= (rtc_bcd_to_bin(rtc_bin_to_bcd(v)) == v);
    }
    ok &= (rtc_bin_to_bcd(59) == 0x59);
    ok &= (rtc_bcd_to_bin(0x23) == 23);
    ok &= (rtc_bcd_to_bin(0x00) == 0);
    test_check("rtc_bcd", ok);
}

static void test_rtc_weekday(void)
{
    /* 已核对过的公历事实：2000-01-01 周六、2024-01-01 周一、
       2025-01-01 周三、2026-08-30 周日（项目诞生日 / RTC 首次校时基准）、
       2026-11-14 周六、2026-11-15 周日 */
    bool ok = true;
    ok &= (rtc_weekday_from_date(2000, 1, 1) == 6);
    ok &= (rtc_weekday_from_date(2024, 1, 1) == 1);
    ok &= (rtc_weekday_from_date(2025, 1, 1) == 3);
    ok &= (rtc_weekday_from_date(2026, 8, 30) == 0);
    ok &= (rtc_weekday_from_date(2026, 11, 14) == 6);
    ok &= (rtc_weekday_from_date(2026, 11, 15) == 0);
    test_check("rtc_weekday", ok);
}

static void test_rtc_decode(void)
{
    bool ok = true;

    /* 向量 1：芯片复位默认值 → 00:00:00，2000-01-01，周六（OS=1） */
    const uint8_t reset_raw[7] = { 0x80, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00 };
    rtc_time_t t;
    memset(&t, 0, sizeof(t));
    ok &= (rtc_decode_time(reset_raw, &t) == ESP_OK);
    ok &= (t.year == 2000 && t.month == 1 && t.day == 1);
    ok &= (t.hour == 0 && t.minute == 0 && t.second == 0);
    ok &= (t.weekday == 6);

    /* 向量 2：2026-11-14 01:48:57 周六 */
    const uint8_t now_raw[7] = { 0x57, 0x48, 0x01, 0x14, 0x06, 0x11, 0x26 };
    memset(&t, 0, sizeof(t));
    ok &= (rtc_decode_time(now_raw, &t) == ESP_OK);
    ok &= (t.year == 2026 && t.month == 11 && t.day == 14);
    ok &= (t.hour == 1 && t.minute == 48 && t.second == 57);
    ok &= (t.weekday == 6);

    /* 非法 BCD（秒个位 = 0xA）/ 月份 13 → 必须报错，不能解析出垃圾时间 */
    const uint8_t bad_sec[7] = { 0x5A, 0x00, 0x00, 0x01, 0x06, 0x01, 0x26 };
    const uint8_t bad_mon[7] = { 0x00, 0x00, 0x00, 0x01, 0x06, 0x13, 0x26 };
    ok &= (rtc_decode_time(bad_sec, &t) == ESP_ERR_INVALID_RESPONSE);
    ok &= (rtc_decode_time(bad_mon, &t) == ESP_ERR_INVALID_RESPONSE);

    test_check("rtc_decode", ok);
}

static void test_rtc_sanity(void)
{
    rtc_time_t t = { .year = 2024, .month = 2, .day = 29,
                     .hour = 23, .minute = 59, .second = 59, .weekday = 4 };
    bool ok = rtc_time_is_sane(&t);          /* 2024 闰年 2/29 合法 */

    t.year = 2026;
    ok &= !rtc_time_is_sane(&t);             /* 2026 非闰年 2/29 非法 */

    t.year = 2026; t.month = 13; t.day = 1;
    ok &= !rtc_time_is_sane(&t);             /* 月份越界 */

    t.year = 2026; t.month = 4; t.day = 31;
    ok &= !rtc_time_is_sane(&t);             /* 4 月没有 31 日 */

    t.year = 2026; t.month = 12; t.day = 31; t.hour = 24;
    ok &= !rtc_time_is_sane(&t);             /* 小时越界 */

    test_check("rtc_sanity", ok);
}

static void test_battery_fuse(void)
{
    bool ok = true;
    bool used = false;

    /* 放电、gauge 与电压一致（3860mV≈50%）→ 加权后仍接近 50 */
    uint8_t p = battery_fuse_percent(50, 3860, false, AXP2101_CHG_NOT_CHARGING, &used);
    ok &= (p >= 48 && p <= 52) && !used;

    /* 放电、gauge 90% 但电压只有 3700mV（≈26%）→ 偏差过大，必须走电压兜底 */
    p = battery_fuse_percent(90, 3700, false, AXP2101_CHG_NOT_CHARGING, &used);
    ok &= (p <= 30) && used;

    /* gauge 报 0 但电压 3900mV（≈56%）→ 端点可疑，走电压 */
    p = battery_fuse_percent(0, 3900, false, AXP2101_CHG_NOT_CHARGING, &used);
    ok &= (p >= 50) && used;

    /* 充电中阶段 DONE → 100%，不受 gauge 影响 */
    p = battery_fuse_percent(80, 4100, true, AXP2101_CHG_DONE, &used);
    ok &= (p == 100) && !used;

    /* 充电中 CC、gauge 100 → 锁 99%（没到 DONE 不能显示满电） */
    p = battery_fuse_percent(100, 4100, true, AXP2101_CHG_CC, &used);
    ok &= (p == 99);

    /* gauge 不可用（0xFF）→ 电压兜底 */
    p = battery_fuse_percent(0xFF, 3860, false, AXP2101_CHG_NOT_CHARGING, &used);
    ok &= (p >= 48 && p <= 52) && used;

    test_check("battery_fuse", ok);
}

/* ------------------------------------------------------------------ */
/* 二、真机测试：AXP2101（只读 / 读+清中断状态）                        */
/* ------------------------------------------------------------------ */

static void pmu_dump_regs(void)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x10, 0x18, 0x1A, 0x20, 0x21, 0x22, 0x27, 0x30,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
        0x40, 0x41, 0x42, 0x48, 0x49, 0x4A, 0x50, 0x61, 0x62, 0x63,
        0x64, 0x67, 0xA4,
    };
    char buf[320];
    int pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]) && pos < (int)sizeof(buf) - 8; i++) {
        uint8_t v = 0;
        if (axp2101_read_reg(regs[i], &v) == ESP_OK) {
            pos += snprintf(&buf[pos], sizeof(buf) - (size_t)pos, "%02X=%02X ", regs[i], v);
        }
    }
    ESP_LOGI(TAG, "PMU regs: %s", buf);
}

static void test_pmu_hardware(void)
{
    if (!battery_is_available()) {
        test_skip("pmu_hardware", "AXP2101 not ready");
        return;
    }

    /* 1) 完整状态快照 */
    battery_info_t info;
    memset(&info, 0, sizeof(info));
    esp_err_t err = battery_read_info(&info);
    test_check("pmu_read_status", err == ESP_OK);
    if (err != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "PMU: %umV (vsys %umV), vbus=%d bat=%d, %s/%s",
             info.vbat_mv, info.vsys_mv, info.vbus_present, info.battery_present,
             axp2101_current_dir_name(info.current_dir),
             axp2101_charge_stage_name(info.charge_stage));
    char gauge_str[8];
    char pct_str[8];
    if (info.gauge_percent == 0xFF) {
        snprintf(gauge_str, sizeof(gauge_str), "n/a");
    } else {
        snprintf(gauge_str, sizeof(gauge_str), "%u%%", info.gauge_percent);
    }
    if (info.percent == 0xFF) {
        snprintf(pct_str, sizeof(pct_str), "n/a");
    } else {
        snprintf(pct_str, sizeof(pct_str), "%u%%", info.percent);
    }
    ESP_LOGI(TAG, "PMU battery: gauge=%s -> display=%s%s", gauge_str, pct_str,
             info.percent_from_voltage ? " (voltage fallback)" : "");

    /* 2) 电压合理区间（电池不在位时只记录、不判失败：可能正用 USB 供电） */
    char detail[64];
    snprintf(detail, sizeof(detail), "%umV, battery_present=%d",
             info.vbat_mv, info.battery_present);
    if (info.battery_present) {
        if (info.vbat_mv >= 2000 && info.vbat_mv <= 4500) {
            test_pass("pmu_vbat_range");
        } else {
            test_fail("pmu_vbat_range", detail);
        }
    } else {
        test_skip("pmu_vbat_range", "no battery detected");
    }

    /* 3) 电压采样噪声：连读 8 次，记录峰峰值（首板上板先看数据，不设硬阈值） */
    uint16_t vmin = 0xFFFF, vmax = 0;
    bool samples_ok = true;
    for (int i = 0; i < 8; i++) {
        uint16_t mv = 0;
        if (axp2101_read_vbat_mv(&mv) != ESP_OK) {
            samples_ok = false;
            break;
        }
        if (mv < vmin) { vmin = mv; }
        if (mv > vmax) { vmax = mv; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (samples_ok) {
        ESP_LOGI(TAG, "vbat 8 samples: %u..%u mV (p-p %u mV)", vmin, vmax,
                 (unsigned)(vmax - vmin));
        test_check("pmu_vbat_samples", true);
    } else {
        test_fail("pmu_vbat_samples", "I2C read failed");
    }

    /* 4) 电量计读数：0xA4 只保证 0~100，越界由驱动拒绝（返回 0xFF） */
    uint8_t soc = 0;
    err = axp2101_read_soc(&soc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "gauge SOC = %u%%", soc);
        test_check("pmu_gauge_soc", soc <= 100);
    } else {
        test_skip("pmu_gauge_soc", esp_err_to_name(err));
    }

    /* 5) TS 引脚：判断板上有没有 NTC（≈500mV=NTC，≈0V=下拉到地，其它=悬空） */
    uint16_t ts_mv = 0;
    if (axp2101_read_ts_mv(&ts_mv) == ESP_OK) {
        ESP_LOGI(TAG, "TS pin = %u mV (%s)", ts_mv,
                 (ts_mv < 50) ? "pulled to GND, no NTC"
                              : (ts_mv > 300 && ts_mv < 800) ? "NTC present"
                                                             : "floating/unexpected");
        test_pass("pmu_ts_pin");
    } else {
        test_fail("pmu_ts_pin", "read failed");
    }

    /* 6) 长按阈值（只读；默认值来自 EFUSE，需上板确认） */
    uint8_t key_level = 0;
    if (axp2101_read_reg(AXP2101_REG_KEY_LEVEL, &key_level) == ESP_OK) {
        uint8_t on = 0, off = 0, irq = 0;
        axp2101_decode_key_levels(key_level, &on, &off, &irq);
        ESP_LOGI(TAG, "key levels: ONLEVEL=%u OFFLEVEL=%u IRQLEVEL=%u (0x27=0x%02X)",
                 on, off, irq, key_level);
        test_pass("pmu_key_levels_read");
    } else {
        test_fail("pmu_key_levels_read", "read failed");
    }

    /* 7) 开机来源 / 上次关机原因 */
    axp2101_boot_info_t boot;
    if (axp2101_read_boot_info(&boot) == ESP_OK) {
        ESP_LOGI(TAG, "boot src=0x%02X (key=%d vbus=%d bat_insert=%d), "
                      "last off=0x%02X (soft=%d longpress=%d vsys_uv=%d)",
                 boot.pwron_status, boot.pwron_status & 1,
                 (boot.pwron_status >> 2) & 1, (boot.pwron_status >> 4) & 1,
                 boot.pwr_off_status, (boot.pwr_off_status >> 1) & 1,
                 boot.pwr_off_status & 1, (boot.pwr_off_status >> 3) & 1);
        test_pass("pmu_boot_info");
    } else {
        test_fail("pmu_boot_info", "read failed");
    }

    /* 8) 中断状态读 + 写 1 清（不使能新中断源，也不动按键配置） */
    uint8_t st[3] = { 0 };
    bool any = false;
    if (axp2101_read_irq_status(st, &any) == ESP_OK) {
        ESP_LOGI(TAG, "IRQ status: 0x48=%02X 0x49=%02X 0x4A=%02X%s",
                 st[0], st[1], st[2], any ? " (cleared)" : "");
        test_pass("pmu_irq_status");
    } else {
        test_fail("pmu_irq_status", "read/clear failed");
    }

    /* 9) 充电配置寄存器（只读：确认 ICC/ITERM/CV 当前档位） */
    uint8_t icc_reg = 0, iterm_reg = 0, cv_reg = 0;
    if (axp2101_read_reg(AXP2101_REG_ICC, &icc_reg) == ESP_OK
        && axp2101_read_reg(AXP2101_REG_ITERM, &iterm_reg) == ESP_OK
        && axp2101_read_reg(AXP2101_REG_CV, &cv_reg) == ESP_OK) {
        ESP_LOGI(TAG, "charge cfg: ICC code=0x%02X (%u mA), ITERM code=0x%X, CV code=%u",
                 icc_reg & 0x1F, axp2101_icc_ma_from_code(icc_reg), iterm_reg & 0x0F,
                 cv_reg & 0x07);
        test_pass("pmu_charge_cfg_read");
    } else {
        test_fail("pmu_charge_cfg_read", "read failed");
    }

    /* 10) 寄存器转储（排查用；读取 0x34/0x35 的顺序正好是先高后低） */
    pmu_dump_regs();
    test_pass("pmu_reg_dump");
}

/* ------------------------------------------------------------------ */
/* 三、真机测试：PCF85063A（读时间 + RAM_byte 写读回/恢复）             */
/* ------------------------------------------------------------------ */

static void rtc_dump_regs(void)
{
    char buf[192];
    int pos = 0;
    buf[0] = '\0';
    for (uint8_t reg = 0; reg <= 0x11 && pos < (int)sizeof(buf) - 8; reg++) {
        uint8_t v = 0;
        if (rtc_read_reg(reg, &v) == ESP_OK) {
            pos += snprintf(&buf[pos], sizeof(buf) - (size_t)pos, "%02X=%02X ", reg, v);
        }
    }
    ESP_LOGI(TAG, "RTC regs: %s", buf);
}

static void test_rtc_hardware(void)
{
    if (!rtc_is_ready()) {
        test_skip("rtc_hardware", "PCF85063A not ready");
        return;
    }

    /* 1) 读时间 */
    rtc_time_t t;
    memset(&t, 0, sizeof(t));
    esp_err_t err = rtc_get_time(&t);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC time: %04u-%02u-%02u %02u:%02u:%02u wd=%u",
                 t.year, t.month, t.day, t.hour, t.minute, t.second, t.weekday);
        test_pass("rtc_get_time");
    } else {
        test_fail("rtc_get_time", esp_err_to_name(err));
        return;
    }

    /* 2) 有效性（OS 位 + 字段合理 + 年份下限） */
    bool valid = false, osf = false;
    rtc_is_valid(&valid);
    rtc_get_osf(&osf);
    ESP_LOGI(TAG, "RTC valid=%d OS=%d (time %s)", valid, osf,
             osf ? "needs setting" : "is trustworthy");
    test_pass("rtc_validity");

    /* 3) CLKOUT 应已被 init 关闭（COF=111） */
    uint8_t c2 = 0;
    if (rtc_read_reg(RTC_REG_CONTROL_2, &c2) == ESP_OK) {
        test_check("rtc_clkout_off", (c2 & RTC_C2_COF_MASK) == RTC_C2_COF_MASK);
    } else {
        test_fail("rtc_clkout_off", "read failed");
    }

    /* 4) RAM_byte（03h）写读回：芯片里唯一的自由 RAM，写坏也不影响走时；
          测试后恢复原值。 */
    uint8_t ram_orig = 0, ram_read = 0;
    if (rtc_read_reg(RTC_REG_RAM_BYTE, &ram_orig) == ESP_OK
        && rtc_write_reg(RTC_REG_RAM_BYTE, 0xA5) == ESP_OK
        && rtc_read_reg(RTC_REG_RAM_BYTE, &ram_read) == ESP_OK
        && rtc_write_reg(RTC_REG_RAM_BYTE, ram_orig) == ESP_OK) {
        char detail[48];
        snprintf(detail, sizeof(detail), "wrote 0xA5, read 0x%02X", ram_read);
        if (ram_read == 0xA5) {
            test_pass("rtc_ram_roundtrip");
        } else {
            test_fail("rtc_ram_roundtrip", detail);
        }
    } else {
        test_fail("rtc_ram_roundtrip", "I2C failed");
    }

    /* 5) 走时校验：等 1.1 秒再读，秒应有 1~2 的增量（跨分钟/小时也成立）。
          ⚠ 只读不写，不会改动用户时间。 */
    rtc_time_t t1, t2;
    if (rtc_get_time(&t1) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1100));
        if (rtc_get_time(&t2) == ESP_OK) {
            int32_t s1 = (int32_t)t1.hour * 3600 + t1.minute * 60 + t1.second;
            int32_t s2 = (int32_t)t2.hour * 3600 + t2.minute * 60 + t2.second;
            int32_t delta = (s2 - s1 + 86400) % 86400;
            char detail[64];
            snprintf(detail, sizeof(detail), "delta=%lds (expect 1~2)",
                     (long)delta);
            if (delta >= 1 && delta <= 2) {
                test_pass("rtc_ticks");
            } else {
                test_fail("rtc_ticks", detail);
            }
        } else {
            test_fail("rtc_ticks", "second read failed");
        }
    } else {
        test_fail("rtc_ticks", "first read failed");
    }

    /* 6) Offset（频率微调）寄存器：只读，确认当前补偿值（本板无 CLKOUT，
          无法自动测频，仅记录） */
    uint8_t off = 0;
    if (rtc_read_reg(RTC_REG_OFFSET, &off) == ESP_OK) {
        int8_t raw = (int8_t)(off & 0x7F);
        int lsb = (raw & 0x40) ? (int)raw - 128 : (int)raw;
        ESP_LOGI(TAG, "offset: MODE=%u OFFSET=%d LSB (%.1f ppm/LSB)",
                 (off >> 7) & 1, lsb, ((off >> 7) & 1) ? 4.069 : 4.34);
        test_pass("rtc_offset_read");
    } else {
        test_fail("rtc_offset_read", "read failed");
    }

/* 7) 寄存器转储（18 个寄存器，排查用） */
    rtc_dump_regs();
    test_pass("rtc_reg_dump");

#ifdef CONFIG_RTC_SELFTEST_AUTO_INIT
    /* 8) 首次校时路径（破坏性，默认关闭）：
          把时间写成 2000-01-01（年份 < 2024 → rtc_is_valid=false，等价于
          "出厂默认/从未校时"；OS 位是硬件置位、软件写 1 无效，不能用来模拟），
          再调用 rtc_init_time_if_invalid()，校验时间被写成项目诞生日
          （2026-08-30 13:45:41）且有效性恢复。失败时恢复原时间。 */
    {
        rtc_time_t saved;
        const bool have_saved = (rtc_get_time(&saved) == ESP_OK
                                 && rtc_time_is_sane(&saved));

        const rtc_time_t old = {
            .year = 2000, .month = 1, .day = 1,
            .hour = 0, .minute = 0, .second = 0, .weekday = 6,
        };
        const bool forced = (rtc_set_time(&old) == ESP_OK);

        bool invalid_seen = false;
        bool wrote = false;
        bool epoch_ok = false;
        esp_err_t auto_err = ESP_FAIL;
        if (forced) {
            bool valid = true;
            rtc_is_valid(&valid);
            invalid_seen = !valid;

            auto_err = rtc_init_time_if_invalid(&wrote);
            if (auto_err == ESP_OK && wrote) {
                rtc_time_t t;
                bool after_valid = false;
                if (rtc_get_time(&t) == ESP_OK
                    && rtc_is_valid(&after_valid) == ESP_OK) {
                    epoch_ok = after_valid
                               && t.year == 2026 && t.month == 8 && t.day == 30;
                    ESP_LOGI(TAG, "auto-init result: %04u-%02u-%02u %02u:%02u:%02u",
                             t.year, t.month, t.day, t.hour, t.minute, t.second);
                }
            }
        }

        const bool ok = forced && invalid_seen && wrote && epoch_ok;
        if (!ok) {
            ESP_LOGW(TAG, "auto-init diag: forced=%d invalid_seen=%d err=%s "
                          "wrote=%d epoch_ok=%d",
                     forced, invalid_seen, esp_err_to_name(auto_err), wrote, epoch_ok);
            /* 测试失败：把用户原来的时间写回去，避免把设备留在 2000 年 */
            if (have_saved) {
                rtc_set_time(&saved);
            }
        }
        test_check("rtc_auto_init_project_epoch", ok);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* 任务入口                                                             */
/* ------------------------------------------------------------------ */

static void hw_selftest_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "===== AXP2101 / PCF85063A driver self-test start =====");
    ESP_LOGI(TAG, "note: power-off (0x10[0]) is intentionally NOT tested");

    test_pmu_icc_encoding();
    test_pmu_soc_interp();
    test_pmu_status_decode();
    test_pmu_key_levels_encode();
    test_rtc_bcd();
    test_rtc_weekday();
    test_rtc_decode();
    test_rtc_sanity();
    test_battery_fuse();

    test_pmu_hardware();
    test_rtc_hardware();

    ESP_LOGI(TAG, "===== HWST summary: %d passed, %d failed, %d skipped =====",
             s_pass, s_fail, s_skip);
    if (s_fail == 0) {
        ESP_LOGI(TAG, "HWST RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "HWST RESULT: FAIL");
    }

    vTaskDelete(NULL);
}

void hw_selftest_run(void)
{
    if (s_started) {
        return;
    }
    s_started = true;

    /* 栈放 PSRAM：自检只做 I²C 读 + 延时，不写 flash（不会触发关缓存断言）。
       优先级 3、Core0，让 UI/音频随时可以抢占。 */
    if (task_create_psram_stack(hw_selftest_task, "hw_selftest", 4096, NULL, 3,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "create hw_selftest task failed");
    }
}
