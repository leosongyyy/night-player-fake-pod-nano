#include "core/hardware/axp2101.h"

#include <string.h>

#include "core/utils/task_psram.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "axp2101";

/* 0x34 设备句柄（挂在触摸驱动建立的 I²C0 总线上） */
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_ready = false;

/* 中断状态"读 + 写 1 清"需要跨事务互斥：事件任务与自检都会调用，若不串行，
   后一次读会把前一次已读到的位再读一遍（重复上报），或者把别人刚读到、
   尚未处理的状态位清掉（丢事件）。 */
static SemaphoreHandle_t s_irq_lock = NULL;

/* ------------------------------------------------------------------ */
/* 底层寄存器访问（写操作一律单寄存器一次事务，不依赖 AXP 的地址自增；     */
/* 读操作允许突发读，芯片内部地址指针自增，见参考文档 §2.1）             */
/* ------------------------------------------------------------------ */

static esp_err_t axp2101_write_reg(uint8_t reg, uint8_t value)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
}

static esp_err_t axp2101_write_masked(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old = 0;
    esp_err_t err = axp2101_read_reg(reg, &old);
    if (err != ESP_OK) {
        return err;
    }
    return axp2101_write_reg(reg, (uint8_t)((old & ~mask) | (value & mask)));
}

static esp_err_t axp2101_read_burst(uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_dev == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, AXP2101_I2C_TIMEOUT_MS);
}

esp_err_t axp2101_read_reg(uint8_t reg, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp2101_read_burst(reg, value, 1);
}

/* 14-bit ADC 通道读取：⚠ 必须先读高字节（读它会锁存当前转换结果），再读低字节。
   先读低字节会拿到上一批/错位的数据（参考文档 §5.1）。 */
