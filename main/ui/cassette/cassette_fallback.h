#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_fallback.h
 * @brief 新旧 UI 切换的 NVS 记忆 + 开机 A+B 组合键应急回退（L1）
 *
 * ⚠ F16：P0 状态全部放 RAM，本模块只在**开机早期**写一次 ui_mode，
 *   且绝不触碰 LVGL 对象；避免 flash 关缓存期间 LVGL 读 PSRAM 触发 Core1 fault。
 */

typedef enum {
    CAS_UI_MODE_CASSETTE = 0,   /* 默认：新拟物磁带 UI */
    CAS_UI_MODE_LEGACY    = 1,   /* 回退：旧扁平四页 UI */
} cas_ui_mode_t;

/** 读当前 UI 模式（NVS 失败按 CASSETTE 处理） */
cas_ui_mode_t cassette_ui_mode_get(void);

/**
 * 写 UI 模式（NVS 一次性写入）。
 * 写完后当前进程不自动切界面：由调用方决定 esp_restart()（M6 隐藏项）等。
 */
void cassette_ui_mode_set(cas_ui_mode_t mode);

/**
 * 开机早期检测 A+B 同时按住约 3 秒；满 3 秒写 ui_mode=LEGACY。
 * ⚠ 仅当检测瞬间两键都按下才进入计时循环，否则立即返回（正常开机零延迟）。
 *   必须在 button_service_init() 之后调用（GPIO61 已配置为输入上拉）。
 *   不依赖按键回调，也不碰 LVGL。
 */
void cassette_boot_combo_check(void);

/** 当前是否应运行磁带 UI（boot_flow / key_handler 统一走这里） */
bool cassette_ui_is_active(void);

#ifdef __cplusplus
}
#endif
