/*******************************************************************************
 * ftoption_lvgl.h
 * 覆盖 FreeType 的 ftoption.h —— **只为把光栅化器的渲染池从 16KB 压到 4KB**。
 *
 * 怎么生效：FreeType 的 ftheader.h 里有官方定制口子
 *     #ifndef FT_CONFIG_OPTIONS_H
 *     #define FT_CONFIG_OPTIONS_H  <freetype/config/ftoption.h>
 *     #endif
 * main/CMakeLists.txt 给 freetype 目标注入
 *     FT_CONFIG_OPTIONS_H=<本文件的绝对路径>
 * 于是 FreeType 先包含本文件；本文件先把组件自带（构建时生成）的那份原始配置
 * 整个包含进来，再改掉渲染池大小 —— 其它设置（zlib、整数类型、模块裁剪等）
 * 全部保持组件生成的原样，不会漂移。
 *
 * ============================ 为什么要改它 ============================
 * FreeType 的平滑光栅化器把渲染池当**栈上局部变量**用：
 *     smooth/ftgrays.c:  TCell buffer[FT_MAX_GRAY_POOL];
 *                        FT_MAX_GRAY_POOL = FT_RENDER_POOL_SIZE / sizeof(TCell)
 * 默认 FT_RENDER_POOL_SIZE = 16384，即**单次 FT_Render_Glyph 就要 16KB 栈**。
 * 任何"可能光栅化字形"的任务都得有这么多额外栈，而本板内部 RAM 紧张
 * （开机完成只剩 ~30KB），不可能给每个渲染任务都加 16KB 栈。
 *
 * 2026-10 TTF 迁移时的实测事故（定位过程见 ideas_and_exps/FontSystem.md）：
 *   主任务栈 CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192，开机在 app_main 里
 *   持锁渲染首帧 → LVGL 绘制链 → FreeType 光栅化 → 栈溢出 12.6KB 跌进 .bss，
 *   把 esp_pm 的 s_rtos_lock_handle[]（PM 锁句柄）与 task_wdt 的静态量一起写坏。
 *   现象很误导：Core0/Core1 在 esp_pm_lock_acquire/leave_idle 里
 *   LoadProhibited（句柄变成 0x19/0xd4 这种小整数），Core1 空闲任务还报
 *   `task_wdt: esp_task_wdt_reset: task not found`；
 *   用硬件**写监视点**才抓到真凶是 smooth/ftgrays.c:621 的 gray_set_cell。
 *
 * 缩小池子是安全的：池子不够时 ftgrays.c 会自动把字形切成水平"带"多次
 * 光栅化（banding），**结果位图不变**，只是复杂字形稍慢；而且每个字形只
 * 光栅化一次，之后走 LVGL 的字形缓存（LV_FREETYPE_CACHE_FT_GLYPH_CNT）。
 *
 * 取值：ftgrays.c / ftraster.c 里 `FT_RENDER_POOL_SIZE <= 2048` 时按 2048 算，
 * 所以 2048 是下限。这里取 4096 折中 —— 栈开销 16.4KB → 4.1KB，
 * 分带次数也不会太多。若将来内部 RAM 变宽裕、想换回接近默认的渲染性能，
 * 把它调到 8192（栈 ~8.2KB）即可，但**同时要核对下面几个任务的栈余量**：
 * LVGL 任务、deep_worker（息屏唤醒后 lv_refr_now 全屏重刷）、主任务。
 ******************************************************************************/
#ifndef FTOPTION_LVGL_H
#define FTOPTION_LVGL_H

/* 1) 先原样包含组件生成的那份配置（本文件必须能在其之后再改宏） */
#include <freetype/config/ftoption.h>

/* 2) 再把渲染池压小（见上面长篇说明，勿随手改大） */
#undef FT_RENDER_POOL_SIZE
#define FT_RENDER_POOL_SIZE  4096L

#endif /* FTOPTION_LVGL_H */
