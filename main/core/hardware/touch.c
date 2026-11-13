#include "core/hardware/touch.h"
#include "core/hardware/display.h"
#include "core/utils/task_psram.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "touch";

/* I2C 总线（IDF 6.1 新驱动 driver/i2c_master.h）
   ⚠ 2026-11 由旧驱动（driver/i2c.h，v6.0 起 EOL、计划 v7.0 移除）迁移而来。
   新驱动与旧驱动的关键差异：
     · 旧：端口号 I2C_NUM_0 + i2c_param_config/i2c_driver_install，读写时现填地址；
       新：先 i2c_new_master_bus() 建**总线句柄**，再为每个从设备
       i2c_master_bus_add_device() 建**设备句柄**（地址绑定在设备句柄上）。
     · 旧驱动的“超时”是 i2c_set_timeout() 配的硬件超时；新驱动按**每次传输**传
       xfer_timeout_ms，故本文件不再调用 i2c_set_timeout（旧调用值 0x02=2ms）。
     · esp_lcd 的 I2C 面板 IO 层在 v6.0 起**只支持新驱动**：
       esp_lcd_new_panel_io_i2c() 现在只接受 i2c_master_bus_handle_t
       （esp_lcd_i2c_bus_handle_t / _v1 / _v2 已从 IDF 中完全删除）——这正是
       本工程在 6.1 上“必然编译失败”的那一处。
   本总线句柄**对外公开**（touch_get_i2c_bus），因为 CS43131 DAC 与触摸共用
   同一条总线（见 cs43131.c，它把设备挂到这条总线上）。 */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

#if EXAMPLE_USE_TOUCH

// 根据TOUCH_IC_CONFIG的配置，选择调用的头文件
#if TOUCH_IC_CONFIG == CST816_ID
#include "esp_lcd_touch_cst816.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CST816_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_cst816

#elif TOUCH_IC_CONFIG == CST820_ID
#include "esp_lcd_touch_cst820.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_cst820

#elif TOUCH_IC_CONFIG == CHSC6417_ID
#include "esp_lcd_touch_chsc6417.h"
#define TOUCH_IO_I2C_CONFIG    ESP_LCD_TOUCH_IO_I2C_CHSC6417_CONFIG
#define esp_lcd_touch_new_i2c  esp_lcd_touch_new_i2c_chsc6417
#endif

static esp_lcd_touch_handle_t tp = NULL;
static TaskHandle_t s_touch_init_task = NULL;   /* 触摸异步初始化任务句柄 */
static volatile bool s_touch_ready = false;     /* 触摸初始化是否已完成 */

/* 触摸初始化完成回调（由初始化任务完成后调用，可经 touch_register_ready_cb 注册） */
static touch_ready_cb_t s_ready_cb = NULL;
static void *s_ready_ctx = NULL;

/* 触摸异步初始化任务：执行耗时的控制器完整初始化（含约 400ms 复位等待），
   不阻塞开机流程；完成后通知已注册的回调（如 LVGL 启用触摸输入）。 */
