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
 * 独占 SD 卡的功能，统一弹「该功能需要前往「里世界」完成…」，确认后写
 * otadata 切到这里。
 *
 * 与旧对时固件的关键差别：
 *   · 旧版：开机擦 otadata → 对时 → 自动重启回表世界（一次性）；
 *   · 新版：开机擦 otadata → 显示功能列表，用户选做哪个 → 点「回到表世界」
 *     才重启。**擦 otadata 这一条纪律不变**：之后任何意外复位/断电都回表世界，
 *     绝不会卡在里世界反复重启。
 *
 * 完整方案见 ideas_and_exps/INNER_WORLD.md。
 */

/**
 * @brief 里世界固件入口（由 main.c 在 CONFIG_FIRMWARE_ROLE_SYNC 时调用）
 *
 * 初始化顺序与旧对时固件一致：config_store → display → LVGL → 字体 →
 * 触摸 → PMU/RTC/SD → 创建里世界界面 → 点亮屏幕。返回后 LVGL 任务继续
 * 渲染，功能由用户点击驱动。
 */
void inner_world_app_run(void);

#ifdef __cplusplus
}
#endif
