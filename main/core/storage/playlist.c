#include "core/storage/playlist.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/pldata.h"
#include "core/storage/playlist_store.h"
#include "esp_log.h"

static const char *TAG = "playlist";

/* 映射后缓存的分区指针（数据始终在 Flash，仅做只读解析） */
static const uint8_t *s_data = NULL;        /* 播放列表数据起点（含 PLST 头） */
static const playlist_header_t *s_hdr = NULL;
static const playlist_dir_entry_t *s_dirs = NULL;
static const playlist_song_entry_t *s_songs = NULL;
static const char *s_strpool = NULL;
static bool s_from_buffer = false;          /* true = 数据源是内存 buffer（非 mmap） */

/* 从 s_data 解析出各分区指针（magic 校验） */
static esp_err_t playlist_parse(const uint8_t *data)
{
    s_data = data;
    s_hdr = (const playlist_header_t *)s_data;

    if (memcmp(s_hdr->magic, PLS_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "bad playlist magic (%.4s)", s_hdr->magic);
        s_data = NULL; s_hdr = NULL;
        return ESP_ERR_INVALID_VERSION;
    }

    s_dirs   = (const playlist_dir_entry_t *)(s_data + s_hdr->dir_offset);
    s_songs  = (const playlist_song_entry_t *)(s_data + s_hdr->song_offset);
    s_strpool = (const char *)(s_data + s_hdr->str_offset);
    return ESP_OK;
}

esp_err_t playlist_init(void)
{
    if (!playlist_store_is_valid()) {
        ESP_LOGW(TAG, "playlist partition not valid (尚未保存播放列表)");
        return ESP_ERR_INVALID_STATE;
    }

    const void *base = NULL;
    esp_err_t ret = playlist_store_map_to_memory(&base);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 播放列表数据存于 playlist_store 数据区起点 */
    const uint8_t *data = (const uint8_t *)base + PLAYLIST_STORE_DATA_OFFSET;
    ret = playlist_parse(data);
    if (ret != ESP_OK) {
        pldata_munmap();
        return ret;
    }
    s_from_buffer = false;

    ESP_LOGI(TAG, "playlist loaded: dirs=%u songs=%u str_len=%u",
             (unsigned)s_hdr->dir_count, (unsigned)playlist_song_count(),
             (unsigned)s_hdr->str_len);
    return ESP_OK;
}

esp_err_t playlist_init_from_buffer(const void *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = playlist_parse((const uint8_t *)data);
    if (ret != ESP_OK) {
        return ret;
    }
    s_from_buffer = true;
    ESP_LOGI(TAG, "playlist parsed from memory buffer: dirs=%u songs=%u str_len=%u",
             (unsigned)s_hdr->dir_count, (unsigned)playlist_song_count(),
             (unsigned)s_hdr->str_len);
    return ESP_OK;
}

void playlist_deinit(void)
{
    if (!s_from_buffer) {
        pldata_munmap();
    }
    s_data = NULL; s_hdr = NULL; s_dirs = NULL; s_songs = NULL; s_strpool = NULL;
    s_from_buffer = false;
}

uint32_t playlist_dir_count(void)
{
    return s_hdr ? s_hdr->dir_count : 0;
}

uint32_t playlist_song_count(void)
{
    if (!s_hdr || s_hdr->dir_count == 0) {
        return 0;
    }
    /* 歌曲数组连续无空洞，总数 = 最后一个目录的终点 */
    const playlist_dir_entry_t *last = &s_dirs[s_hdr->dir_count - 1];
    return last->song_start + last->song_count;
}

const playlist_dir_entry_t *playlist_dir_at(uint32_t i)
{
    return (s_dirs && i < s_hdr->dir_count) ? &s_dirs[i] : NULL;
}

const playlist_song_entry_t *playlist_song_at(uint32_t i)
{
    if (!s_songs || i >= playlist_song_count()) {
        return NULL;
    }
    return &s_songs[i];
}

const char *playlist_get_string(uint32_t idx)
{
    return s_strpool ? s_strpool + idx : "";
}

size_t playlist_build_path(uint32_t song_idx, char *buf, size_t buf_size)
{
    if (!s_songs || song_idx >= playlist_song_count() || buf == NULL || buf_size == 0) {
        if (buf && buf_size) buf[0] = '\0';
        return 0;
    }
    const playlist_song_entry_t *song = &s_songs[song_idx];
    const char *name = playlist_get_string(song->name_str);
    const char *path = playlist_get_string(song->path_str);
    if (path[0]) {
        snprintf(buf, buf_size, "/sdcard/%s/%s", path, name);
    } else {
        snprintf(buf, buf_size, "/sdcard/%s", name);
    }
    return strlen(buf);
}
