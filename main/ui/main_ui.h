#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主界面（当前为占位：仅一个居中的 Hello World 标签）
 *
 * 播放列表 / 播放控制界面开发中，后续在此替换为真正的播放界面。
 * 需在持有 LVGL 锁时调用。
 */
void main_ui_create(void);

#ifdef __cplusplus
}
#endif
