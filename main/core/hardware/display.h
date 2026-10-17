#pragma once

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qspi_amoled.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AMOLED LCD 硬件抽象（QSPI 接口，鱼鹰光电屏）
 *
 * 本模块负责：
 *  - LCD 引脚定义
 *  - 屏幕尺寸选择（CURRENT_SCREEN_SIZE）
 *  - SPI 总线 / 面板 IO / 面板驱动初始化
 */

#define LCD_HOST    SPI2_HOST

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL       (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL       (16)
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

// 鱼鹰光电屏幕验证底板
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_8)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_7)

#define CST816_ID   1
#define CST820_ID   2
#define CHSC6417_ID 3

// 鱼鹰光电
#define AM196Q410502LK_196_410x502   1
#define AM178Q368448LK_178_368x448   2
#define AM151Q466466LK_151_466x466_C 3
#define AM160Q480480LK_160_480x480_C 4
#define AM201Q240296LK_201_240x296   5
#define AM200Q460460LK_200_460x460   6  //本项目使用这个

// 设置当前屏幕尺寸
#define CURRENT_SCREEN_SIZE AM200Q460460LK_200_460x460 // 在这里选择屏幕尺寸

#if CURRENT_SCREEN_SIZE == AM196Q410502LK_196_410x502
  #define EXAMPLE_LCD_H_RES              410
  #define EXAMPLE_LCD_V_RES              502
  #define EXAMPLE_LCD_X_GAP              22
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CST820_ID
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#elif CURRENT_SCREEN_SIZE == AM178Q368448LK_178_368x448
  #define EXAMPLE_LCD_H_RES              368
  #define EXAMPLE_LCD_V_RES              448
  #define EXAMPLE_LCD_X_GAP              16
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CHSC6417_ID
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#elif CURRENT_SCREEN_SIZE == AM151Q466466LK_151_466x466_C
  #define EXAMPLE_LCD_H_RES              466
  #define EXAMPLE_LCD_V_RES              466
  #define EXAMPLE_LCD_X_GAP              6
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CST820_ID
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#elif CURRENT_SCREEN_SIZE == AM160Q480480LK_160_480x480_C
  #define EXAMPLE_LCD_H_RES              480
  #define EXAMPLE_LCD_V_RES              480
  #define EXAMPLE_LCD_X_GAP              0
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CHSC6417_ID
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#elif CURRENT_SCREEN_SIZE == AM201Q240296LK_201_240x296
  #define EXAMPLE_LCD_H_RES              240
  #define EXAMPLE_LCD_V_RES              296
  #define EXAMPLE_LCD_X_GAP              0
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CST816_ID
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#elif CURRENT_SCREEN_SIZE == AM200Q460460LK_200_460x460
  #define EXAMPLE_LCD_H_RES              460
  #define EXAMPLE_LCD_V_RES              460
  #define EXAMPLE_LCD_X_GAP              10
  #define EXAMPLE_LCD_Y_GAP              0
  #define TOUCH_IC_CONFIG                CST820_ID
  /* QSPI 像素时钟。**这是全局刷屏带宽的硬上限，直接决定能跑多少帧**：
     4 条数据线 × pclk，40 MHz ≈ 16 MB/s；一帧全屏 = 460×460×2B = 423 KB，
     列表滚动时每帧都要重发整个视口（约 340 KB），40 MHz 下光传输就要 21 ms
     以上，帧间隔被压在 45~60 ms（约 20 fps）——这是本机当前的带宽天花板。
     ⚠ **2026-11 实测：80 MHz 在这块屏上会留下"偶发 1px 黑竖线"残留**
       （表现为被局部重绘的区域左边缘多出一条 1px 黑线，对象变宽后才被清掉；
       连 LVGL 自带的 FPS 监视器、进度时长标签都有，与小尺寸局部刷新强相关，
       说明是传输时序边际而非绘制逻辑）。同一固件回到 40 MHz 即完全消失，
       故**保持 40 MHz**，不要为几毫秒 render 去冒这个风险。
     ⚠ 只在这块屏的排线过长 / 杜邦线飞线时才需要再降（症状是花屏/噪点）。 */
  #define AMOLED_QSPI_MAX_PCLK           40 * 1000 * 1000
