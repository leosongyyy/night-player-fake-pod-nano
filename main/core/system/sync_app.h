#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file sync_app.h
 * @brief 「WiFi对时」专用固件（烧在 `ota_0`）的启动流程
 *
 * 只在 `CONFIG_FIRMWARE_ROLE_SYNC=y`（idfc 构建变体 `sync`）时编进固件。
 * `main.c` 的 `app_main()` 按角色分派：对时固件直接调本函数，不再走播放器
 * 那套初始化（蓝牙/音频/播放列表/按键/电源服务/性能监视统统不初始化）。
 *
 * 流程（顺序有讲究，别改）：
 *   1. **先**把下次启动改回播放器（`fw_switch_to_player()` = 擦 otadata），
 *      之后崩溃/断电/看门狗复位都会回播放器，不会卡在对时固件里；
 *   2. 最小初始化：config_store → display → lvgl_port → 字体 → 触摸 → PMU →
 *      RTC → SD（wifi.txt 在 SD 卡上）；
 *   3. 先渲染黑底 + 对时界面，再给亮度（与播放器开机同一套防白屏顺序）；
 *   4. `time_sync_ui_start()` 起内部 RAM 栈任务跑 WiFi + SNTP，本函数随即返回；
 *   5. 对时结束（成功 2.5s / 失败 8s / 用户点关闭）→ 写一条结果文本进 NVS
 *      （播放器下次启动 Toast 展示）→ `esp_restart()` 回播放器。
 *
 * ⚠ 本函数不返回前不会重启；即使起不来也会在 3 秒后重启回播放器（不死等）。
 */
void sync_app_run(void);

#ifdef __cplusplus
}
#endif
