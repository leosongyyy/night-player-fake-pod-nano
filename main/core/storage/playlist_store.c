#include "core/storage/playlist_store.h"

#include <string.h>

#include "core/hardware/pldata.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "playlist_store";

/* 头部魔数："PLTK"，用于 is_valid 快速判断 */
#define PLAYLIST_STORE_MAGIC 0x504C544Bu
/* 数据结构版本（写入头部，便于将来升级迁移）
 * v4：音频格式从 MP3/FLAC/OGG 扩展到再加 WAV，旧缓存自动重扫一次；
 * v3：音频格式从仅 MP3 扩展到 MP3/FLAC/OGG；
 *     头部仍保留 SD 卡 ID / 曲库指纹 / 文件数（v2 引入）。 */
#define PLAYLIST_STORE_VERSION 4u

/**
 * @brief 头部（占镜像最前面一个扇区，offset 0）
 *
 * magic 是有效性判据；data_len 供读取方得知数据长度；crc32 对数据本体做
 * 完整性校验（SD 后端每次 is_valid 都真校验，见 playlist_store_is_valid）。
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

/* 编译期护栏：镜像（头 + 数据本体）必须装进 pldata 缓冲上限。
   改 PLS_STR_MAX_OFFSET 或 PLDATA_MAX_IMAGE 时忘了同步，这里会直接报错。 */
_Static_assert(PLAYLIST_STORE_DATA_OFFSET + PLAYLIST_STORE_MAX_DATA <= PLDATA_MAX_IMAGE,
               "playlist image does not fit into pldata max image");

/* 头部是否合规（不做 CRC 校验，只做结构判断） */
static bool playlist_store_header_ok(const playlist_store_header_t *hdr)
{
    return hdr->magic == PLAYLIST_STORE_MAGIC &&
           hdr->version == PLAYLIST_STORE_VERSION &&
           hdr->data_len <= PLAYLIST_STORE_MAX_DATA;
}

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
    if (!playlist_store_header_ok(&hdr)) {
        return false;
    }

    /* ⚠ 与 flash 时代不同：SD 卡会坏（掉电写坏扇区、劣质卡），所以这里做一次
       **整块 CRC32 真校验**（620KB ≈ 2ms，相对开机 1.3s 的指纹探测可忽略）。
       老固件如果 crc32 字段是 0（未填），跳过校验，避免把旧缓存误判成坏数据。 */
    if (hdr.crc32 != 0) {
        const void *base = NULL;
        size_t size = 0;
        if (pldata_mmap(&base, &size) != ESP_OK) {
            return false;
        }
        /* 文件可能被截断（写卡中途断电/被 PC 改过）：镜像长度必须真的装得下 data_len */
        if (size < (size_t)PLAYLIST_STORE_DATA_OFFSET + hdr.data_len) {
            ESP_LOGE(TAG, "cache truncated: file=%u B, header needs %u B",
                     (unsigned)size,
                     (unsigned)(PLAYLIST_STORE_DATA_OFFSET + hdr.data_len));
            return false;
        }
        uint32_t crc = esp_rom_crc32_le(
            0, (const uint8_t *)base + PLAYLIST_STORE_DATA_OFFSET, hdr.data_len);
        if (crc != hdr.crc32) {
            ESP_LOGE(TAG, "cache CRC mismatch (stored=0x%08x computed=0x%08x), "
                          "treat as invalid", (unsigned)hdr.crc32, (unsigned)crc);
            return false;
        }
    }
    return true;
}

esp_err_t playlist_store_invalidate(void)
{
    /* SD 后端：**删除卡上的缓存文件**即"立刻失效"（不再像 flash 那样只擦头部扇区）。
       设置菜单「重新扫描」走的就是这条路：删文件 → 重启 → boot_flow 自动重扫。
       ⚠ 调用方必须先 playlist_deinit()（清掉指向 pldata 缓冲的指针）。 */
    esp_err_t ret = pldata_delete_cache_file();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "playlist invalidated (cache file deleted)");
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

    /* 1. 最先擦头部 → 缓冲立即失效（中途失败也不会被读到半截数据） */
    ret = pldata_erase_range(0, PLAYLIST_STORE_HEADER_SECTOR_SIZE);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 按 len 对齐扇区擦数据区（SD 后端只是内存 memset 0xFF） */
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
    if (ret != ESP_OK) {
        return ret;
    }

    /* 5. 落盘：写 tmp → fsync → rename 原子替换。
       失败时卡上仍是旧文件（或仍没有文件），下次开机会重扫，不会读到坏数据。 */
    ret = pldata_commit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "commit cache to SD failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "playlist saved: len=%u crc=0x%08x card=%llx sig=%llx files=%u",
             (unsigned)len, (unsigned)hdr.crc32,
             (unsigned long long)hdr.card_id,
             (unsigned long long)hdr.signature,
             (unsigned)hdr.file_count);
    return ESP_OK;
}

bool playlist_store_get_meta(playlist_store_meta_t *out)
{
    if (out == NULL) {
        return false;
    }
    playlist_store_header_t hdr;
    if (playlist_store_read_header(&hdr) != ESP_OK || !playlist_store_header_ok(&hdr)) {
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
    /* 拿到整份镜像基址；数据本体位于 PLAYLIST_STORE_DATA_OFFSET 处 */
    size_t size = 0;
    esp_err_t ret = pldata_mmap(out_ptr, &size);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "playlist image at %p (size=%u, data @ +0x%x)", *out_ptr,
             (unsigned)size, PLAYLIST_STORE_DATA_OFFSET);
    return ESP_OK;
}

size_t playlist_store_get_data_len(void)
{
    playlist_store_header_t hdr;
    if (playlist_store_read_header(&hdr) != ESP_OK || !playlist_store_header_ok(&hdr)) {
        return 0;
    }
    return hdr.data_len;
}

/* ------------------------------------------------------------------ */
/* 关于 flash 里的旧 `pldata` 分区：**固件完全不再读取它**                 */
/* ------------------------------------------------------------------ */
/* P1 阶段曾有一个"一次性迁移"：卡上没缓存时把 flash 分区里的旧列表导入成
   卡上文件。2026-11-16 已**删除**，原因是要了命的行为冲突：设置菜单「重新扫描」
   的动作是"删缓存文件 → 重启 → 重新扫描"，而迁移逻辑会在重启后立刻把 flash 里的
   旧数据搬回卡上，于是 is_valid() 直接通过、扫描根本不执行（用户看到的是"点重新
   扫描没反应"）。
   代价只是老固件升级后第一开机多扫一次（9047 首约 2.3s，写卡 95ms），完全值得。
   `pldata` 分区现仅作为"死数据"占位，等 P3 删分区 + 重排布局时一起清掉。
   详见 ideas_and_exps/PLDATA_ON_SDCARD.md §14.5。 */
