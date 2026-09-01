#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/hardware/audio/mp3_player.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放控制器（NightPlayer 播放系统的"第三部分"）
 *
 * 定位在播放引擎核心（mp3_player）与播放列表提供者（playlist）之上，
 * 作为**独立的后台服务**，负责：
 *   - 维护当前播放位置（哪个目录/歌单的哪首歌）
 *   - 播放 / 暂停 / 上一曲 / 下一曲 / 跳转等控制
 *   - 按循环模式在歌曲自然播完后自动续播
 *   - 用静态状态供前端界面读取，用方法供前端界面调用
 *
 * 实现为一个独立 FreeRTOS 任务（固定在 Core0），通过命令队列接收控制请求，
 * 串行处理；把 mp3_player_play() 切换歌曲时最长 2s 的阻塞等待放在独立任务里，
 * 避免卡住 LVGL / UI 所在线程。
 *
 * 初始化（player_controller_init）后默认恢复上次播放位置（NVS 持久化：
 * 歌曲全局下标 + 秒），无记录时从第一个目录的第一首音乐开始播放；
 * 循环模式默认【列表循环】，开机从 NVS 恢复用户设置。
 *
 * 随机播放（shuffle，设置页开关，NVS 持久化）：
 *   基于"无限确定性随机列表"（见 core/player/shuffle_player）：随机序列
 *   由无数个"块"（当前列表歌曲的随机排列）组成，只需保存一个 uint64
 *   逻辑位置即可完全恢复现场。**随机序列固定基于当前播放目录/歌单**
 *   （与循环模式无关，永不跨出当前列表）：位置 P → 目录内索引 → 全局下标，
 *   仅当用户从其它目录选歌时随机列表随之切换。随机位置的重置（重新生成）
 *   仅在以下时机发生：
 *     - 用户从列表指定播放歌曲（player_controller_play_song，含切换到
 *       其它目录的列表选歌）
 *     - 用户把随机播放关闭后再次开启
 *   其余情况（上一曲/下一曲/自然播完/暂停/关机/开机恢复）均保持并保存
 *   随机位置，保证断点续播回到同一首歌、同一序列。
 */

/** 循环模式 */
typedef enum {
    PLAYER_LOOP_ALL = 0,   /* 全部循环：所有目录/歌单连成一条序列，首尾相接 */
    PLAYER_LOOP_SINGLE,    /* 单曲循环：当前歌曲循环播放（手动仍可切相邻曲目） */
    PLAYER_LOOP_LIST,      /* 列表循环：当前列表按顺序播放，播完回到本列表第一首 */
} player_loop_mode_t;

/** 供前端界面读取的播放器状态快照 */
typedef struct {
    mp3_player_state_t playing;   /* IDLE / PLAYING / PAUSED */
    player_loop_mode_t loop_mode; /* 当前循环模式 */
    bool has_song;                /* 是否有可播放的歌曲（已就绪且有歌） */
    uint32_t cur_global;          /* 当前歌曲全局下标（0..total_song_count-1） */
    uint32_t cur_dir;             /* 当前目录/歌单下标 */
    uint32_t cur_song_in_dir;     /* 当前歌曲在目录内的序号（0 起） */
    uint32_t dir_song_count;      /* 当前目录的歌曲总数 */
    uint32_t dir_count;           /* 目录/歌单总数 */
    uint32_t total_song_count;    /* 歌曲总数 */
    uint32_t position_s;          /* 当前播放位置（秒） */
    uint32_t duration_s;          /* 当前歌曲总时长（秒，0=未知） */
    char dir_name[128];           /* 当前目录/歌单名 */
    char song_name[256];          /* 当前歌曲文件名（含扩展名） */
} player_controller_status_t;

/**
 * @brief 初始化播放控制器（创建 Core0 任务 + 命令队列 + 加载播放列表）
 *
 * 内部会调用 playlist_init()（内存映射 pldata 分区）。初始化成功后默认
 * 自动播放第一个目录的第一首音乐。
 *
 * @return ESP_OK 成功；分区无效 / 无音乐 / 资源不足返回错误码。可重复调用，
 *         已初始化时直接返回 ESP_OK。
 */
esp_err_t player_controller_init(void);

/** @brief 控制器是否已就绪（初始化成功且已加载播放列表） */
bool player_controller_is_ready(void);

/* ---------- 控制方法（供前端界面 / 按键调用，线程安全，非阻塞） ---------- */

/** @brief 播放：暂停中恢复；未播放则从头播放当前曲 */
void player_controller_play(void);

/** @brief 暂停（若正在播放） */
void player_controller_pause(void);

/** @brief 播放/暂停切换 */
void player_controller_toggle(void);

/** @brief 下一曲（按循环模式在边界处表现） */
void player_controller_next(void);

/** @brief 上一曲（按循环模式在边界处表现） */
void player_controller_prev(void);

/** @brief 跳转到当前曲的指定位置（秒） */
void player_controller_seek(uint32_t target_s);

/** @brief 播放指定全局下标的歌曲（供未来播放列表 UI 直接选歌） */
void player_controller_play_song(uint32_t global_idx);

/** @brief 停止播放 */
void player_controller_stop(void);

/**
 * @brief 保存当前播放位置到 NVS（供伪关机/睡眠定时器关机前调用）
 *
 * 记录当前歌曲全局下标 + 当前播放位置（秒），下次开机恢复。
 * 控制器未就绪时不做任何事。
 */
void player_controller_save_last_position(void);

/** @brief 设置循环模式 */
void player_controller_set_loop_mode(player_loop_mode_t mode);

/** @brief 读取当前循环模式 */
player_loop_mode_t player_controller_get_loop_mode(void);

/**
 * @brief 设置随机播放开关（true=随机，false=顺序）
 *
 * 关闭→开启时：初始化随机列表，并重新生成随机位置（定位到当前歌曲，
 * 之后"下一曲/上一曲"从新随机序列继续）；开启→关闭时切回顺序播放。
 * 开关状态经 config_store 持久化并通知 UI（设置页图标同步）。
 */
void player_controller_set_shuffle(bool on);

/** @brief 读取当前随机播放开关（与 config_store 持久化值一致） */
bool player_controller_get_shuffle(void);

/** @brief 读取播放器状态快照（供前端界面刷新） */
void player_controller_get_status(player_controller_status_t *out);

#ifdef __cplusplus
}
#endif