static esp_err_t axp2101_read_adc14(uint8_t reg_h, uint16_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t h = 0, l = 0;
    esp_err_t err = axp2101_read_reg(reg_h, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = axp2101_read_reg((uint8_t)(reg_h + 1), &l);
    if (err != ESP_OK) {
        return err;
    }
    *out = (uint16_t)(((uint16_t)(h & 0x3F) << 8) | l);   /* 1 mV/LSB */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 状态读取                                                            */
/* ------------------------------------------------------------------ */

esp_err_t axp2101_read_vbat_mv(uint16_t *mv)
{
    return axp2101_read_adc14(AXP2101_REG_VBAT_H, mv);
}

esp_err_t axp2101_read_vsys_mv(uint16_t *mv)
{
    return axp2101_read_adc14(AXP2101_REG_VSYS_H, mv);
}

esp_err_t axp2101_read_vbus_mv(uint16_t *mv)
{
    return axp2101_read_adc14(AXP2101_REG_VBUS_H, mv);
}

esp_err_t axp2101_read_ts_mv(uint16_t *mv)
{
    uint16_t raw = 0;
    esp_err_t err = axp2101_read_adc14(AXP2101_REG_TS_H, &raw);
    if (err != ESP_OK) {
        return err;
    }
    /* ⚠ TS 通道的 ADC 是 **0.5 mV/LSB**（datasheet Table 6-5），
       VBAT/VBUS/VSYS 才是 1 mV/LSB；以前这里把原始码直接当 mV 返回，
       会把 300mV 读成 600。 */
    *mv = (uint16_t)((raw + 1u) >> 1);
    return ESP_OK;
}

esp_err_t axp2101_read_soc(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = 0;
    esp_err_t err = axp2101_read_reg(AXP2101_REG_SOC, &v);
    if (err != ESP_OK) {
        return err;
    }
    /* 电量计只保证 0~100；越界说明电量计没跑起来/数据无效，交给上层走电压兜底 */
    if (v > 100) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *percent = v;
    return ESP_OK;
}

esp_err_t axp2101_read_status(axp2101_status_t *st)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(st, 0, sizeof(*st));

    uint8_t raw[2] = { 0 };
    esp_err_t err = axp2101_read_burst(AXP2101_REG_STATUS1, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    axp2101_parse_status(raw[0], raw[1], st);

    err = axp2101_read_vbat_mv(&st->vbat_mv);
    if (err != ESP_OK) {
        return err;
    }

    /* VSYS / 电量计是"有更好、没有也能活"的辅助信息：失败不返回错误。
       ⚠ 不读 TS：本板 TS 是固定电阻、没有 NTC，温度无意义（见 axp2101_read_ts_mv）。 */
    uint16_t vsys = 0;
    if (axp2101_read_vsys_mv(&vsys) == ESP_OK) {
        st->vsys_mv = vsys;
    }

    uint8_t gauge_cfg = 0;
    if (axp2101_read_reg(AXP2101_REG_GAUGE_CFG, &gauge_cfg) == ESP_OK) {
        st->gauge_enabled = ((gauge_cfg >> 3) & 1u) != 0;
    }

    st->soc_percent = 0xFF;
    uint8_t soc = 0;
    if (axp2101_read_soc(&soc) == ESP_OK) {
        st->soc_percent = soc;
    }
    return ESP_OK;
}

void axp2101_parse_status(uint8_t status1, uint8_t status2, axp2101_status_t *st)
{
    if (st == NULL) {
        return;
    }
    st->raw_status1 = status1;
    st->raw_status2 = status2;

    st->vbus_present    = ((status1 >> 5) & 1u) != 0;
    st->batfet_closed   = ((status1 >> 4) & 1u) == 0;   /* 0 = BATFET 导通 */
    st->battery_present = ((status1 >> 3) & 1u) != 0;

    uint8_t dir = (uint8_t)((status2 >> 5) & 0x3u);
    st->current_dir = (dir <= AXP2101_CURRENT_DISCHARGING)
                          ? (axp2101_current_dir_t)dir
                          : AXP2101_CURRENT_STANDBY;

    uint8_t stage = (uint8_t)(status2 & 0x7u);
    /* 0x01[2:0] 的合法值只到 101b（未充电）；110/111 保留，按未充电处理 */
    st->charge_stage = (stage <= AXP2101_CHG_NOT_CHARGING)
                           ? (axp2101_charge_stage_t)stage
                           : AXP2101_CHG_NOT_CHARGING;

    /* "是否在充电"不能只看电流方向：充电完成的瞬间方向可能已回待机，
       但阶段仍是 DONE（UI 要显示"已充满"）。 */
    st->charging = (st->current_dir == AXP2101_CURRENT_CHARGING)
                   || (st->charge_stage == AXP2101_CHG_PRECHARGE)
                   || (st->charge_stage == AXP2101_CHG_CC)
                   || (st->charge_stage == AXP2101_CHG_CV)
                   || (st->charge_stage == AXP2101_CHG_DONE);
}

esp_err_t axp2101_read_boot_info(axp2101_boot_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2] = { 0 };
    esp_err_t err = axp2101_read_burst(AXP2101_REG_PWRON_STATUS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    info->pwron_status = buf[0];
    info->pwr_off_status = buf[1];
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 中断 / 电源键                                                       */
/* ------------------------------------------------------------------ */

esp_err_t axp2101_read_irq_status(uint8_t status3[3], bool *had_event)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_irq_lock != NULL) {
        xSemaphoreTake(s_irq_lock, portMAX_DELAY);
    }

    uint8_t buf[3] = { 0 };
    esp_err_t err = axp2101_read_burst(AXP2101_REG_IRQ_ST0, buf, sizeof(buf));
    if (err == ESP_OK) {
        bool any = (buf[0] | buf[1] | buf[2]) != 0;
        /* RW1C：把刚读到的置位写回去即清除。只写非零字节，避免多余事务。 */
        for (int i = 0; i < 3 && any; i++) {
            if (buf[i] != 0) {
                esp_err_t clr = axp2101_write_reg((uint8_t)(AXP2101_REG_IRQ_ST0 + i), buf[i]);
                if (clr != ESP_OK) {
                    err = clr;
                }
            }
        }
        if (status3 != NULL) {
            memcpy(status3, buf, sizeof(buf));
        }
        if (had_event != NULL) {
            *had_event = any;
        }
    }

    if (s_irq_lock != NULL) {
        xSemaphoreGive(s_irq_lock);
    }
    return err;
}

esp_err_t axp2101_clear_irq_status(void)
{
    return axp2101_read_irq_status(NULL, NULL);
}

#ifdef CONFIG_PMU_KEY_IRQ

static QueueHandle_t   s_irq_queue = NULL;
static TaskHandle_t    s_irq_task  = NULL;

/* 电源键当前是否按住：由 0x49 的 DOWN/UP 边沿事件维护，供按键服务做双键
   同时按下判定（读写都是单字节，跨任务安全）。 */
static volatile bool s_pwr_key_pressed = false;

/* GPIO49 下降沿 ISR：只做"投递事件"，绝不做 I²C（新驱动会阻塞/拿互斥锁）。
   状态位在任务里读并写 1 清，避免在中断上下文拼事务。 */
static void IRAM_ATTR axp2101_int_isr(void *arg)
{
    (void)arg;
    uint8_t evt = 1;
    BaseType_t hp = pdFALSE;
    if (s_irq_queue != NULL) {
        xQueueSendFromISR(s_irq_queue, &evt, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void axp2101_irq_task(void *arg)
{
    (void)arg;
    uint8_t evt = 0;

    while (1) {
        if (xQueueReceive(s_irq_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* 等 IRQ 线稳定：一次按键可能在极短时间内产生多个边沿 */
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t st[3] = { 0 };
        bool any = false;
        if (axp2101_read_irq_status(st, &any) != ESP_OK) {
            ESP_LOGW(TAG, "read IRQ status failed");
            continue;
        }

        if (any) {
            /* 0x49（st[1]）：电源键四类事件。
               业务语义（短按/长按/双键）统一由 button_service 依据"按住状态"
               生成，这里只维护按住状态并清中断，避免两套阈值/重复事件。
               若某次读同时拿到 DOWN 和 UP（极短点击），按已松开处理。 */
            if (st[1] & AXP2101_IRQ_KEY_DOWN) {
                s_pwr_key_pressed = true;
            }
            if (st[1] & AXP2101_IRQ_KEY_UP) {
                s_pwr_key_pressed = false;
            }
            if (st[1] & (AXP2101_IRQ_KEY_DOWN | AXP2101_IRQ_KEY_UP
                         | AXP2101_IRQ_KEY_SHORT | AXP2101_IRQ_KEY_LONG)) {
                ESP_LOGD(TAG, "PWRON IRQ 0x49=0x%02x, held=%d",
                         st[1], (int)s_pwr_key_pressed);
            }
            /* 其它组理论上不会置位（init 只使能电源键），但若置了就记录，
               方便排查"IRQ 一直低"时到底是谁在拉。 */
            if (st[0] != 0) {
                ESP_LOGI(TAG, "IRQ status0=0x%02x (gauge/temp)", st[0]);
            }
            if (st[1] & ~(AXP2101_IRQ_KEY_SHORT | AXP2101_IRQ_KEY_LONG
                          | AXP2101_IRQ_KEY_DOWN | AXP2101_IRQ_KEY_UP)) {
                ESP_LOGI(TAG, "IRQ status1=0x%02x (vbus/battery)", st[1]);
            }
            if (st[2] != 0) {
                ESP_LOGI(TAG, "IRQ status2=0x%02x (system/charge)", st[2]);
            }
        }

        /* 保护：清完状态后 IRQ 线仍为低，说明还有积压状态或存在未处理源。
           连续读+清 5 次仍为低就关掉电源键中断——否则这条线会持续拉低，
           边沿中断退化成中断风暴，拖死 UI 任务。 */
        for (int retry = 0; retry < 5 && gpio_get_level(AXP2101_INT_GPIO) == 0; retry++) {
            vTaskDelay(pdMS_TO_TICKS(2));
            uint8_t again[3] = { 0 };
            bool any_again = false;
            if (axp2101_read_irq_status(again, &any_again) != ESP_OK) {
                break;
            }
            if (any_again) {
                ESP_LOGD(TAG, "IRQ retry %d: 0x48=%02X 0x49=%02X 0x4A=%02X",
                         retry, again[0], again[1], again[2]);
            }
        }
        if (gpio_get_level(AXP2101_INT_GPIO) == 0) {
            ESP_LOGE(TAG, "PMU IRQ stuck low after clearing, disabling key IRQ");
            gpio_intr_disable(AXP2101_INT_GPIO);
            gpio_isr_handler_remove(AXP2101_INT_GPIO);
            axp2101_write_reg(AXP2101_REG_IRQ_EN1, 0);
            s_irq_task = NULL;
            vTaskDelete(NULL);
        }
    }
}

static esp_err_t axp2101_key_irq_start(void)
{
    if (s_irq_task != NULL) {
        return ESP_OK;
    }
    if (s_irq_queue == NULL) {
        s_irq_queue = xQueueCreate(4, sizeof(uint8_t));
        if (s_irq_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* GPIO49：输入 + 上拉兜底（板载 4.7k 外部上拉，开漏低有效），下降沿触发。
       ⚠ esp_lcd_touch 也可能安装全局 GPIO ISR 服务（安装过会返回
       ESP_ERR_INVALID_STATE，属正常，不是错误）。 */
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << AXP2101_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d) failed: %s", AXP2101_INT_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    /* 只使能"电源键"相关中断（短按/长按/按下/抬起）：其余（电量计、插拔、
       充电）由 battery 模块轮询状态寄存器获取。默认全开的 IRQ 会让本板 IRQ
       线被"电量计 SOC 更新"之类的事件持续拉低，变成中断风暴。
       ⚠ 必须包含 DOWN/UP（0x41[1:0]）：按键服务要靠"按住状态"做双键判定。 */
    axp2101_write_reg(AXP2101_REG_IRQ_EN0, 0x00);
    axp2101_write_reg(AXP2101_REG_IRQ_EN1,
                      AXP2101_IRQ_KEY_SHORT | AXP2101_IRQ_KEY_LONG
                      | AXP2101_IRQ_KEY_DOWN | AXP2101_IRQ_KEY_UP);
    axp2101_write_reg(AXP2101_REG_IRQ_EN2, 0x00);
    /* 使能前可能已有状态位积压，先清一次，否则 IRQ 线一开始就是低的 */
    axp2101_clear_irq_status();

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_isr_handler_add(AXP2101_INT_GPIO, axp2101_int_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add(GPIO%d) failed: %s", AXP2101_INT_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    /* 事件任务：栈放 PSRAM（只做 I²C + 日志，不写 flash），优先级 4，Core0 */
    if (task_create_psram_stack(axp2101_irq_task, "pmu_irq", 3072, NULL, 4,
                                &s_irq_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "create PMU IRQ task failed");
        gpio_isr_handler_remove(AXP2101_INT_GPIO);
        gpio_intr_disable(AXP2101_INT_GPIO);
        axp2101_write_reg(AXP2101_REG_IRQ_EN1, 0);
        s_irq_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PWRON IRQ enabled: GPIO%d falling edge, "
                  "short/long + press/release edges", AXP2101_INT_GPIO);
    return ESP_OK;
}

#else  /* !CONFIG_PMU_KEY_IRQ */

bool axp2101_is_power_key_pressed(void)
{
    return false;
}

#endif /* CONFIG_PMU_KEY_IRQ */

#ifdef CONFIG_PMU_KEY_IRQ
bool axp2101_is_power_key_pressed(void)
{
    return s_pwr_key_pressed;
}
#endif

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

uint8_t axp2101_encode_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel)
{
    /* 0x27：[5:4] IRQLEVEL、[3:2] OFFLEVEL、[1:0] ONLEVEL；[7:6] 是只读保留位，
       所以本函数只生成低 6 位，写入时由 axp2101_set_key_levels 做读-改-写。 */
    return (uint8_t)(((irqlevel & 0x3u) << 4) | ((offlevel & 0x3u) << 2)
                     | (onlevel & 0x3u));
}

void axp2101_decode_key_levels(uint8_t reg, uint8_t *onlevel, uint8_t *offlevel,
                               uint8_t *irqlevel)
{
    if (onlevel != NULL) {
        *onlevel = (uint8_t)(reg & 0x3u);
    }
    if (offlevel != NULL) {
        *offlevel = (uint8_t)((reg >> 2) & 0x3u);
    }
    if (irqlevel != NULL) {
        *irqlevel = (uint8_t)((reg >> 4) & 0x3u);
    }
}

esp_err_t axp2101_set_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel)
{
    /* ⚠ 必须读-改-写：0x27 的 [7:6] 是只读保留位，整字节写可能踩到未定义位 */
    uint8_t v = axp2101_encode_key_levels(onlevel, offlevel, irqlevel);
    esp_err_t err = axp2101_write_masked(AXP2101_REG_KEY_LEVEL, 0x3F, v);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "key levels set: ONLEVEL=%u OFFLEVEL=%u IRQLEVEL=%u",
                 onlevel, offlevel, irqlevel);
    }
    return err;
}

esp_err_t axp2101_configure_charge(uint16_t icc_ma, uint8_t iterm_code, bool cv_42v)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t icc_code = axp2101_icc_code_from_ma(icc_ma);
    esp_err_t err = axp2101_write_masked(AXP2101_REG_ICC, 0x1F, icc_code);
    if (err != ESP_OK) {
        return err;
    }

    /* 0x63：[4] 截止使能（保持 1）、[3:0] 截止电流档位 */
    err = axp2101_write_masked(AXP2101_REG_ITERM, 0x1F,
                               (uint8_t)(0x10 | (iterm_code & 0x0Fu)));
    if (err != ESP_OK) {
        return err;
    }

    if (cv_42v) {
        /* 0x64[2:0] = 011b 对应 4.2 V（600 mAh 电芯满电电压，不要设 4.35/4.4） */
        err = axp2101_set_charge_voltage(AXP2101_CV_4V2);
    }

    ESP_LOGI(TAG, "charge config: ICC %u mA (code 0x%02X), ITERM code 0x%X, CV %s",
             axp2101_icc_ma_from_code(icc_code), icc_code, (unsigned)(iterm_code & 0x0F),
             cv_42v ? "4.2V" : "unchanged");
    return err;
}

esp_err_t axp2101_set_charge_voltage(axp2101_cv_t cv)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只用 4.0 / 4.1 / 4.2V 三档；100b(4.35V)/101b(4.4V)/000b(手册标 5.0V)
       对单节 4.2V 电芯都是错误档位，直接拒绝，避免过充。 */
    if (cv != AXP2101_CV_4V0 && cv != AXP2101_CV_4V1 && cv != AXP2101_CV_4V2) {
        ESP_LOGE(TAG, "refuse unsafe CV code %d (only 4.0/4.1/4.2V allowed)",
                 (int)cv);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = axp2101_write_masked(AXP2101_REG_CV, 0x07, (uint8_t)cv);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "charge CV -> %.1f V%s", cv == AXP2101_CV_4V0 ? 4.0
                                        : cv == AXP2101_CV_4V1 ? 4.1 : 4.2,
                 cv == AXP2101_CV_4V0 ? " (battery health protection)" : "");
    } else {
        ESP_LOGW(TAG, "set charge CV failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t axp2101_set_charge_enable(bool enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t v = 0;
    esp_err_t err = axp2101_read_reg(AXP2101_REG_GAUGE_CFG, &v);
    if (err != ESP_OK) {
        return err;
    }
    /* 只改 bit1（充电使能）：bit3 电量计保持 1、bit0 看门狗保持 0 */
    const uint8_t nv = enable ? (uint8_t)(v | 0x02u) : (uint8_t)(v & ~0x02u);
    if (nv != v) {
        err = axp2101_write_reg(AXP2101_REG_GAUGE_CFG, nv);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "charge %s (0x18: 0x%02X -> 0x%02X)",
             enable ? "enabled" : "disabled (4.0V limit)", v, nv);
    return ESP_OK;
}

bool axp2101_is_charge_enabled(void)
{
    if (s_dev == NULL) {
        return true;
    }
    uint8_t v = 0;
    if (axp2101_read_reg(AXP2101_REG_GAUGE_CFG, &v) != ESP_OK) {
        return true;   /* 读不到时按"已使能"处理，避免误停充电 */
    }
    return (v & 0x02u) != 0;
}

esp_err_t axp2101_power_off(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 0x10[0] = 1：PMU 关掉除 RTCLDO 外全部电源轨。写之前调用方必须保证
       没有正在进行的 flash 写（NVS/pldata），否则会写坏数据。 */
    ESP_LOGW(TAG, "PMU soft power off (0x10[0]=1)");
    return axp2101_write_masked(AXP2101_REG_COMMON_CFG, 0x01, 0x01);
}

/* ------------------------------------------------------------------ */
/* 纯函数                                                              */
/* ------------------------------------------------------------------ */

uint16_t axp2101_icc_ma_from_code(uint8_t code)
{
    /* datasheet：N≤8 → 25×N mA；N>8 → 200+100×(N-8) mA。
       ⚠ 不是线性的：0x08=200mA、0x09=300mA，不能按 +25mA 递推。 */
    code &= 0x1Fu;
    if (code <= 8) {
        return (uint16_t)(25u * code);
    }
    if (code > 16) {
        code = 16;   /* 0x11 以上未定义，按上限 1000 mA 处理 */
    }
    return (uint16_t)(200u + 100u * (code - 8u));
}

uint8_t axp2101_icc_code_from_ma(uint16_t ma)
{
    /* 在合法编码 0~16 里找"电流最接近"的一档；并列时取较小档（更保守） */
    uint8_t best = 0;
    uint16_t best_diff = 0xFFFF;
    for (uint8_t code = 0; code <= 16; code++) {
        uint16_t cur = axp2101_icc_ma_from_code(code);
        uint16_t diff = (cur > ma) ? (uint16_t)(cur - ma) : (uint16_t)(ma - cur);
        if (diff < best_diff) {
            best_diff = diff;
            best = code;
        }
    }
    return best;
}

uint8_t axp2101_soc_from_mv(uint16_t mv)
{
    /* 600mAh 钴酸锂电芯的静置 OCV–SOC 参考表（reference_docs §6.4），
       降序查表 + 线性插值。⚠ 表是"静置"值，播放中带载会低 30~80mV，
       仅作 E-Gauge 不可信时的兜底。 */
    static const uint16_t ocv_mv[] = {
        4200, 4150, 4110, 4050, 3980, 3920, 3860, 3800,
        3740, 3680, 3630, 3570, 3490, 3420, 3300,
    };
    static const uint8_t ocv_soc[] = {
        100, 95, 90, 80, 70, 60, 50, 40,
        30, 20, 15, 10, 5, 3, 0,
    };
    const size_t n = sizeof(ocv_mv) / sizeof(ocv_mv[0]);

    if (mv >= ocv_mv[0]) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= ocv_mv[i]) {
            int dv = (int)ocv_mv[i - 1] - (int)ocv_mv[i];
            int ds = (int)ocv_soc[i - 1] - (int)ocv_soc[i];
            int soc = (int)ocv_soc[i] + (int)(mv - ocv_mv[i]) * ds / (dv > 0 ? dv : 1);
            if (soc < 0) {
                soc = 0;
            }
            if (soc > 100) {
                soc = 100;
            }
            return (uint8_t)soc;
        }
    }
    return 0;
}

bool axp2101_ts_mv_to_celsius(uint16_t ts_mv, int16_t *out_c)
{
    /* AXP2101 datasheet §6.7.4.4 Table 6-3（10kΩ NTC @25℃ + 50µA 恒流）：
       温度从 -20℃ 到 60℃，对应 TS 电压从 3150mV 降到 152mV。
       两列都按"温度升序"排列，查表后线性插值。 */
    static const uint16_t ts_mv_tab[] = {
        3150, 2508, 2013, 1628, 1325, 1084, 889, 732, 604,
        500, 416, 348, 292, 246, 209, 177, 152,
    };
    static const int16_t ts_c_tab[] = {
        -20, -15, -10, -5, 0, 5, 10, 15, 20,
        25, 30, 35, 40, 45, 50, 55, 60,
    };
    const size_t n = sizeof(ts_mv_tab) / sizeof(ts_mv_tab[0]);

    if (out_c == NULL) {
        return false;
    }
    if (ts_mv > ts_mv_tab[0] || ts_mv < ts_mv_tab[n - 1]) {
        return false;   /* 低于 -20℃ / 高于 60℃，或 TS 悬空/被拉死 */
    }

    for (size_t i = 0; i + 1 < n; i++) {
        if (ts_mv <= ts_mv_tab[i] && ts_mv >= ts_mv_tab[i + 1]) {
            int dv = (int)ts_mv_tab[i] - (int)ts_mv_tab[i + 1];
            int dt = (int)ts_c_tab[i + 1] - (int)ts_c_tab[i];
            int c = (int)ts_c_tab[i]
                    + ((int)ts_mv_tab[i] - (int)ts_mv) * dt / (dv > 0 ? dv : 1);
            *out_c = (int16_t)c;
            return true;
        }
    }
    return false;
}

const char *axp2101_charge_stage_name(axp2101_charge_stage_t stage)
{
    switch (stage) {
    case AXP2101_CHG_TRICKLE:      return "trickle";
    case AXP2101_CHG_PRECHARGE:    return "precharge";
    case AXP2101_CHG_CC:           return "CC";
    case AXP2101_CHG_CV:           return "CV";
    case AXP2101_CHG_DONE:         return "done";
    case AXP2101_CHG_NOT_CHARGING: return "not-charging";
    default:                       return "unknown";
    }
}

const char *axp2101_current_dir_name(axp2101_current_dir_t dir)
{
    switch (dir) {
    case AXP2101_CURRENT_STANDBY:     return "standby";
    case AXP2101_CURRENT_CHARGING:    return "charging";
    case AXP2101_CURRENT_DISCHARGING: return "discharging";
    default:                          return "reserved";
    }
}

/* 长按阈值档位 → 毫秒（日志/文档用，芯片内部按档位判定） */
static const uint16_t k_onlevel_ms[]  = { 128, 512, 1000, 2000 };
static const uint16_t k_offlevel_ms[] = { 4000, 6000, 8000, 10000 };
static const uint16_t k_irqlevel_ms[] = { 1000, 1500, 2000, 2500 };

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

bool axp2101_is_ready(void)
{
    return s_ready;
}

esp_err_t axp2101_init(i2c_master_bus_handle_t bus)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready (touch_init must run first)");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_irq_lock == NULL) {
        s_irq_lock = xSemaphoreCreateMutex();
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400 * 1000,   /* 必须 > 0，IDF 6.1 硬校验 */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", AXP2101_I2C_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    /* probe：只发地址看 ACK，确认芯片在位（比读寄存器安全，不改变任何状态） */
    err = i2c_master_probe(bus, AXP2101_I2C_ADDR, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not responding at 0x%02x (%s) — "
                      "battery info will be unavailable",
                 AXP2101_I2C_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* 读一次状态与开机原因（只读，便于确认"本次是按键开机还是插 USB 开机"） */
    uint8_t raw[2] = { 0 };
    if (axp2101_read_burst(AXP2101_REG_STATUS1, raw, sizeof(raw)) == ESP_OK) {
        ESP_LOGI(TAG, "status1=0x%02x (VBUS=%d BAT=%d BATFET=%d) status2=0x%02x",
                 raw[0], (raw[0] >> 5) & 1, (raw[0] >> 3) & 1, ((raw[0] >> 4) & 1) ^ 1,
                 raw[1]);
    }

    axp2101_boot_info_t boot = { 0 };
    if (axp2101_read_boot_info(&boot) == ESP_OK) {
        ESP_LOGI(TAG, "boot: PWRON status=0x%02x (key=%d vbus=%d battery=%d), "
                      "last power-off status=0x%02x (soft=%d long-press=%d)",
                 boot.pwron_status, boot.pwron_status & 1,
                 (boot.pwron_status >> 2) & 1, (boot.pwron_status >> 4) & 1,
                 boot.pwr_off_status, (boot.pwr_off_status >> 1) & 1,
                 boot.pwr_off_status & 1);
    }

    /* 清掉开机前积压的中断状态，否则 GPIO49 可能一直是低的 */
    axp2101_clear_irq_status();

#ifdef CONFIG_PMU_CHARGE_CONFIG
    /* 600 mAh / 3.7 V / 4.2 V 电芯整定：ICC 300mA(0.5C)、ITERM 50mA、
       CV 4.2V。⚠ 这是可选的写配置，默认关闭（CONFIG_PMU_CHARGE_CONFIG=n），
       首次上板应先只读确认电压/充电状态，再按需打开。 */
    err = axp2101_configure_charge(300, 0x02, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "configure charge failed: %s", esp_err_to_name(err));
    }
#endif

#ifdef CONFIG_PMU_KEY_IRQ
    err = axp2101_key_irq_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU key IRQ unavailable: %s", esp_err_to_name(err));
    }
#endif

    /* 当前长按阈值（只读日志；默认值来自 EFUSE，需实际上板确认，见参考文档 §17） */
    uint8_t key_level = 0;
    if (axp2101_read_reg(AXP2101_REG_KEY_LEVEL, &key_level) == ESP_OK) {
        uint8_t on = 0, off = 0, irq = 0;
        axp2101_decode_key_levels(key_level, &on, &off, &irq);
        ESP_LOGI(TAG, "key levels: ONLEVEL=%ums OFFLEVEL=%ums IRQLEVEL=%ums (0x27=0x%02x)",
                 k_onlevel_ms[on & 3], k_offlevel_ms[off & 3], k_irqlevel_ms[irq & 3],
                 key_level);
    }

    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 ready at 0x%02x (I2C0)", AXP2101_I2C_ADDR);
    return ESP_OK;
}
