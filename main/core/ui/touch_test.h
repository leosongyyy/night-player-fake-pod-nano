#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸区域测试：20px 白色圆点跟随手指移动，抬起后 1s 内渐变消失
 *
 * 需在 lvgl_port_init() 之后、持有 LVGL 锁时调用。
 * 应在所有界面之后创建，确保圆点绘制在屏幕最上层。
 */
void touch_test_create(void);

#ifdef __cplusplus
}
#endif
