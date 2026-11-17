#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "core/hardware/button_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_nav.h
 * @brief 7 页注册表 + 轻量页管理 + A/B 键路由
 *
 * 不用 lv_tabview（横滑手势与 P0 点击语义耦合）、不用 lv_scr_load（工程无
 * screen manager）。切页 = 目标页 clear HIDDEN / 其余 add HIDDEN +
 * lv_obj_move_foreground()，全程在 pages 容器内，无全屏重绘抖动。
 *
 * ⚠ `cas_nav_on_key()` 内部会操作 LVGL 对象，调用方必须在 LVGL 任务上下文
 *   或持有 lvgl_port_lock()（key_handler.c 已按此约定加锁）。
 */

typedef enum {
    PAGE_PLAYER = 0,
    PAGE_QUICK,
    PAGE_WEATHER,
    PAGE_CLOCK,
    PAGE_LIST,
    PAGE_SETTINGS,
    PAGE_FILES,
    PAGE_COUNT,
} cas_page_id_t;

typedef void (*cas_create_fn)(lv_obj_t *parent);
typedef void (*cas_refresh_fn)(void);

typedef struct {
    cas_page_id_t id;
    const char   *title;      /* 状态栏标题（Web 各屏 .st 文案） */
    const char   *short_title;/* 抽屉导航按钮短名（Web 顶栏 nav 文案） */
    lv_obj_t     *root;       /* 当前常驻页根容器 */
    bool          created;
    cas_create_fn create;     /* T03~T05 模块填充；NULL = 空页占位 */
    cas_refresh_fn refresh;
} cas_page_t;

/** 在 pages 容器下创建 7 个全屏页根（首次调用幂等） */
void cas_nav_init(lv_obj_t *pages_container);

/** 切到指定页（循环保护 + 状态栏标题同步 + 抽屉高亮同步） */
void cas_nav_show(cas_page_id_t id);

/** 下一页 / 上一页（7 页循环） */
void cas_nav_next(void);
void cas_nav_prev(void);

/**
 * A/B 键路由：仅磁带 UI 且亮屏时消费。
 *   UP_SHORT   -> 下一模块
 *   DOWN_SHORT -> 上一模块
 *   DOWN_LONG  -> 开/合抽屉
 *   UP_LONG / BOTH / 息屏事件 -> 返回 false（保留原有安全语义）
 * @return true = 已消费，key_handler 应立即 return
 */
bool cas_nav_on_key(button_event_t ev);

/** 当前页 / 标题 / 页根（供抽屉高亮与 T03 集成使用） */
cas_page_id_t cas_nav_active(void);
const char *cas_nav_title(cas_page_id_t id);
const char *cas_nav_short_title(cas_page_id_t id);
lv_obj_t *cas_nav_page_root(cas_page_id_t id);

#ifdef __cplusplus
}
#endif
