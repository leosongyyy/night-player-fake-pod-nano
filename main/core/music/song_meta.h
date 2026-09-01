#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 歌曲元数据（上位机扫描生成的 .npd 文件解析，见 .ecov 目录）
 *
 * 上位机（NightPlayerNanoInfoScanner，桌面 TestMP3PictureInfo 工程）扫描
 * SD 卡上每首 MP3 的标题 / 艺术家-专辑 / 回放增益 / 封面，重新编码后按
 * 固定二进制格式写入 SD 卡根目录的 .ecov 文件夹，路径与歌曲一一对应：
 *
 *   歌曲    /sdcard/<目录>/<文件>.mp3
 *   → 元数据 /sdcard/.ecov/<目录>/<文件>.mp3.npd
 *
 * .npd 二进制格式（全部小端）：
 *   1. int32  标题字节长度（UTF-8）
 *   2. bytes  标题 UTF-8
 *   3. int32  副标题字节长度（UTF-8）
 *   4. bytes  副标题 UTF-8（"艺术家 - 专辑"）
 *   5. uint32 回放增益线性值 × 65536（16.16 定点，65536 = 1.0 = 0dB）
 *   6. int32  JPEG 封面字节长度（0 = 无封面）
 *   7. bytes  JPEG 封面数据（460x460，黑底居中）
 *
 * 本模块只负责文件解析（不含 JPEG 解码，解码见 core/image/cover_jpeg）。
 */

/** 歌曲元数据（解析结果，字段为独立分配的内存，用完调 song_meta_free） */
typedef struct {
    char *title;         /* 标题（UTF-8，malloc，可能为 NULL） */
    char *subtitle;      /* 副标题 = 艺术家 - 专辑（UTF-8，malloc，可能为 NULL） */
    uint32_t gain_q16;   /* 回放增益（16.16 定点，65536 = 1.0 = 0dB） */
    uint8_t *jpeg_data;  /* JPEG 封面数据（malloc，无封面为 NULL） */
    uint32_t jpeg_len;   /* JPEG 数据长度（无封面为 0） */
} song_meta_t;

/**
 * @brief 由歌曲完整播放路径构造对应的 .npd 元数据文件路径
 *
 * "/sdcard/abc/x.mp3" → "/sdcard/.ecov/abc/x.mp3.npd"
 *
 * @param song_path 歌曲完整路径（须以 /sdcard/ 开头）
 * @param buf       输出缓冲
 * @param buf_size  缓冲大小
 * @return 写入的字符数（不含 '\0'），失败返回 0
 */
size_t song_meta_build_npd_path(const char *song_path, char *buf, size_t buf_size);

/**
 * @brief 读取并解析歌曲元数据文件
 *
 * 内部自动由歌曲路径换算 .npd 路径并读取；字段为 malloc 内存
 * （标题/副标题/JPEG 优先放 PSRAM，内部 RAM 兜底）。
 *
 * @param song_path 歌曲完整路径（如 "/sdcard/Music/a.mp3"）
 * @param out       输出（初始值任意；失败时保证 out 为全零，无需清理）
 * @return ESP_OK 成功；文件不存在 / 格式非法返回错误码
 */
esp_err_t song_meta_load(const char *song_path, song_meta_t *out);

/** @brief 释放 song_meta_load 分配的内存（安全；m 可为 NULL 或全零结构） */
void song_meta_free(song_meta_t *m);

#ifdef __cplusplus
}
#endif
