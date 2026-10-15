#include "core/hardware/display.h"
#include "core/ui/lvgl_port.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint8_t s_brightness = 0xFF;  // 当前亮度，默认最亮
static bool s_display_asleep = false; /* 面板是否处于 SLEEP IN（息屏）状态 */

#if CURRENT_SCREEN_SIZE == AM196Q410502LK_196_410x502
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},// 退出睡眠模式
    {0x35, (uint8_t []){0x00}, 1, 0},// 开启撕裂效果
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},// SPI 模式控制
    // {0x36, (uint8_t []){0x00}, 1, 0},// 设置内存数据访问控制
    {0x3A, (uint8_t []){0x55}, 1, 0},//// 设置像素格式 16位
    {0x53, (uint8_t []){0x20}, 1, 0},// 设置 CTRL 显示1
    {0x63, (uint8_t []){0xFF}, 1, 0},// 设置 HBM 模式下的亮度值
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},// 打开显示器前先置亮度 0（全黑启动，避免 RAM 未初始化时闪白屏）
    {0x29, (uint8_t []){0x00}, 0, 60},// 打开显示器
    {0x58, (uint8_t []){0x07}, 1, 10},// 设置正常模式下的亮度值
  };
#elif CURRENT_SCREEN_SIZE == AM178Q368448LK_178_368x448
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},// 退出睡眠模式
    {0x35, (uint8_t []){0x00}, 1, 0},// 开启撕裂效果
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},// SPI 模式控制
    // {0x36, (uint8_t []){0x00}, 1, 0},// 设置内存数据访问控制
    {0x3A, (uint8_t []){0x55}, 1, 0},//// 设置像素格式 16位
    {0x53, (uint8_t []){0x20}, 1, 0},// 设置 CTRL 显示1
    {0x63, (uint8_t []){0xFF}, 1, 0},// 设置 HBM 模式下的亮度值
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},// 打开显示器前先置亮度 0（全黑启动，避免 RAM 未初始化时闪白屏）
    {0x29, (uint8_t []){0x00}, 0, 60},// 打开显示器
    {0x58, (uint8_t []){0x07}, 1, 10},// 设置正常模式下的亮度值
  };
#elif CURRENT_SCREEN_SIZE == AM151Q466466LK_151_466x466_C
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},// 退出睡眠模式
    {0x35, (uint8_t []){0x00}, 1, 0},// 开启撕裂效果
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},// SPI 模式控制
    // {0x36, (uint8_t []){0x00}, 1, 0},// 设置内存数据访问控制
    {0x3A, (uint8_t []){0x55}, 1, 0},//// 设置像素格式 16位
    {0x53, (uint8_t []){0x20}, 1, 0},// 设置 CTRL 显示1
    {0x63, (uint8_t []){0xFF}, 1, 0},// 设置 HBM 模式下的亮度值
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},// 亮度先置 0（全黑启动，避免 RAM 未初始化时闪白屏）
    {0x58, (uint8_t []){0x07}, 1, 10},// 设置正常模式下的亮度值
  };
#elif CURRENT_SCREEN_SIZE == AM160Q480480LK_160_480x480_C
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x50}, 1, 0},
    {0xB1, (uint8_t []){0x78,0x70}, 2, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},// 亮度先置 0（全黑启动，避免 RAM 未初始化时闪白屏）
    {0x63, (uint8_t []){0xFF}, 1, 0},
    {0x64, (uint8_t []){0x10}, 1, 0},
    {0x67, (uint8_t []){0x01}, 1, 0},
    {0x68, (uint8_t []){0x31}, 1, 0},
    {0x2A, (uint8_t []){0x00,0x00,0x01,0xdf}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xdf}, 4, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 120},
  };
#elif CURRENT_SCREEN_SIZE == AM201Q240296LK_201_240x296
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
      {0xFE, (uint8_t []){0x00}, 1, 0},
      {0xC4, (uint8_t []){0x80}, 1, 0},
      {0x35, (uint8_t []){0x00}, 1, 0},
      {0x3A, (uint8_t []){0x55}, 1, 0},
      {0x53, (uint8_t []){0x20}, 1, 0},
      {0x51, (uint8_t []){0x00}, 1, 0},// 亮度先置 0（全黑启动，避免 RAM 未初始化时闪白屏）
      {0x63, (uint8_t []){0xFF}, 1, 0},
      {0x2A, (uint8_t []){0x00,0x00,0x00,0xEF}, 4, 0},
      {0x2B, (uint8_t []){0x00,0x00,0x01,0x27}, 4, 0},
      {0x11, (uint8_t []){0x00}, 0, 80},
      {0x29, (uint8_t []){0x00}, 0, 10},
  };
