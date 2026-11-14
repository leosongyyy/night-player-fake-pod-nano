#include "core/storage/playlist_store.h"

#include <string.h>

#include "core/hardware/pldata.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "playlist_store";

/* 头部魔数："PLTK"，用于 is_valid 快速判断 */
#define PLAYLIST_STORE_MAGIC 0x504C544Bu
/* 数据结构版本（写入头部，便于将来升级迁移）
 * v3：音频格式从仅 MP3 扩展到 MP3/FLAC/OGG，旧缓存自动重扫一次；
 *     头部仍保留 SD 卡 ID / 曲库指纹 / 文件数（v2 引入）。 */
#define PLAYLIST_STORE_VERSION 3u

/**
 * @brief 分区头部（占数据区之前的一个扇区，offset 0）
 *
 * magic 是有效性判据；data_len 供读取方得知数据长度；crc32 对数据本体做
 * 附加校验（is_valid 只做快速判断，需要时可据此进一步核验数据完整性）。
 * card_id/signature/file_count 用于开机快速判断缓存是否仍匹配当前 SD 卡。
 */
typedef struct {
    uint32_t magic;        /* 魔数，is_valid 判断依据 */
    uint32_t version;      /* 数据结构版本 */
    uint32_t data_len;     /* 数据本体长度（字节） */
    uint32_t crc32;        /* 数据本体 CRC32 */
    uint64_t card_id;      /* SD 卡身份 */
    uint64_t signature;    /* 曲库指纹 */
    uint32_t file_count;   /* 物理音频文件数 */
    uint32_t reserved;     /* 0 */
} playlist_store_header_t;

/* 头部扇区大小（= 数据区起始偏移） */
#define PLAYLIST_STORE_HEADER_SECTOR_SIZE PLAYLIST_STORE_DATA_OFFSET

/* 读头部到栈上局部变量 */
static esp_err_t playlist_store_read_header(playlist_store_header_t *hdr)
{
    return pldata_read(0, hdr, sizeof(*hdr));
}

bool playlist_store_is_valid(void)
{
    playlist_store_header_t hdr;
    if (playlist_store_read_header(&hdr) != ESP_OK) {
        return false;
    }
    /* 有效性判据：magic/版本匹配，且数据长度不超过分区容量 */
    if (hdr.magic != PLAYLIST_STORE_MAGIC ||
        hdr.version != PLAYLIST_STORE_VERSION) {
        return false;
    }
    if (hdr.data_len > PLAYLIST_STORE_MAX_DATA) {
        return false;
    }
    return true;
}

esp_err_t playlist_store_invalidate(void)
{
    /* 只擦头部扇区（offset 0 起一个扇区），数据区保留，分区立即无效 */
    esp_err_t ret = pldata_erase_range(0, PLAYLIST_STORE_HEADER_SECTOR_SIZE);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "playlist invalidated (header erased)");
    }
    return ret;
}

esp_err_t playlist_store_save_playlist(const void *data, size_t len,
                                       const playlist_store_meta_t *meta)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > PLAYLIST_STORE_MAX_DATA) {
        ESP_LOGE(TAG, "data too large: %u > max %u", (unsigned)len,
                 (unsigned)PLAYLIST_STORE_MAX_DATA);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret;

    /* 1. 最先擦头部 → 分区立即失效（中途断电也保持无效，不会被读到半截数据） */
    ret = pldata_erase_range(0, PLAYLIST_STORE_HEADER_SECTOR_SIZE);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 按 len 对齐扇区擦数据区 */
    ret = pldata_erase_range(PLAYLIST_STORE_DATA_OFFSET,
                             PLAYLIST_STORE_DATA_ERASE_SZ(len));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 3. 写数据本体 */
    ret = pldata_write(PLAYLIST_STORE_DATA_OFFSET, data, len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 最后写头部（commit）——此前所有失败路径都会停留在"无效"状态 */
    playlist_store_header_t hdr = {
        .magic = PLAYLIST_STORE_MAGIC,
        .version = PLAYLIST_STORE_VERSION,
        .data_len = (uint32_t)len,
        .crc32 = esp_rom_crc32_le(0, (const uint8_t *)data, (uint32_t)len),
        .card_id = meta ? meta->card_id : 0,
        .signature = meta ? meta->signature : 0,
        .file_count = meta ? meta->file_count : 0,
        .reserved = 0,
    };
    ret = pldata_write(0, &hdr, sizeof(hdr));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "playlist saved: len=%u crc=0x%08x card=%llx sig=%llx files=%u",
                 (unsigned)len, hdr.crc32,
                 (unsigned long long)hdr.card_id,
                 (unsigned long long)hdr.signature,
                 (unsigned)hdr.file_count);
    }
    return ret;
}

bool playlist_store_get_meta(playlist_store_meta_t *out)
{
    if (out == NULL) {
        return false;
    }
    playlist_store_header_t hdr;
    if (playlist_store_read_header(&hdr) != ESP_OK ||
        hdr.magic != PLAYLIST_STORE_MAGIC ||
        hdr.version != PLAYLIST_STORE_VERSION ||
        hdr.data_len > PLAYLIST_STORE_MAX_DATA) {
        return false;
    }
    out->card_id = hdr.card_id;
    out->signature = hdr.signature;
    out->file_count = hdr.file_count;
    out->reserved = 0;
    return true;
}

esp_err_t playlist_store_map_to_memory(const void **out_ptr)
{
    if (out_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 映射整个分区；数据本体位于 PLAYLIST_STORE_DATA_OFFSET 处 */
    size_t size = 0;
    esp_err_t ret = pldata_mmap(out_ptr, &size);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "playlist mapped at %p (data @ +0x%x)", *out_ptr,
             PLAYLIST_STORE_DATA_OFFSET);
    return ESP_OK;
}

size_t playlist_store_get_data_len(void)
{
    playlist_store_header_t hdr;
    if (playlist_store_read_header(&hdr) != ESP_OK) {
        return 0;
    }
    if (hdr.magic != PLAYLIST_STORE_MAGIC ||
        hdr.version != PLAYLIST_STORE_VERSION ||
        hdr.data_len > PLAYLIST_STORE_MAX_DATA) {
        return 0;
    }
    return hdr.data_len;
}