#else
  #error "Unsupported screen size"
#endif

/* 开机默认亮度（最亮）。
   开机流程：display_init() 以亮度 0（全黑）初始化，避免面板 RAM 未初始化时
   闪白屏（init 序列中 0x51 在 DISPON 前为 0x00）；LVGL 首帧渲染完成后由
   app_main 调用 display_set_brightness() 恢复该值。
   ⚠ display_set_brightness() 必须在持有 lvgl_port_lock() 时调用：亮度命令
   与 LVGL 刷屏共用 SPI 总线，锁外并发执行会冲突（曾致画面定格/花屏）。 */
#define DISPLAY_BRIGHTNESS_DEFAULT (0xFF)

/**
 * @brief 初始化 AMOLED 显示屏（SPI 总线 + 面板 IO + 面板驱动）
 *
 * @return esp_lcd_panel_handle_t 面板句柄，供 LVGL 移植层使用
 */
esp_lcd_panel_handle_t display_init(void);

/**
 * @brief 设置屏幕亮度
 *
 * @param brightness 亮度值 0x00~0xFF（0 最暗，255 最亮）
 * @return esp_err_t
 */
esp_err_t display_set_brightness(uint8_t brightness);

/**
 * @brief 获取当前屏幕亮度
 *
 * @return 当前亮度值（0x00~0xFF）
 */
uint8_t display_get_brightness(void);

/**
 * @brief 息屏：AMOLED 进入低功耗休眠（DISPOFF + SLEEP IN，可逆）
 *
 * 先发 0x28 关显示（像素熄灭），再发 0x10 SLEEP IN：驱动 IC 停止 DC/DC、
 * 振荡器与扫描（显示 RAM 保留），这才是 AMOLED 真正的低功耗态——仅亮度 0
 * 或仅 DISPOFF 时驱动 IC 仍满负荷运行（实测息屏电流大头）。
 * 与 display_power_off() 命令序列相同，但语义为"可逆息屏"，由
 * display_sleep_exit() 恢复；深度休眠（deepsleep）场景仍用 display_power_off
 * （唤醒即重启，走 display_init 全初始化）。
 *
 * ⚠ 命令与 LVGL 刷屏共用 SPI 总线，必须在持有 lvgl_port_lock() 时调用。
 * 幂等：已休眠时重复调用直接返回。
 */
void display_sleep_enter(void);

/**
 * @brief 亮屏：退出 AMOLED 休眠并恢复显示（可逆）
 *
 * 实测该面板 SLEEP OUT 后仅发 0x11 + DISPON **无法恢复显示**（屏黑），必须
 * 重新初始化：先 0x11 唤醒并等 120ms（规格书恢复时间，与 display_init 中
 * 0x11 后等待同规格，不可省），再 esp_lcd_panel_init() 重发完整初始化
 * （MADCTL/COLMOD + display_init 的 lcd_init_cmds，含 0x51=0 全黑、0x29
 * DISPON；不含硬件 reset）。重初始化会重置显示 RAM → 调用方须随后全屏重刷
 * 并按用户配置恢复亮度（见 power_service 亮屏：invalidate + lv_refr_now +
 * display_set_brightness，同开机"全黑启动防白屏"顺序）。
 *
 * ⚠ 必须在持有 lvgl_port_lock() 时调用；须在 LVGL 任务恢复（resume）之前
 * 完成——休眠中/刚唤醒的面板不能接收刷屏数据。
 * 幂等：未休眠时重复调用直接返回。
 */
void display_sleep_exit(void);

/**
 * @brief 关闭显示屏（深度休眠前调用，等效 display_sleep_enter）
 *
 * 发送 0x28 关显示 + 0x10 SLEEP IN（驱动 IC 停 DC/DC/振荡器，内存保留）。
 * 唤醒/重启后由 display_init() 重新初始化恢复。
 */
void display_power_off(void);

#ifdef __cplusplus
}
#endif
