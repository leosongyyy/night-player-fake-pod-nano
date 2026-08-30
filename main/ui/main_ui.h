#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 当前测试曲目（44.1kHz），开机自动播放 */
#define TRACK_44100_PATH "/sdcard/music/Nodkrai/静伫的海峤 The Still Sea-Cliff.mp3"

/**
 * @brief 主界面（MP3 播放控制：播放/暂停 + 可拖动进度条 + 时间 + 音量
 *        + CS43131 关机/开机测试按钮）
 *
 * 内部自注册进度刷新定时器，需在持有 LVGL 锁时调用。
 */
void main_ui_create(void);

/**
 * @brief 当前播放曲目路径
 *
 * main.c 开机自动播放共用。UI 创建前调用亦安全。
 */
const char *main_ui_get_current_track(void);

#ifdef __cplusplus
}
#endif