#elif CURRENT_SCREEN_SIZE == AM200Q460460LK_200_460x460
  static const qspi_amoled_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},// 退出睡眠模式（规格书要求的恢复时间，不可省）
    {0x35, (uint8_t []){0x00}, 1, 0},// 开启撕裂效果
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},// SPI 模式控制
    // 0x36/0x3A 无需重复发送：组件内部已发 MADCTL(0x36) 与 COLMOD(0x3A=0x55)，
    // 再发只会触发 "3Ah command overwritten" 警告
    {0x53, (uint8_t []){0x20}, 1, 0},// 设置 CTRL 显示1
    {0x63, (uint8_t []){0xFF}, 1, 0},// 设置 HBM 模式下的亮度值
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},// 打开显示器前先置亮度 0（全黑启动，避免 RAM 未初始化时闪白屏）
    {0x29, (uint8_t []){0x00}, 0, 0},// 打开显示器（DISPON 本身无等待要求，加快开机）
    {0x58, (uint8_t []){0x07}, 1, 10},// 设置正常模式下的亮度值
  };
#else
  #error "Unsupported screen size"
#endif

esp_lcd_panel_handle_t display_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = QSPI_AMOLED_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                                 EXAMPLE_PIN_NUM_LCD_DATA0,
                                                                 EXAMPLE_PIN_NUM_LCD_DATA1,
                                                                 EXAMPLE_PIN_NUM_LCD_DATA2,
                                                                 EXAMPLE_PIN_NUM_LCD_DATA3,
                                                                 EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    // flush 完成回调与 LVGL 显示驱动相关，由 LVGL 移植层提供
    esp_lcd_panel_io_spi_config_t io_config = QSPI_AMOLED_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                                lvgl_port_notify_flush_ready,
                                                                                lvgl_port_get_flush_ctx());

    io_config.pclk_hz = (unsigned int)AMOLED_QSPI_MAX_PCLK;
    qspi_amoled_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_LOGI(TAG, "Install QSPI_AMOLED panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_qspi_amoled(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, EXAMPLE_LCD_X_GAP, EXAMPLE_LCD_Y_GAP));
    // 打开显示器：初始化序列已将亮度置 0（全黑），此时面板 RAM 尚未被 LVGL
    // 绘制，若以默认亮度点亮会先闪出未初始化内容（白屏）。UI 创建完成后由
    // app_main 在持有 LVGL 锁时调用 display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT)
    // 恢复亮度（亮度命令与 LVGL 刷屏共用 SPI 总线，锁外并发会冲突，见 display.h）。
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    // 记录面板句柄，并同步亮度状态为 0（全黑启动，防白屏）
    s_panel_handle = panel_handle;
    ESP_ERROR_CHECK(display_set_brightness(0));

    return panel_handle;
}

esp_err_t display_set_brightness(uint8_t brightness)
{
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_brightness = brightness;
    return panel_qspi_amoled_set_brightness(s_panel_handle, brightness);
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

void display_sleep_enter(void)
{
    if (s_panel_handle == NULL || s_display_asleep) {
        return;
    }
    /* 先 0x28 关显示（像素熄灭），再 0x10 SLEEP IN（驱动 IC 停止 DC/DC、
       振荡器与扫描，显示 RAM 保留）——SLEEP IN 才是 AMOLED 真正的低功耗态，
       仅 DISPOFF 时驱动 IC 仍满负荷运行（实测息屏电流大头）。
       唤醒用 display_sleep_exit（SLEEP OUT 恢复，无需重走初始化）。 */
    esp_lcd_panel_disp_on_off(s_panel_handle, false);
    esp_lcd_panel_disp_sleep(s_panel_handle, true);
    s_display_asleep = true;
    ESP_LOGI(TAG, "display sleep in (screen off)");
}

void display_sleep_exit(void)
{
    if (s_panel_handle == NULL || !s_display_asleep) {
        return;
    }
    /* 实测：该面板 SLEEP OUT 后仅发 0x11 + 120ms + 0x29 无法恢复显示（屏黑），
       必须重新初始化（组件注释亦注明"退出睡眠后需重新初始化"）：
       1. 先 0x11 唤醒并等规格书要求的 120ms 恢复时间——SLEEP IN 状态下
          扩展命令可能无效，0x11 是 MIPI 标准命令，保证能唤醒；
       2. 再 esp_lcd_panel_init() 重发完整 init（MADCTL/COLMOD + display_init
          的 lcd_init_cmds，含 0x51=0 全黑、0x29 DISPON；不含硬件 reset）。
          面板 RAM 随之重置为黑——调用方须随后全屏重刷（power_service 亮屏
          时 lv_obj_invalidate + lv_refr_now），再按用户配置恢复亮度
          （与开机"全黑启动防白屏"的点亮顺序一致）。 */
    esp_lcd_panel_disp_sleep(s_panel_handle, false);   /* 0x11 SLEEP OUT */
    vTaskDelay(pdMS_TO_TICKS(120));                    /* 规格书恢复时间，不可省 */
    esp_err_t err = esp_lcd_panel_init(s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel re-init after sleep out failed: %s",
                 esp_err_to_name(err));
    }
    s_display_asleep = false;
    ESP_LOGI(TAG, "display awake (sleep out + re-init)");
}

void display_power_off(void)
{
    if (s_panel_handle == NULL) {
        return;
    }
    /* 深度休眠前的最终断电：命令序列与息屏相同（DISPOFF + SLEEP IN）；
       唤醒即系统重启，display_init() 会重新走完整初始化，无需恢复 */
    display_sleep_enter();
    ESP_LOGI(TAG, "display powered off (DISPOFF + SLEEP IN)");
}
