/**
 * @file lvgl_mem.h
 * @brief LVGL 9 自定义内存分配器（core/ui/lvgl_mem.c）的对外接口
 *
 * 目前只暴露一个东西：**"热点对象优先内部 RAM"的窗口开关**。
 *
 * ⚠ 该开关的实现受 Kconfig `CONFIG_UI_LVGL_PIN_HOT_OBJECTS` 控制，**2026-11 起
 *   默认关闭**（S31 要把内部 RAM 留给 WiFi/BT）：关闭时本函数是空操作，调用点
 *   保持原样即可，所有 LVGL 对象（含滚动列表页）都分配在 PSRAM。
 *   实测对比见 ideas_and_exps/IRAM_OBJECTS_PSRAM_S31.md。
 */

#ifndef CORE_UI_LVGL_MEM_H
#define CORE_UI_LVGL_MEM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 【路径B】进入/离开"热点对象"窗口：窗口内 LVGL 的所有分配优先落内部 RAM。
 *
 * 为什么需要它（实测数据见 ideas_and_exps/IRAM_AUDIT_LVGL9.md §12）：
 *   LVGL 对象放内部 RAM 时列表快速滚动 21fps，放 PSRAM 只有 11fps —— 这是比
 *   "blend 是否进 IRAM"（11 vs 9 fps）大得多的性能杠杆，因为滚动的开销主要在
 *   对象树的遍历/布局/样式解析，而对象工作集约 88KB，远超 ESP32-S3 的 32KB
 *   数据 cache。
 *   但**全部**对象放进内部 RAM 装不下（§11 实测：会直接见底、SD 读失败、
 *   判坏曲跳曲、newlib 建锁 abort），所以只能"按创建时机圈定"：
 *   只在创建滚动热对象（virtual_list 的行槽）的那一小段时间打开本开关。
 *
 * 用法（必须配对，且不要跨任务/跨锁）：
 *     lvgl_mem_set_internal_prefer(true);
 *     ... 创建热对象 ...
 *     lvgl_mem_set_internal_prefer(false);
 *
 * 安全阀在 lvgl_mem.c 内部：窗口内若内部 RAM 空闲低于保留底线（40KB），
 * 分配会自动回退 PSRAM —— 功能不受影响，只是那部分对象不享受加速。
 *
 * ⚠ 未启用 CONFIG_LV_USE_CUSTOM_MALLOC（LVGL 用自带分配器）时本函数是空操作。
 * ⚠ 未启用 CONFIG_UI_LVGL_PIN_HOT_OBJECTS（2026-11 起的默认）时本函数也是空操作，
 *   上面的 21fps / 11fps 是 ESP32-S3 时代的数据，S31 的复测见
 *   ideas_and_exps/IRAM_OBJECTS_PSRAM_S31.md。
 */
void lvgl_mem_set_internal_prefer(bool on);

#ifdef __cplusplus
}
#endif

#endif /* CORE_UI_LVGL_MEM_H */
