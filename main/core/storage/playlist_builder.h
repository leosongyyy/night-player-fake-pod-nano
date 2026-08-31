#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表构建器
 *
 * 从 SD 卡（挂载于 /sdcard）递归扫描音频文件，按 NightPlayer 的
 * PLST 格式在内存（PSRAM）中构建播放列表数据，并整体持久化到
 * pldata 分区的 playlist_store。
 *
 * 使用显式栈遍历（非递归），目录/文件按名称前 4 字节排序，
 * 同一目录的歌曲共享父路径字符串。调用方应在一个大栈任务中
 * 调用本函数（扫描 + 写分区较耗时）。
 */

/**
 * @brief 扫描构建阶段
 */
typedef enum {
    PLAYLIST_BUILD_PHASE_SCAN  = 0,  /* 正在扫描 / 遍历目录并收集歌曲 */
    PLAYLIST_BUILD_PHASE_WRITE = 1,  /* 正在把数据写入 Flash 分区（持久化） */
} playlist_build_phase_t;

/**
 * @brief 扫描进度回调（可选）
 *
 * 由扫描任务在构建过程中周期调用：SCAN 阶段每处理完一个目录调用一次，
 * WRITE 阶段开始写入分区时调用一次。回调在扫描任务上下文执行，若要更新
 * LVGL 控件，请只更新共享变量/置标志，不要在回调内直接调用 LVGL API
 * （非线程安全）。
 *
 * @param phase      当前阶段（SCAN 期间周期通知 / WRITE 开始时通知一次）
 * @param song_count 当前已收集的歌曲数量
 * @param user_ctx   调用方传入的用户上下文
 */
typedef void (*playlist_build_progress_cb)(playlist_build_phase_t phase,
                                           uint32_t song_count, void *user_ctx);

/** 构建结果统计 */
typedef struct {
    uint32_t dir_count;     /* 实际目录条目数（含歌曲的目录） */
    uint32_t song_count;    /* 实际歌曲数 */
    uint32_t str_len;       /* 字符串池已用长度 */
    uint32_t total_size;    /* 写入分区的总字节数（= 字符串池起点偏移 + 池长度） */
    bool     truncated;     /* 是否因目录/歌曲/字符串池容量被截断 */
} playlist_build_stats_t;

/**
 * @brief 扫描 SD 卡并构建、保存播放列表
 *
 * @param[out] stats        构建结果统计（可为 NULL）
 * @param progress_cb       扫描进度回调（可为 NULL，则不发进度通知）
 * @param progress_ctx      透传给进度回调的用户上下文
 * @return ESP_OK 成功；SD 未挂载 / 内存不足 / 写入失败返回错误码
 */
esp_err_t playlist_build_from_sd(playlist_build_stats_t *stats,
                                 playlist_build_progress_cb progress_cb,
                                 void *progress_ctx);

/**
 * @brief 仅扫描并构建到内存（不写回分区），返回 buffer 供内存解析测试
 *
 * @param[out] stats    构建结果统计（可为 NULL）
 * @param[out] out_buf  构建完成的 PLST 数据 buffer（调用方负责 free()）
 * @param[out] out_size buffer 字节数
 * @return ESP_OK 成功
 */
esp_err_t playlist_build_in_memory(playlist_build_stats_t *stats,
                                   const void **out_buf, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
