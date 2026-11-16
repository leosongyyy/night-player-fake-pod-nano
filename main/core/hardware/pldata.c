#include "core/hardware/pldata.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/hardware/sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pldata";

/* 卡上缓存文件位置。
   ⚠ 目录名以 `.` 开头：playlist_builder 的 collect_dir() 跳过隐藏项，
     所以既不会被当成音乐目录扫进播放列表，也不会出现在 UI 里。
   ⚠ 只写这两个路径（正式文件 + 同级 tmp），绝不碰卡上其它文件。 */
#define PLDATA_DIR_PATH   SD_MOUNT_POINT "/.ecov"
#define PLDATA_FILE_PATH  PLDATA_DIR_PATH "/pldata.bin"
#define PLDATA_TMP_PATH   PLDATA_DIR_PATH "/pldata.tmp"

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

/* 整份镜像（4KB 头 + PLST 数据本体）常驻 PSRAM；NULL = 尚未分配。
   稳态约 620KB（9042 首），上限 PLDATA_MAX_IMAGE（4MB + 4KB）。 */
static uint8_t *s_buf = NULL;
/* 已分配容量（字节），按 PLDATA_SECTOR_SIZE 向上取整；只增不减 */
static size_t   s_cap = 0;
/* 有效镜像长度（= 卡上文件大小；0 = 没有数据）。读写都以它为边界 */
static size_t   s_len = 0;
/* 缓冲已改动，需要 pldata_commit() 落盘 */
static bool     s_dirty = false;
/* pldata_init() 只真正执行一次（之后重复调用只回报结果） */
static bool     s_inited = false;

/* ------------------------------------------------------------------ */
/* 内部辅助                                                            */
/* ------------------------------------------------------------------ */

/* 确保目录存在（已存在也算成功）。失败不致命：commit 会在 fopen 时再报错 */
static void pldata_ensure_dir(void)
{
    if (mkdir(PLDATA_DIR_PATH, 0777) == 0) {
        ESP_LOGI(TAG, "created cache dir %s", PLDATA_DIR_PATH);
        return;
    }
    if (errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d), commit may fail", PLDATA_DIR_PATH, errno);
    }
}

/* 按需增长缓冲（PSRAM）。只在 4KB → 整份镜像 这两次增长，不做指数式过度分配。
   ⚠ 增长出来的新区域内容未初始化，但调用方一定会先 memset 0xFF 或 memcpy 覆盖
     到 s_len 范围内，未覆盖部分不会被写进文件。 */
