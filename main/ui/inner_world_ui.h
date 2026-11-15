#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file inner_world_ui.h
 * @brief 「里世界」主界面：标题 + 功能列表（WiFi对时 / USB传歌）+「回到表世界」
 *
 * 2026-11 新增。里世界是烧在 ota_0 的独立固件（构建变体 sync，见
 * ideas_and_exps/INNER_WORLD.md）：表世界（播放器）点需要网络/独占 SD 卡的
 * 功能时，弹窗确认后重启进来；这里提供两个功能入口，做完点「回到表世界」
 * 重启回播放器（otadata 在里世界启动时已被擦掉，任何重启都回 factory）。
 *
 * 界面结构（全屏黑底，挂屏幕层，不透明）：
 *   ┌──────────────────────────┐
 *   │         里世界            │  标题
 *   │   暂停播放 · 网络对时 · 传歌 │  副标题
 *   │  ┌────────────────────┐  │
 *   │  │ WiFi对时      12:34 │  │  功能行（点击进入）
 *   │  ├────────────────────┤  │
 *   │  │ USB传歌     可传歌  │  │  功能行（点击进入）
 *   │  └────────────────────┘  │
 *   │                          │
 *   │      [回到表世界]         │  底部按钮 → esp_restart()
 *   └──────────────────────────┘
 *
 * ⚠ 全部接口须在 LVGL 任务/持锁上下文调用（与 ui/time_sync_ui.h 相同约定）。
 */

/** @brief 创建里世界主界面（只建一次；重复调用无操作） */
void inner_world_ui_create(void);

/** @brief 让里世界主界面重新显示到最前（从对时/U盘界面退回时用） */
void inner_world_ui_show(void);

/** @brief 里世界主界面是否正在显示（USB 传歌模式或对时界面盖住时为 false） */
bool inner_world_ui_is_visible(void);

#ifdef __cplusplus
}
#endif
