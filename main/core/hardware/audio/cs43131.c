#include "core/hardware/audio/cs43131.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cs43131";

/* ------------------------------------------------------------------ */
/* 引脚与 I2C 配置                                                      */
/* ------------------------------------------------------------------ */

#define CS43131_I2C_PORT   I2C_NUM_0      /* 与触摸共用总线（touch_init 已安装） */
#define CS43131_RST_PIN    GPIO_NUM_41    /* RESET#，低有效，经 3.3→1.8V 转换 */

/* ------------------------------------------------------------------ */
/* 寄存器地址（数据手册 DS1155，寄存器为 24 位地址）                      */
/* ------------------------------------------------------------------ */

#define REG_DEVID_AB       0x010000  /* 设备 ID A/B（只读，期望 0x43） */
#define REG_DEVID_CD       0x010001  /* 设备 ID C/D（只读，期望 0x13） */
#define REG_DEVID_E        0x010002  /* 设备 ID E（只读，期望 0x10） */
#define REG_MCLK_SRC       0x010006  /* 系统时钟源控制（MCLK_INT + MCLK_SRC_SEL） */
#define REG_SAMPLE_RATE    0x01000B  /* ASP 采样率（ASP_SPRATE） */
#define REG_SAMPLE_LEN     0x01000C  /* 串口样本位宽（XSP_SPSIZE + ASP_SPSIZE） */
#define REG_PAD_CFG        0x01000D  /* 引脚接口配置（从机模式下不影响） */
#define REG_POP_FREE       0x010010  /* pop-free 设置 */
#define REG_PWR_CTL        0x020000  /* 电源控制（各 PDN 位） */
#define REG_XTAL_CTL       0x020052  /* 晶振驱动电流（XTAL_IBIAS） */
#define REG_ASP_CLK_CFG    0x040018  /* ASP 时钟配置（主从、SCLK/LRCK 极性） */
#define REG_ASP_FRAME_CFG  0x040019  /* ASP 帧配置（起始相位、50/50、帧延迟） */
#define REG_ASP_CH_LOC_A   0x050000  /* ASP 通道 1 位置 */
#define REG_ASP_CH_LOC_B   0x050001  /* ASP 通道 2 位置 */
#define REG_ASP_CH1_SIZE   0x05000A  /* ASP 通道 1 尺寸与使能 */
#define REG_ASP_CH2_SIZE   0x05000B  /* ASP 通道 2 尺寸与使能 */
#define REG_HP_OUT_CTL1    0x080000  /* 耳机输出控制 1（OUT_FS 满幅电压） */
#define REG_HP_CTL         0x080032  /* 耳机控制（pop-free 配套） */
#define REG_PCM_FILTER     0x090000  /* PCM 滤波器选择 */
#define REG_PCM_VOL_B      0x090001  /* PCM 音量 B */
#define REG_PCM_VOL_A      0x090002  /* PCM 音量 A */
#define REG_PCM_MUTE_CTL   0x090003  /* PCM 静音/软斜坡/自动静音控制 */
#define REG_PCM_PATH_CTL   0x090004  /* PCM 路径信号控制 2（极性/复制/交换） */
#define REG_HV_MODE        0x0B0000  /* Class H 高压模式控制 */
#define REG_INT_STATUS1    0x0F0000  /* 中断状态 1（读一次清 sticky） */
#define REG_INT_MASK1      0x0F0010  /* 中断掩码 1（bit0 = PDN_DONE_INT_MASK，0 使能） */

/* 0x010006 系统时钟源：Direct XTAL + 24.576MHz（MCLK_SRC_SEL=00, MCLK_INT=0） */
#define MCLK_DIRECT_24576   0x00

/* ASP 采样率编码（0x01000B，ASP_SPRATE） */
#define ASP_RATE_32000      0x00
#define ASP_RATE_44100      0x01
#define ASP_RATE_48000      0x02

/* 内部状态 */
static uint8_t  s_i2c_addr      = 0x30;  /* 探测后锁定的设备地址 */
static bool     s_ready         = false; /* 设备是否可用 */
static uint32_t s_cur_rate      = 48000; /* 当前采样率 */

/* ------------------------------------------------------------------ */
/* I2C 读写（帧格式：3 字节 MAP 地址 + 1 字节控制 + 数据）                */
/* ------------------------------------------------------------------ */