static esp_err_t pldata_ensure_capacity(size_t need)
{
    if (need > PLDATA_MAX_IMAGE) {
        ESP_LOGE(TAG, "capacity request %u exceeds image limit %u",
                 (unsigned)need, (unsigned)PLDATA_MAX_IMAGE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (need <= s_cap) {
        return ESP_OK;
    }

    size_t cap = (need + PLDATA_SECTOR_SIZE - 1) & ~(size_t)(PLDATA_SECTOR_SIZE - 1);
    uint8_t *buf = (s_buf == NULL)
                       ? heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                       : heap_caps_realloc(s_buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "alloc %u B PSRAM failed (free=%u, largest=%u)", (unsigned)cap,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "buffer %u → %u B (PSRAM)", (unsigned)s_cap, (unsigned)cap);
    s_buf = buf;
    s_cap = cap;
    return ESP_OK;
}

/* 从卡上文件整块读进 PSRAM。返回 ESP_ERR_NOT_FOUND = 没有缓存文件（正常） */
static esp_err_t pldata_load_file(void)
{
    FILE *f = fopen(PLDATA_FILE_PATH, "rb");
    if (f == NULL) {
        ESP_LOGI(TAG, "no cache file (%s), playlist needs scanning", PLDATA_FILE_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    long sz = -1;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "seek %s failed, ignored", PLDATA_FILE_PATH);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    /* 至少要有 1 个头部扇区；上限就是镜像上限（更可能是别的什么东西占了这个名字） */
    if (sz < (long)PLDATA_SECTOR_SIZE || sz > (long)PLDATA_MAX_IMAGE) {
        ESP_LOGW(TAG, "cache file size %ld out of range [%u, %u], ignored",
                 sz, (unsigned)PLDATA_SECTOR_SIZE, (unsigned)PLDATA_MAX_IMAGE);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = pldata_ensure_capacity((size_t)sz);
    if (ret != ESP_OK) {
        fclose(f);
        return ret;
    }

    size_t got = fread(s_buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        ESP_LOGE(TAG, "short read %u/%ld B from %s", (unsigned)got, sz, PLDATA_FILE_PATH);
        return ESP_ERR_INVALID_SIZE;
    }
    s_len = (size_t)sz;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t pldata_init(void)
{
    if (s_inited) {
        return (s_len > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_inited = true;
    s_len = 0;

    /* ⚠ 本函数要在 sd_card_init() 之后调用：整份镜像在卡上 */
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, pldata unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = pldata_load_file();
    if (ret != ESP_OK) {
        /* 没有可用缓存不是错误路径：boot_flow 会扫描，扫描末尾 commit 生成文件 */
        return (ret == ESP_ERR_NO_MEM) ? ret : ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "pldata loaded from SD: %u B in %lld ms (%s)", (unsigned)s_len,
             (long long)((esp_timer_get_time() - t0) / 1000), PLDATA_FILE_PATH);
    return ESP_OK;
}

bool pldata_has_data(void)
{
    return s_len > 0;
}

size_t pldata_get_size(void)
{
    return s_cap;
}

esp_err_t pldata_erase_all(void)
{
    if (s_len == 0) {
        return ESP_OK;
    }
    size_t sz = (s_len + PLDATA_SECTOR_SIZE - 1) & ~(size_t)(PLDATA_SECTOR_SIZE - 1);
    return pldata_erase_range(0, sz);
}

esp_err_t pldata_erase_range(uint32_t offset, size_t size)
{
    if (size == 0) {
        return ESP_OK;
    }
    /* 入参契约沿用 flash 后端（扇区对齐），playlist_store 一直这么传 */
    if ((offset % PLDATA_SECTOR_SIZE) != 0 || (size % PLDATA_SECTOR_SIZE) != 0) {
        ESP_LOGE(TAG, "erase range must be %d-byte aligned (off=0x%lx size=%u)",
                 PLDATA_SECTOR_SIZE, (unsigned long)offset, (unsigned)size);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = pldata_ensure_capacity((size_t)offset + size);
    if (ret != ESP_OK) {
        return ret;
    }
    /* 语义等价于 flash 扇区擦除：写全 0xFF。⚠ 不延长 s_len（擦除不产生数据） */
    memset(s_buf + offset, 0xFF, size);
    s_dirty = true;
    return ESP_OK;
}

esp_err_t pldata_write(uint32_t offset, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > (uint64_t)PLDATA_MAX_IMAGE) {
        ESP_LOGE(TAG, "write out of image (off=0x%lx len=%u)", (unsigned long)offset,
                 (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = pldata_ensure_capacity((size_t)offset + len);
    if (ret != ESP_OK) {
        return ret;
    }
    memcpy(s_buf + offset, data, len);
    /* 写入延长镜像长度：文件大小 = 头部扇区 + 数据本体 */
    if (s_len < (size_t)offset + len) {
        s_len = (size_t)offset + len;
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t pldata_read(uint32_t offset, void *dst, size_t len)
{
    if (dst == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > (uint64_t)s_len) {
        /* 无缓存文件时 s_len == 0，任何读都会走到这里（= "无效"，不算错误日志） */
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, s_buf + offset, len);
    return ESP_OK;
}

esp_err_t pldata_mmap(const void **out_ptr, size_t *out_size)
{
    if (out_ptr == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_ptr = s_buf;
    *out_size = s_len;
    return ESP_OK;
}

void pldata_munmap(void)
{
    /* SD 后端：数据常驻 PSRAM，没有映射可解除。
       上层（playlist_deinit、settings_menu 重扫链、deep_worker 注释）仍会调用，
       保留空实现即可，调用点不必改动。 */
}

esp_err_t pldata_commit(void)
{
    if (s_len == 0) {
        ESP_LOGE(TAG, "commit without data");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_dirty) {
        return ESP_OK;   /* 没有改动，不必重复写卡 */
    }

    pldata_ensure_dir();

    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(PLDATA_TMP_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed (写保护卡/卡满?)", PLDATA_TMP_PATH);
        return ESP_FAIL;
    }

    size_t put = fwrite(s_buf, 1, s_len, f);
    bool synced = false;
    if (put == s_len) {
        /* fflush + fsync：让 FatFs 真正 f_sync 到卡，缩短"脏数据在飞"的窗口 */
        fflush(f);
        synced = (fsync(fileno(f)) == 0);
    }
    fclose(f);

    if (put != s_len) {
        ESP_LOGE(TAG, "short write %u/%u B, keep old cache", (unsigned)put, (unsigned)s_len);
        remove(PLDATA_TMP_PATH);
        return ESP_FAIL;
    }
    if (!synced) {
        /* 同步失败仍继续（数据多半已写入），只是记一笔，便于排查坏卡 */
        ESP_LOGW(TAG, "fsync failed (errno=%d), data may not be fully flushed", errno);
    }

    /* 原子替换：FAT/exFAT 的 rename 不覆盖已存在文件，故先删旧文件。
       ⚠ 最坏情况（删完旧文件、rename 前断电）＝ 卡上没有缓存 → 下次开机重扫，
         不会读到半截数据。 */
    remove(PLDATA_FILE_PATH);
    if (rename(PLDATA_TMP_PATH, PLDATA_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "rename %s → %s failed", PLDATA_TMP_PATH, PLDATA_FILE_PATH);
        remove(PLDATA_TMP_PATH);
        return ESP_FAIL;
    }

    s_dirty = false;
    ESP_LOGI(TAG, "committed %u B to SD in %lld ms", (unsigned)s_len,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

esp_err_t pldata_delete_cache_file(void)
{
    esp_err_t ret = ESP_OK;

    /* 残留的 tmp（上次写卡中途断电）一并清掉 */
    remove(PLDATA_TMP_PATH);
    if (remove(PLDATA_FILE_PATH) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "remove %s failed (errno=%d)", PLDATA_FILE_PATH, errno);
        ret = ESP_FAIL;
    }

    /* 缓冲保留（不 free）：马上要重扫/重启，留着还能直接复用；有效长度清零即可让
       playlist_store_is_valid() 立刻返回 false。上层调用本函数前必须已经
       playlist_deinit()（清掉指向缓冲的指针）。 */
    s_len = 0;
    s_dirty = false;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "cache file deleted: %s", PLDATA_FILE_PATH);
    }
    return ret;
}
