#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file inner_world_app.h
 * @brief 「里世界」固件的启动流程（矩阵 role=inner，烧在 ota_0）
 *
 * 2026-11 新增：原来的"WiFi 对时固件"升级成里世界固件，除了对时还提供
 * **USB 传歌**（把 SD 卡当 U 盘连电脑）。表世界（播放器）里需要网络或需要
 * 独占 SD 卡的功能，统一弹「该功能需要前往「里世界」完成…」，确认后写 RTC
 * 分区位（`RAM_byte(0x03)` 的 bit1:0 = 0b01）切到这里。
 *
 * 与旧对时固件的关键差别：
 *   · 旧版：开机擦 otadata → 对时 → 自动重启回表世界（一次性）；
 *   · 新版：显示功能列表，用户选做哪个 → 点「回到表世界」才重启。
 *     ⚠ 不需要"开机先擦 otadata"：分区位是**一次性请求**，bootloader 进 ota_0
 *     时已清 0，所以里世界内任何意外复位/断电都回表世界，绝不会卡在里世界
 *     反复重启（otadata 已完全退出启动路径）。
 *
 * 2026-11-16 起里世界也有完整的电源行为：deep_worker + power_service +
 * button_service（自动息屏 / 息屏未充电超时关机 / 上键长按关机 / 限充 tick）。
 *
 * 完整方案见 ideas_and_exps/INNER_WORLD.md。
 */

/**
 * @brief 里世界固件入口（由 main.c 在 CONFIG_FIRMWARE_ROLE_SYNC 时调用）
 *
 * 初始化顺序：config_store → deep_worker → display → LVGL → 字体 → 触摸 →
 * PMU/RTC(清启动标志)/SD → 创建里世界界面 → 点亮屏幕 → 按键 + 电源服务。
 * 返回后 LVGL 任务继续渲染，功能由用户点击驱动。
 */
void inner_world_app_run(void);

#ifdef __cplusplus
}
#endif
