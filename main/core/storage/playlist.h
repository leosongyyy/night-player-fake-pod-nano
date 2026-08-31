#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/storage/playlist_format.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表读取/解析 API（基于 pldata 分区内存映射，省 RAM）
 *
 * 将 pldata 分区中已保存的播放列表数据内存映射后，按 PLST 格式解析，
 * 提供目录 / 歌曲 / 字符串的只读访问。数据始终留在 Flash，不占堆 RAM。
 *
 * 典型用法：
 * @code
 *   if (playlist_init() == ESP_OK) {
 *       for (i < playlist_dir_count()) { ... playlist_dir_at(i) ... }
 *       for (i < playlist_song_count()) {
 *           char path[512];
 *           playlist_build_path(i, path, sizeof(path)); // "/sdcard/..."
 *       }
 *   }
 * @endcode
 */

/**
 * @brief 初始化播放列表（校验有效 + 内存映射 + 解析各分区指针）
 *
 * @return ESP_OK 成功；分区无效 / 未保存 / magic 不符返回错误码
 */
esp_err_t playlist_init(void);

/**
 * @brief 从内存 buffer 解析播放列表（不写回、不 mmap，用于测试内存内容）
 *
 * 与 playlist_init() 等价，但数据源是内存中的 PLST buffer（如
 * playlist_build_in_memory 的产物），而非分区 mmap。无需释放分区映射。
 *
 * @param data PLST 数据 buffer 指针（生命周期由调用方保证，测试期间需存活）
 * @return ESP_OK 成功；magic 不符返回错误码
 */
esp_err_t playlist_init_from_buffer(const void *data);

/** @brief 释放播放列表映射（可重复调用） */
void playlist_deinit(void);

/** @brief 目录条目总数 */
uint32_t playlist_dir_count(void);

/** @brief 歌曲总数（= 最后一个目录的 song_start + song_count，无目录时为 0） */
uint32_t playlist_song_count(void);

/** @brief 取第 i 个目录条目（i < playlist_dir_count()） */
const playlist_dir_entry_t *playlist_dir_at(uint32_t i);

/** @brief 取第 i 首歌曲条目（i < playlist_song_count()） */
const playlist_song_entry_t *playlist_song_at(uint32_t i);

/** @brief 取字符串池中索引为 idx 的 null-terminated 字符串 */
const char *playlist_get_string(uint32_t idx);

/**
 * @brief 构造第 song_idx 首歌曲的完整播放路径（含 /sdcard 前缀）
 *
 * @param song_idx 歌曲下标
 * @param buf      输出缓冲
 * @param buf_size 缓冲大小
 * @return 写入的字节数（不含结尾 '\0'）
 */
size_t playlist_build_path(uint32_t song_idx, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