/* 控制字节（数据手册 Table 4-13）：bit0=INCR(自动递增)，bit[2:1]=SIZE(访问宽度)。
   单字节访问无需自动递增，故控制字节 = 0x00（INCR=0, SIZE=00=8bit）。 */
#define CS43131_CTRL_BYTE  0x00

static esp_err_t cs43131_write_reg(uint32_t addr, uint8_t dat)
{
    uint8_t buf[5];
    buf[0] = (uint8_t)((addr >> 16) & 0xff);
    buf[1] = (uint8_t)((addr >> 8) & 0xff);
    buf[2] = (uint8_t)(addr & 0xff);
    buf[3] = CS43131_CTRL_BYTE;
    buf[4] = dat;

    esp_err_t ret = i2c_master_write_to_device(CS43131_I2C_PORT, s_i2c_addr,
                                               buf, sizeof(buf), pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write 0x%06lx = 0x%02x failed: %s",
                 (unsigned long)addr, dat, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t cs43131_read_reg(uint32_t addr, uint8_t *dst)
{
    uint8_t wbuf[4];
    wbuf[0] = (uint8_t)((addr >> 16) & 0xff);
    wbuf[1] = (uint8_t)((addr >> 8) & 0xff);
    wbuf[2] = (uint8_t)(addr & 0xff);
    wbuf[3] = CS43131_CTRL_BYTE;

    return i2c_master_write_read_device(CS43131_I2C_PORT, s_i2c_addr,
                                        wbuf, sizeof(wbuf), dst, 1,
                                        pdMS_TO_TICKS(200));
}

/* ------------------------------------------------------------------ */
/* 电源域控制（读-改-写，清除对应 PDN 位 = 上电指定电源域）               */
/* ------------------------------------------------------------------ */

static esp_err_t cs43131_power_up_domain(uint8_t pdn_bits)
{
    uint8_t reg = 0;
    esp_err_t ret = cs43131_read_reg(REG_PWR_CTL, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    reg = (uint8_t)(reg & ~pdn_bits);
    return cs43131_write_reg(REG_PWR_CTL, reg);
}

/* ------------------------------------------------------------------ */
/* 设备探测：在 CS43131 可能的地址上做 ACK + 设备 ID 双重校验             */
/* ------------------------------------------------------------------ */

static bool cs43131_probe_addr(uint8_t addr, uint8_t *id_ab, uint8_t *id_cd, uint8_t *id_e)
{
    /* 只发设备地址 + STOP，验证 ACK */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(CS43131_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return false;
    }

    s_i2c_addr = addr;
    uint8_t a = 0, b = 0, c = 0;
    if (cs43131_read_reg(REG_DEVID_AB, &a) != ESP_OK ||
        cs43131_read_reg(REG_DEVID_CD, &b) != ESP_OK ||
        cs43131_read_reg(REG_DEVID_E, &c) != ESP_OK) {
        return false;
    }

    if (a == 0x43 && b == 0x13 && c == 0x10) {
        *id_ab = a;
        *id_cd = b;
        *id_e = c;
        return true;
    }
    return false;
}

static bool cs43131_detect(void)
{
    /* CS43131 的 7 位地址 = 5 位基址 + 2 位 ADR。基址实测为 0b01100（0x30），
       兼容 0b01000（0x20）与全部 4 种 ADR 组合，避免因 ADR 电阻差异漏检。 */
    static const uint8_t cand[] = { 0x30, 0x20, 0x31, 0x21, 0x32, 0x22, 0x33, 0x23 };
    for (size_t i = 0; i < sizeof(cand); i++) {
        uint8_t id_ab = 0, id_cd = 0, id_e = 0;
        if (cs43131_probe_addr(cand[i], &id_ab, &id_cd, &id_e)) {
            s_i2c_addr = cand[i];
            ESP_LOGI(TAG, "CS43131 found at I2C addr 0x%02x, ID = %02x %02x %02x",
                     s_i2c_addr, id_ab, id_cd, id_e);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 完整配置序列（init 与 power_up 共用）                                 */
/* ------------------------------------------------------------------ */

/* 应用 CS43131 完整寄存器配置（晶振 → 直通 MCLK → ASP 从机 → HP/pop-free →
   PCM 路径）。前置条件：设备已探测到，电源域处于默认（全掉电）状态。
   PCM 音量寄存器（0x090001/0x090002）不在此写——音量由 audio 层
   audio_set_volume 统一管理，避免开机恢复配置时把用户音量重置回 0dB。 */
static esp_err_t cs43131_apply_full_config(void)
{
    uint8_t scratch = 0;

    /* 晶振驱动电流：0x04 = 12.5µA（默认值，本板 24.576MHz 晶振实测可用） */
    esp_err_t ret = cs43131_write_reg(REG_XTAL_CTL, 0x04);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 清中断 sticky 位（读 Interrupt Status 1） */
    cs43131_read_reg(REG_INT_STATUS1, &scratch);

    /* 上电晶振并等待起振（tXTAL_pup 约 6.5ms，留足裕量） */
    ret = cs43131_power_up_domain(CS43131_PWR_XTAL);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 切到直通 MCLK（24.576MHz 晶振），并等 150µs（手册要求切换后等待） */
    ret = cs43131_write_reg(REG_MCLK_SRC, MCLK_DIRECT_24576);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    /* ASP 从机接口配置（标准 I2S，32bit/sample）：
        - 0x040018 = 0x0C：Slave（bit4=0）| SCPOL_IN=1（SCLK 下降沿采样，
          与 ESP32 主机 bclk_inv=false 输出匹配，手册 I2S 从机推荐极性）
        - 0x040019 = 0x0A：LRCK 50/50 | 帧起始延迟 1 个 SCLK（匹配 I2S Philips）
        - 0x05000A/B = 0x07/0x0F：通道 1/2 使能，各 32bit，通道 1 低相位、通道 2 高相位 */
    ret = cs43131_write_reg(REG_SAMPLE_RATE, ASP_RATE_48000); /* 默认 48kHz */
    if (ret != ESP_OK) {
        return ret;
    }
    cs43131_write_reg(REG_SAMPLE_LEN, 0x04);            /* XSP=24bit(忽略)，ASP=32bit */
    cs43131_write_reg(REG_PAD_CFG, 0x00);               /* 从机模式下时钟为输入，不受影响 */
    ret = cs43131_write_reg(REG_ASP_CLK_CFG, 0x0C);     /* 从机 + SCPOL_IN=1（手册 I2S 从机推荐：SCLK 下降沿采样） */
    if (ret != ESP_OK) {
        return ret;
    }
    cs43131_write_reg(REG_ASP_FRAME_CFG, 0x0A);         /* I2S 输入：50/50 + 帧延迟 1 */
    cs43131_write_reg(REG_ASP_CH_LOC_A, 0x00);          /* 左声道从 SCLK0 开始（关键！） */
    cs43131_write_reg(REG_ASP_CH_LOC_B, 0x00);          /* 右声道从 SCLK0 开始 */
    ret = cs43131_write_reg(REG_ASP_CH1_SIZE, 0x07);    /* CH1 32bit，使能，LRCK 低相位 */
    if (ret != ESP_OK) {
        return ret;
    }
    cs43131_write_reg(REG_ASP_CH2_SIZE, 0x0F);          /* CH2 32bit，使能，LRCK 高相位 */

    /* 耳机输出：Class H（自适应，HV_EN=1），满幅 1.73Vrms */
    cs43131_write_reg(REG_HV_MODE, 0x1E);               /* ADPT_PWR=111(自适应) HV_EN=1 */
    cs43131_write_reg(REG_HP_OUT_CTL1, 0x30);           /* OUT_FS=11 → 1.73Vrms */

    /* 上电 ASP 数据路径 */
    ret = cs43131_power_up_domain(CS43131_PWR_ASP);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 上电耳机放大器：先写 pop-free 设置，再上电，等 12ms 后恢复 */
    cs43131_write_reg(REG_POP_FREE, 0x99);
    cs43131_write_reg(REG_HP_CTL, 0x20);
    ret = cs43131_power_up_domain(CS43131_PWR_HP);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(12));
    cs43131_write_reg(REG_HP_CTL, 0x00);
    cs43131_write_reg(REG_POP_FREE, 0x00);

    /* PCM 数据路径：
        - 0x090000 = 0x02：快速滤波器 + 低延迟 + 高通使能（默认值）
        - 0x090003 = 0xEC：软斜坡 | 音量 A 同时控制两声道 | 自动静音 | 两声道未静音
        （PCM 音量 0x090001/0x090002 由 audio 层管理，这里不写，见函数头注释） */
    cs43131_write_reg(REG_PCM_FILTER, 0x02);
    ret = cs43131_write_reg(REG_PCM_MUTE_CTL, 0xEC);    /* 软斜坡 + 音量A控双声道 + 未静音 */
    if (ret != ESP_OK) {
        return ret;
    }
    cs43131_write_reg(REG_PCM_PATH_CTL, 0x00);          /* 不反相/不交换/不复制 */

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t cs43131_init(void)
{
    /* 1. 硬件复位：RESET# 拉低 ≥1ms 后拉高，等待 tPUD（1.5ms）+ 余量 */
    gpio_reset_pin(CS43131_RST_PIN);
    gpio_set_direction(CS43131_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CS43131_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(CS43131_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2. 探测设备（ACK + 设备 ID 双重校验） */
    if (!cs43131_detect()) {
        ESP_LOGE(TAG, "CS43131 not found on I2C bus, audio unavailable");
        s_ready = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_ready = true;

    /* 3. 完整配置序列（晶振 → MCLK → ASP → HP → PCM） */
    esp_err_t ret = cs43131_apply_full_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "apply full config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 读回关键寄存器做校验 */
    uint8_t v = 0;
    cs43131_read_reg(REG_PWR_CTL, &v);
    ESP_LOGI(TAG, "reg 0x020000 (pwr)  = 0x%02x", v);
    cs43131_read_reg(REG_MCLK_SRC, &v);
    ESP_LOGI(TAG, "reg 0x010006 (mclk) = 0x%02x", v);
    cs43131_read_reg(REG_ASP_CLK_CFG, &v);
    ESP_LOGI(TAG, "reg 0x040018 (asp)  = 0x%02x", v);
    cs43131_read_reg(REG_ASP_CH_LOC_A, &v);
    ESP_LOGI(TAG, "reg 0x050000 (ch1loc)= 0x%02x", v);

    s_cur_rate = 48000;
    ESP_LOGI(TAG, "CS43131 init OK (48kHz, direct XTAL 24.576MHz)");
    return ESP_OK;
}

esp_err_t cs43131_set_sample_rate(uint32_t sample_rate)
{
    uint8_t rate_reg;

    if (sample_rate == 32000) {
        rate_reg = ASP_RATE_32000;
    } else if (sample_rate == 44100) {
        rate_reg = ASP_RATE_44100;
    } else if (sample_rate == 48000) {
        rate_reg = ASP_RATE_48000;
    } else {
        ESP_LOGE(TAG, "unsupported sample rate %lu", (unsigned long)sample_rate);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready) {
        s_cur_rate = sample_rate;
        return ESP_ERR_NOT_FOUND;
    }

    /* 只改 ASP 采样率，不切换 MCLK（始终 24.576MHz 直通），避免切换失响应 */
    esp_err_t ret = cs43131_write_reg(REG_SAMPLE_RATE, rate_reg);
    if (ret != ESP_OK) {
        return ret;
    }
    s_cur_rate = sample_rate;
    ESP_LOGI(TAG, "sample rate -> %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t cs43131_set_volume(uint8_t vol)
{
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 音量 A 控制两声道（PCM_VOL_BEQA=1），但双寄存器同写保持一致性 */
    esp_err_t ret = cs43131_write_reg(REG_PCM_VOL_A, vol);
    if (ret == ESP_OK) {
        ret = cs43131_write_reg(REG_PCM_VOL_B, vol);
    }
    return ret;
}

esp_err_t cs43131_set_mute(bool mute)
{
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t tmp = 0;
    esp_err_t ret = cs43131_read_reg(REG_PCM_MUTE_CTL, &tmp);
    if (ret != ESP_OK) {
        return ret;
    }
    if (mute) {
        tmp |= 0x03;           /* bit[1:0]=0b11 = PCM_MUTE_A/B 均静音 */
    } else {
        tmp &= (uint8_t)~0x03; /* bit[1:0]=0b00 = 两声道取消静音 */
    }
    return cs43131_write_reg(REG_PCM_MUTE_CTL, tmp);
}

uint32_t cs43131_get_sample_rate(void)
{
    return s_cur_rate;
}

/* ------------------------------------------------------------------ */
/* 电源开关（依据数据手册 §5.6/§5.7 耳机上电/掉电时序，供深度休眠使用）    */
/*                                                                     */
/* 关机 = 关闭耳机输出路径（HP → 等 PDN_DONE → ASP），保留 I2C 控制；     */
/* 晶振（PDN_XTAL）按手册保持常开（本板 Direct XTAL 直通 MCLK 依赖它，    */
/* 且晶振功耗极低），PLL/XSP/DSDIF/CLKOUT 本就处于掉电态，不参与操作。    */
/* ------------------------------------------------------------------ */

bool cs43131_is_powered_down(void)
{
    if (!s_ready) {
        return true;   /* 设备不可用视为关机 */
    }
    uint8_t reg = 0;
    if (cs43131_read_reg(REG_PWR_CTL, &reg) != ESP_OK) {
        return true;   /* I2C 读不到也视为关机（异常状态） */
    }
    /* 关机 = 耳机功放与模拟信号路径均已掉电（PDN_HP 与 PDN_ASP 均为 1）；
       晶振常开、其余域保持原值，不参与判断 */
    return (reg & (CS43131_PWR_HP | CS43131_PWR_ASP)) ==
           (CS43131_PWR_HP | CS43131_PWR_ASP);
}

esp_err_t cs43131_power_down(void)
{
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. 解除 PDN_DONE 中断掩码（0xF0010 清 bit0），使能掉电完成中断（读改写） */
    uint8_t reg = 0;
    esp_err_t ret = cs43131_read_reg(REG_INT_MASK1, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_INT_MASK1, (uint8_t)(reg & 0xFE));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 1.5 读一次中断状态，清掉可能残留的 PDN_DONE sticky 位（读即清），
       避免把上次掉电的旧标志误当成本次完成信号 */
    uint8_t stale = 0;
    cs43131_read_reg(REG_INT_STATUS1, &stale);

    /* 2. 先关耳机功放（PDN_HP=1，读改写，不影响晶振等其它位） */
    ret = cs43131_read_reg(REG_PWR_CTL, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_PWR_CTL, (uint8_t)(reg | CS43131_PWR_HP));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 3. 等待 HP 掉电完成（数据手册 Example 5-3 要求等 PDN_DONE_INT）。
       实测结论（手册 §4.12）：PDN_DONE 是"整芯片掉电完成"的边沿锁存事件，
       而本设计晶振保持常开（Direct XTAL 直通 MCLK 依赖它），芯片永远达不到
       完全掉电，故 PDN_DONE 不会置位。这里保留短轮询作为安全网（未来深度
       休眠若连晶振一起关，它能生效），超时 20ms 直接继续——关机前播放已
       暂停（输出已静音），不影响功能。 */
    uint32_t wait = 0;
    uint8_t last_st = 0;
    while (wait < 20) {
        uint8_t st = 0;
        if (cs43131_read_reg(REG_INT_STATUS1, &st) != ESP_OK) {
            break;   /* I2C 异常，放弃等待 */
        }
        last_st = st;
        if (st & 0x01) {
            break;   /* PDN_DONE 置位，HP 掉电完成 */
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        wait += 2;
    }
    if (last_st & 0x01) {
        ESP_LOGI(TAG, "PDN_DONE asserted after %ums", (unsigned)wait);
    } else {
        ESP_LOGI(TAG, "PDN_DONE not asserted in %ums (partial power-down, expected), continuing",
                 (unsigned)wait);
    }

    /* 4. 最后关模拟信号处理路径（PDN_ASP=1，读改写） */
    ret = cs43131_read_reg(REG_PWR_CTL, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_PWR_CTL, (uint8_t)(reg | CS43131_PWR_ASP));
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "CS43131 powered down (HP+ASP off, XTAL kept running)");
    return ESP_OK;
}

esp_err_t cs43131_power_up(void)
{
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. 去 pop（无爆音）上电设置 */
    esp_err_t ret = cs43131_write_reg(REG_POP_FREE, 0x99);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_HP_CTL, 0x20);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 开 ASP（清 PDN_ASP=0，读改写） */
    uint8_t reg = 0;
    ret = cs43131_read_reg(REG_PWR_CTL, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_PWR_CTL, (uint8_t)(reg & ~CS43131_PWR_ASP));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 3. 开 HP（清 PDN_HP=0，读改写） */
    ret = cs43131_read_reg(REG_PWR_CTL, &reg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = cs43131_write_reg(REG_PWR_CTL, (uint8_t)(reg & ~CS43131_PWR_HP));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 等 12ms（HP/DAC 上电稳定，手册要求） */
    vTaskDelay(pdMS_TO_TICKS(12));

    /* 5. 恢复默认设置 */
    cs43131_write_reg(REG_HP_CTL, 0x00);
    cs43131_write_reg(REG_POP_FREE, 0x00);

    ESP_LOGI(TAG, "CS43131 powered up (HP+ASP on)");
    return ESP_OK;
}
