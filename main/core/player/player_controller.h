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
    char title[128];              /* 歌曲标题（来自 .npd 元数据，无则同 song_name） */
    char subtitle[128];           /* 副标题 = 艺术家 - 专辑（来自 .npd，无则同 dir_name） */
    bool has_replay_gain;         /* 当前曲是否有回放增益数据（无 .npd 元数据 = false） */
    uint32_t replay_gain_q16;     /* 当前曲的回放增益（16.16 定点，65536 = 1.0 = 0dB）；
                                     与"回放增益总开关"无关，开关状态见 config_store */
} player_controller_status_t;

/**
 * @brief 初始化播放控制器（创建 Core0 任务 + 命令队列 + 加载播放列表）
 *
 * 内部会调用 playlist_init()（读 SD 卡缓存文件并解析）。初始化成功后默认
 * 自动播放第一个目录的第一首音乐。
 *
 * @return ESP_OK 成功；分区无效 / 无音乐 / 资源不足返回错误码。可重复调用，
 *         已初始化时直接返回 ESP_OK。
 */
esp_err_t player_controller_init(void);

/** @brief 控制器是否已就绪（初始化成功且已加载播放列表） */
bool player_controller_is_ready(void);

/**
 * @brief 释放播放控制器（重新扫描等"数据失效后重启"流程使用）
 *
 * 停止播放并等待解码任务退出 → 挂起控制器任务（不再处理命令队列）→
 * 删除命令队列 → 卸载播放列表（munmap + 清空 playlist 指针）。
 *
 * ⚠ 调用后播放列表数据不可再访问（playlist_* 返回空/0），控制器任务已
 * 挂起不会再触碰 playlist 指针；调用方须确保不再投递控制命令
 * （player_post_cmd 对空队列已判空返回）。
 *
 * 可重复调用（幂等）。重新初始化请走 player_controller_init（重启后）。
 */
void player_controller_deinit(void);

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
 * @brief 保存当前播放位置到 NVS（供关机/睡眠定时器关机前调用）
 *
 * 记录当前歌曲全局下标 + 当前播放位置（秒），下次开机恢复。同时把
 * "关机前是否在播放"与**整套当前配置**（音量/循环/随机/睡眠时长/EQ/歌曲，
 * 含所在列表名）存回**当前活跃配置槽位**——这就是"后续关机时会自动保存到
 * 当前配置"（见 ideas_and_exps/CONFIG_SLOTS.md）。
 * 控制器未就绪时不做任何事。
 */
void player_controller_save_last_position(void);

/**
 * @brief 切固件（重启进里世界）前的音频停机
 *
 * 停解码并等播放任务退出 → `audio_shutdown()`（CS43131 pop-free 掉电 + 停 I2S）。
 * **必须在 `fw_switch_to_sync()` 之前调用**；若切换失败（ota_0 没烧/镜像坏），
 * 调用方必须 `audio_recover_after_failed_shutdown()` 把音频拉回来，否则设备会
 * 一直没声音。
 *
 * 背景：只重启而不关 DAC 时，I2S 停止后 CS43131 仍会输出最后一块解码内容；
 * 里世界不初始化音频，于是切进里世界后余音会一直挂着（2026-11 用户实测）。
 */
void player_controller_shutdown_audio_for_switch(void);

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

/**
 * @brief 恢复一套配置的播放相关部分（配置槽位切换用）并立即开始播放
 *
 * 由 ui/config_slots 在切换配置槽位时调用，**原子**交给控制器任务执行：
 * 循环模式 / 随机播放开关 / 随机序列位置 / 当前歌曲一起生效后再播放，避免
 * UI 线程分几次 set 时被其它切歌命令插队（"用旧循环模式播新列表"）。
 *
 * 随机播放且 shuffle_valid 时的恢复策略：
 *   - 随机列表目录重设为**目标歌曲所在目录**，保存的随机位置能映射到该目录内
 *     的歌曲时精确恢复（回到同一首歌、同一序列）；
 *   - 位置无法映射（播放列表数据变了）→ "重新随机定位到目标歌曲"。
 *
 * @param loop          循环模式（越界钳到列表循环）
 * @param shuffle       随机播放开关
 * @param shuffle_valid 随机序列位置是否有效
 * @param shuffle_pos   随机序列逻辑位置（shuffle_valid 时有效）
 * @param song          目标歌曲全局下标（越界回退第一首）
 */
void player_controller_restore_config(player_loop_mode_t loop, bool shuffle,
                                      bool shuffle_valid, uint64_t shuffle_pos,
                                      uint32_t song);

/**
 * @brief 把当前播放现场（歌曲 / 随机序列位置 / 所在列表名）同步进活跃配置槽位
 *
 * 只写 config_store 的内存配置 + 当前槽位 blob（不触发 UI 通知，也不改播放
 * 状态），目的是让"切走前保存旧槽位"存到的是**此刻真正在播的歌**。
 *
 * ⚠ 调用点：core/storage/config_slots 切换槽位前（必须在
 *   config_store_switch_slot 之前，否则存下去的是上一次落盘的旧歌曲）；
 *   关机保存播放位置时也走它（player_controller_save_last_position 内部）。
 *   控制器未就绪（重扫/重启途中）时不做任何事，可安全调用。
 */
void player_controller_sync_config(void);

/** @brief 读取播放器状态快照（供前端界面刷新） */
void player_controller_get_status(player_controller_status_t *out);

/**
 * @brief 获取当前歌曲的完整播放路径（"/sdcard/..."）
 *
 * 返回控制器内部静态缓冲（只读，勿修改/释放）；无当前歌曲时返回空串。
 * 供元数据/封面加载等需要文件路径的模块使用。
 */
const char *player_controller_get_current_path(void);

/** 播放器需要用户知晓的坏曲/异常事件（UI 轮询拉取展示 toast） */
typedef enum {
    PLAYER_EVENT_NONE = 0,           /* 无事件 */
    PLAYER_EVENT_FILE_MISSING,       /* 歌曲文件不存在 */
    PLAYER_EVENT_DATA_CORRUPT,       /* 歌曲数据损坏 */
    PLAYER_EVENT_FORMAT_UNSUPPORTED, /* 歌曲格式不支持 */
    PLAYER_EVENT_TOO_MANY_BAD,       /* 连续坏曲过多导致停止（请检查SD卡） */
} player_event_type_t;

/** 拉取到的事件：类型 + 发生时刻（esp_timer 单调微秒） */
typedef struct {
    player_event_type_t type;   /* 事件类型 */
    int64_t time_us;            /* 事件发生时刻（esp_timer_get_time，us） */
} player_event_t;

/**
 * @brief 拉取最新坏曲事件（单槽，后写覆盖前写；拉取即清空）
 *
 * 控制器遇到坏曲/格式不支持/文件不存在/连续坏曲停止时写入事件槽；
 * UI 侧周期性调用本接口取走事件并展示。带发生时刻供 UI 判断"是否在
 * 近 15 秒内"（锁屏期间 LVGL 冻结、解锁后补拉时用于过滤过时消息）。
 *
 * @param out 输出事件（类型 + 发生时刻）；无事件时不修改
 * @return true = 取到事件（槽已清空）；false = 无事件
 */
bool player_controller_take_event(player_event_t *out);

#ifdef __cplusplus
}
#endif
