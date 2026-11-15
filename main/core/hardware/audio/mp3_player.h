#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 本地播放器（MP3/FLAC/OGG Vorbis 流式解码 + 播放控制）
 *
 * 单曲播放：mp3_player_play() 启动 Core1 后台任务，按扩展名选择
 * minimp3 / dr_flac / Tremor(OGG) 解码并输出到 I2S。
 * 提供暂停/恢复/停止/seek 控制与播放位置查询。
 */

/** 播放状态 */
typedef enum {
    MP3_STATE_IDLE = 0,     /* 未在播放 */
    MP3_STATE_PLAYING,      /* 播放中 */
    MP3_STATE_PAUSED,       /* 已暂停 */
} mp3_player_state_t;

/** 播放停止原因（供上层区分用户停止 / 自然播完 / 坏曲类别） */
typedef enum {
    MP3_STOP_USER = 0,             /* 用户主动停止（手动切歌/暂停后停止） */
    MP3_STOP_NATURAL,              /* 自然播完 */
    MP3_STOP_FILE_MISSING,         /* 文件不存在/打开失败 */
    MP3_STOP_DATA_CORRUPT,         /* 音频数据损坏（无有效帧/无音频数据/读取失败） */
    MP3_STOP_FORMAT_UNSUPPORTED,   /* 音频格式不支持（采样率不在支持表等） */
} mp3_player_stop_reason_t;

/**
 * @brief 播放停止回调（在播放任务上下文调用，用于自动播放下一曲等）
 * @param reason 停止原因：USER=用户主动停止（控制器应忽略，切歌命令已就绪）；
 *               NATURAL=自然播完；FILE_MISSING/DATA_CORRUPT/FORMAT_UNSUPPORTED
 *               =坏曲（无法播放，供上层跳过并提示）
 */
typedef void (*mp3_player_stopped_cb_t)(mp3_player_stop_reason_t reason);

/**
 * @brief 开始播放指定音频（MP3/FLAC/OGG，Core1 后台任务，非阻塞）
 *
 * 若当前正在播放/暂停，会先停止旧曲并等待其释放资源（解码器、文件句柄、
 * I2S），再启动新曲，因此可用于切换播放不同采样率的文件（采样率在解码
 * 首帧后自动重配 CS43131 与 I2S）。
 *
 * 前置条件：audio_init() 与 sd_card_init() 已完成。
 *
 * @param path 文件完整路径（内部会拷贝到播放任务自己的缓冲，无需常驻）
 * @return ESP_OK 任务创建成功；ESP_ERR_NO_MEM 创建失败；
 *         ESP_ERR_TIMEOUT 停止旧曲超时（3s 内未释放资源）
 */
esp_err_t mp3_player_play(const char *path);

/** @brief 暂停播放（若正在播放） */
esp_err_t mp3_player_pause(void);

/** @brief 恢复播放（若已暂停） */
esp_err_t mp3_player_resume(void);

/** @brief 停止播放并释放资源 */
esp_err_t mp3_player_stop(void);

/**
 * @brief 跳转到指定播放位置（秒）
 *
 * 由各解码器按样本位置 seek（MP3 首次 seek 可能需要扫描建索引）。
 * 播放中或暂停中均可调用。
 *
 * @param target_s 目标时间（秒，超出总时长会被截断）
 * @return ESP_OK 成功
 */
esp_err_t mp3_player_seek(uint32_t target_s);

/** @brief 获取当前播放状态 */
mp3_player_state_t mp3_player_get_state(void);

/**
 * @brief 获取当前播放位置（秒），基于开始播放时间 + 当前时钟推算
 */
uint32_t mp3_player_get_position_s(void);

/** @brief 获取总时长（秒，由文件大小 / 码率估算） */
uint32_t mp3_player_get_duration_s(void);

/**
 * @brief 设置播放停止回调
 *
 * 播放自然播完或用户停止时都会触发；回调在播放任务（Core1）上下文运行，
 * 内部会先把状态置为 IDLE，因此回调里可直接调用 mp3_player_play() 续播。
 *
 * @param cb 回调函数，传 NULL 取消
 */
void mp3_player_set_stopped_callback(mp3_player_stopped_cb_t cb);

/**
 * @brief 设置"下一首"歌曲的回放增益（16.16 定点线性系数，65536 = 0dB）
 *
 * ⚠ 本接口只暂存增益，**在 mp3_player_play() 等待旧任务完全退出、新曲
 * 解码器打开之后才应用到 audio**：旧曲停止是异步的，若在 play 前直接调
 * audio_set_replay_gain，运行中的旧曲会立刻换增益，造成音量突变。
 * 延迟应用后旧曲全程保持旧增益，新歌从第一帧起用新增益。
 *
 * @param gain_q16 线性增益 × 65536（与 audio_set_replay_gain 同语义）。
 *                 ⚠ 由调用方按"回放增益总开关"决定取值：开关关闭时传
 *                 AUDIO_REPLAY_GAIN_UNITY（0dB），本模块不感知开关状态
 */
void mp3_player_set_gain(uint32_t gain_q16);

#ifdef __cplusplus
}
#endif