static void touch_init_task(void *arg)
{
    esp_lcd_panel_io_handle_t tp_io_handle = (esp_lcd_panel_io_handle_t)arg;

    const esp_lcd_touch_config_t tp_cfg = {
        /* S31 的 CST820 输出已经是 460x460 逻辑坐标（hardware_v1.md §4），
           直接用屏幕分辨率作为最大值；x_max/y_max 只影响镜像计算。 */
        .x_max = EXAMPLE_LCD_H_RES - 1,
        .y_max = EXAMPLE_LCD_V_RES - 1,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller (background)");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c(tp_io_handle, &tp_cfg, &tp));
    s_touch_ready = true;

    ESP_LOGI(TAG, "Touch controller ready");
    if (s_ready_cb != NULL) {
        s_ready_cb(tp, s_ready_ctx);
    }
    vTaskDelete(NULL);
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    /* 新驱动自带探测：i2c_master_probe() 内部完成“发地址 + 看 ACK”，
       等价于旧版手搓 i2c_cmd_link + start/write_byte/stop/cmd_begin 那一套。

       ⚠ 只扫 0x01~0x77：0x78~0x7F 是 I2C 规范**保留**的地址段
       （1111_0xx 为 10-bit 地址前缀，1111_1xx 保留给未来协议，0x7E 还是 I3C 广播地址），
       合法从设备不允许占用。本板实测每次开机都会在 0x7E 打出一个 ACK ——
       那不是第 6 颗芯片，而是总线上某颗已有芯片（最可能是 QMI8658 IMU）对保留地址的
       假应答：只有地址字节被 ACK，数据阶段连写一个寄存器地址都被 NACK / 读会直接把
       硬件 I2C 外设卡到超时（需清总线才能恢复）。调查过程与实测数据见
       ideas_and_exps/I2C_0X7E_INVESTIGATION.md。把它扫进日志只会误导"多了一颗设备"。 */
    for (uint8_t addr = 1; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan completed.");
}

i2c_master_bus_handle_t touch_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_lcd_touch_handle_t touch_init(void)
{
    ESP_LOGI(TAG, "Initialize I2C bus (i2c_master)");

    /* 新驱动的总线配置（对应旧版 i2c_config_t 的 master 部分）。
       clk_source 用默认（I2C_CLK_SRC_DEFAULT），glitch_ignore_cnt=7 为 IDF 示例推荐值，
       flags.enable_internal_pullup 打开 IC 内部上拉 —— 与旧配置的
       sda_pullup_en/scl_pullup_en = GPIO_PULLUP_ENABLE 语义一致（板载另有外部上拉）。 */
    const i2c_master_bus_config_t bus_conf = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    // 400kHz：S31 板上 CST820 直接挂在 I2C0（无 TXS0102 电平转换），
    // 按硬件参考文档的 400kHz 参考值使用。
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_conf, &s_i2c_bus));

    // Scan I2C devices
    i2c_scan();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    /* ⚠⚠ IDF 6.1 新增字段 scl_speed_hz，必须显式补上（2026-11 迁移的第二个坑）：
       6.1 的 esp_lcd_new_panel_io_i2c() 内部把它直接转交给新 I2C 驱动的
       i2c_master_bus_add_device()，而后者有硬校验
           ESP_RETURN_ON_FALSE(dev_config->scl_speed_hz > 0, … "invalid scl frequency")
       本地触摸驱动的 ESP_LCD_TOUCH_IO_I2C_*_CONFIG() 宏是 5.x 时代写的，**没有这个
       字段**，宏展开后该成员为 0 → add_device 直接返回 ESP_ERR_INVALID_ARG →
       本行的 ESP_ERROR_CHECK 触发 abort → **开机即 panic 重启循环**。
       实测报错（迁移期真机日志）：
           E i2c.master: i2c_master_bus_add_device(1181): invalid scl frequency
           E lcd_panel.io.i2c: esp_lcd_new_panel_io_i2c(72): i2c add device fail
           ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG) at touch.c
       ⚠ 注意这个值**不是"可省略"的默认**：新 I2C 驱动把它当作该设备每次传输时
       要切换到的 SCL 频率，所以必须与总线同速（都用 200kHz）。
       这里在调用点覆盖而不是改 3 个驱动头文件里的宏：位置集中、不影响上游宏文件。 */
    esp_lcd_panel_io_i2c_config_t tp_io_config = TOUCH_IO_I2C_CONFIG();
    tp_io_config.scl_speed_hz = 400 * 1000;   // 与 i2c_new_master_bus 的总线速率一致
    // Attach the TOUCH to the I2C bus（v6.1 起只接受 i2c_master_bus_handle_t）
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle));

    // 异步初始化触摸控制器：控制器完整初始化（含约 400ms 复位等待）放到
    // 后台任务执行，不阻塞后续硬件/UI 初始化。就绪后调用 ready 回调
    //（上层注册，如把句柄交给 LVGL），在此之前触摸回调不给数据。
    // ⚠ 栈放 PSRAM：一次性任务，只做 I2C 初始化 + 回调，无实时性要求
    //（见 core/utils/task_psram.h），省下 4KB 内部 RAM
    BaseType_t ret = task_create_psram_stack(touch_init_task, "touch_init",
                                             4096, tp_io_handle, 5,
                                             &s_touch_init_task,
                                             tskNO_AFFINITY);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch init task");
        return NULL;
    }

    // 初始化进行中：返回 NULL，LVGL 触摸输入设备在就绪前不提供数据
    return NULL;
}

void touch_register_ready_cb(touch_ready_cb_t cb, void *ctx)
{
    s_ready_cb = cb;
    s_ready_ctx = ctx;
    /* 若触摸已完成初始化（极端情况：回调注册晚于任务完成），立即补发一次 */
    if (s_touch_ready && cb != NULL) {
        cb(tp, ctx);
    }
}

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return tp;
}

void touch_power_off(void)
{
    /* 若触摸初始化任务仍在执行（开机极早期触发 deep sleep），先等它完成，
       避免 i2c_driver_delete 与任务内的 I2C 复位/读 ID 操作冲突 */
    if (s_touch_init_task != NULL && !s_touch_ready) {
        ESP_LOGI(TAG, "touch init task still running, waiting...");
        while (!s_touch_ready) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* 触摸控制器复位：RST 拉低进入复位低功耗 */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(EXAMPLE_PIN_NUM_TOUCH_RST, 0);

    /* 释放 I2C 总线（CS43131 已关机、触摸已复位，总线不再使用；
       引脚回到高阻输入，外部上拉无电流路径，deepsleep 中不额外耗电。
       唤醒后 touch_init() 会重新建总线与触摸控制器）。
       ⚠ 新驱动没有“只卸驱动、保留端口”这种半拆状态：i2c_del_master_bus() 会
       连同挂在总线上的所有设备句柄一起销毁，正好等价于旧版 i2c_driver_delete()。
       调用方须先让 CS43131 停用（见 fake_shutdown.c：cs43131_reset_for_sleep()
       在前、touch_power_off() 在后）。 */
    if (s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    ESP_LOGI(TAG, "touch powered off (RST low, I2C bus released)");
}

#else // !EXAMPLE_USE_TOUCH

esp_lcd_touch_handle_t touch_init(void)
{
    return NULL;
}

void touch_register_ready_cb(touch_ready_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
}

esp_lcd_touch_handle_t touch_get_handle(void)
{
    return NULL;
}

#endif // EXAMPLE_USE_TOUCH
