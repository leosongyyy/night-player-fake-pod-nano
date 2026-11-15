#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查找歌曲封面（内嵌图片优先，其次同目录封面图片）
 *
 * 目前支持：
 *   - MP3：ID3v2.2/2.3/2.4 的 PIC/APIC 帧（JPEG）
 *   - FLAC：PICTURE 元数据块（JPEG）
 *   - OGG Vorbis：Vorbis comment 里的 METADATA_BLOCK_PICTURE / COVERART（JPEG）
 *   - WAV：RIFF 里的 "ID3 " chunk（ID3v2 标签，与 MP3 共用解析）
 *   - 回退：歌曲所在目录的 cover/folder/front/album/封面 .jpg/.jpeg
 *
 * 只返回 JPEG（cover_jpeg_decode 只支持 JPEG），PNG 等格式会被跳过。
 * 返回的数据优先分配在 PSRAM，调用方用 cover_art_free() 释放。
 *
 * @param song_path 歌曲完整路径（"/sdcard/..."；也接受其它绝对路径）
 * @param jpeg_out  输出 JPEG 数据指针（成功时非 NULL）
 * @param len_out   输出 JPEG 长度
 * @return ESP_OK 找到；ESP_ERR_NOT_FOUND 没有可用封面；其它为读取/内存错误
 */
esp_err_t cover_art_find(const char *song_path, uint8_t **jpeg_out,
                         uint32_t *len_out);

/** @brief 释放 cover_art_find 返回的缓冲（安全；p 可为 NULL） */
void cover_art_free(void *p);

#ifdef __cplusplus
}
#endif
