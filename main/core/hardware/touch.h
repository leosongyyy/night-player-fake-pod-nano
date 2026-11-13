#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸控制器硬件抽象（I2C 接口）
 *
 * 根据 display.h 中 TOUCH_IC_CONFIG 自动选择：
 *   CST816 / CST820 / CHSC6417
 */

#define EXAMPLE_USE_TOUCH               1

/* ESP32-S31 / FakePodNano Pro V1 触摸（CST820）：
   I2C0 与 AXP2101 / PCF85063 / QMI8658 共用 SDA48/SCL47，
   INT=45（低有效）、RST=46（低有效）。见 hardware_v1.md §3/§4。 */
#define EXAMPLE_PIN_NUM_TOUCH_SCL         (GPIO_NUM_47)
#define EXAMPLE_PIN_NUM_TOUCH_SDA         (GPIO_NUM_48)
#define EXAMPLE_PIN_NUM_TOUCH_RST         (GPIO_NUM_46)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (GPIO_NUM_45)

/**
 * @brief 初始化 I2C 总线并启动触摸控制器（异步）
 *
 * I2C 总线 / 扫描 / 面板 IO 同步完成（约 20ms）；控制器完整初始化
 * （含约 400ms 复位等待）放到后台任务，函数立即返回。
 *
 * @return esp_lcd_touch_handle_t 触摸句柄；初始化进行中返回 NULL，
 *         就绪后经 touch_register_ready_cb 注册的回调获取
 */
esp_lcd_touch_handle_t touch_init(void);

/**
 * @brief 触摸初始化完成回调类型
 *
 * 触摸控制器完整初始化（含约 400ms 复位等待）在后台任务中异步执行，
 * 完成后在触摸任务上下文中调用该回调，把就绪的句柄交给上层（如 LVGL）。
 *
 * @param tp     初始化完成的触摸句柄
 * @param ctx    注册时传入的用户上下文
 */
typedef void (*touch_ready_cb_t)(esp_lcd_touch_handle_t tp, void *ctx);

/**
 * @brief 注册触摸初始化完成回调（异步初始化完成后调用）
 *
 * @param cb  回调；传 NULL 取消注册
 * @param ctx 透传给回调的用户上下文
 */
void touch_register_ready_cb(touch_ready_cb_t cb, void *ctx);

/**
 * @brief 获取触摸句柄（供 UI 轮询使用）
 */
esp_lcd_touch_handle_t touch_get_handle(void);

/**
 * @brief 获取触摸所在 I2C 总线句柄（CS43131 DAC 与触摸共用同一条总线）
 *
 * IDF 6.1 起 I2C 是「总线句柄 + 每设备句柄」模型：touch_init() 用
 * i2c_new_master_bus() 建立总线后，其它从设备（CS43131，见
 * core/hardware/audio/cs43131.c）必须用本句柄 i2c_master_bus_add_device()
 * 把自己挂上去，而不能再像旧驱动那样“按端口号现填地址”。
 *
 * @return 总线句柄；touch_init() 之前或 touch_power_off() 之后为 NULL
 */
i2c_master_bus_handle_t touch_get_i2c_bus(void);

/**
 * @brief 关闭触摸控制器并释放 I2C 总线（供深度休眠前调用）
 *
 * 触摸 RST 拉低进入复位低功耗，随后 i2c_del_master_bus() 释放整条 I2C_NUM_0
 * 总线（含挂在总线上的全部设备句柄，引脚回到高阻，外部上拉无电流路径）。
 * 须在 CS43131 关机后调用（总线已不再使用）；唤醒后 touch_init() 会重新
 * 建总线与触摸控制器。
 */
void touch_power_off(void);

#ifdef __cplusplus
}
#endif
