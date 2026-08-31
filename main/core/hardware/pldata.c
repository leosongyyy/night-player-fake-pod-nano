#include "core/hardware/pldata.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "pldata";

/* pldata 分区标签（与 partitions.csv 中的分区名一致） */
#define PLDATA_PART_LABEL "pldata"

/* 分区句柄（init 时获取，后续所有操作都基于它） */
static const esp_partition_t *s_part = NULL;
/* 当前内存映射句柄（0 = 未映射） */
static esp_partition_mmap_handle_t s_map_handle = 0;
/* 当前映射基址（NULL = 未映射） */
static const void *s_mapped_ptr = NULL;

/* 确保分区已 init，未 init 则尝试查找；返回 ESP_OK 后 s_part 一定非空 */
static esp_err_t pldata_ensure_part(void)
{
    if (s_part != NULL) {
        return ESP_OK;
    }
    /* 用 label 查找（type=ANY / subtype=ANY 通配），不依赖具体 subtype 值 */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, PLDATA_PART_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition \"%s\" not found, check partitions.csv", PLDATA_PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    s_part = part;
    ESP_LOGI(TAG, "pldata partition: offset=0x%lx size=%u (%u KB)",
             (unsigned long)part->address, (unsigned)part->size,
             (unsigned)(part->size / 1024U));
    return ESP_OK;
}

esp_err_t pldata_init(void)
{
    return pldata_ensure_part();
}

size_t pldata_get_size(void)
{
    return s_part ? s_part->size : 0;
}

/* 写/擦除前自动解除映射，避免 Flash cache 与写入数据不一致 */
static void pldata_unmap_if_mapped(void)
{
    if (s_map_handle != 0) {
        esp_partition_munmap(s_map_handle);
        s_map_handle = 0;
        s_mapped_ptr = NULL;
        ESP_LOGI(TAG, "mmap auto-unmapped before write/erase");
    }
}

esp_err_t pldata_erase_all(void)
{
    return pldata_erase_range(0, s_part->size);
}

esp_err_t pldata_erase_range(uint32_t offset, size_t size)
{
    esp_err_t ret = pldata_ensure_part();
    if (ret != ESP_OK) {
        return ret;
    }
    /* Flash 擦除粒度是扇区（PLDATA_SECTOR_SIZE），偏移与长度必须按扇区对齐 */
    if ((offset % PLDATA_SECTOR_SIZE) != 0 || (size % PLDATA_SECTOR_SIZE) != 0) {
        ESP_LOGE(TAG, "erase range must be %d-byte aligned (off=0x%lx size=%u)",
                 PLDATA_SECTOR_SIZE, (unsigned long)offset, (unsigned)size);
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + size > s_part->size) {
        ESP_LOGE(TAG, "erase range out of partition");
        return ESP_ERR_INVALID_SIZE;
    }
    pldata_unmap_if_mapped();

    ret = esp_partition_erase_range(s_part, offset, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pldata_write(uint32_t offset, const void *data, size_t len)
{
    esp_err_t ret = pldata_ensure_part();
    if (ret != ESP_OK) {
        return ret;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > s_part->size) {
        ESP_LOGE(TAG, "write out of partition (off=0x%lx len=%u)", (unsigned long)offset, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    /* 目标区域必须已擦除（全 0xFF）。mmap 期间写会造成缓存不一致，先解除映射。 */
    pldata_unmap_if_mapped();

    ret = esp_partition_write(s_part, offset, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s (目标区域需先 erase)", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pldata_read(uint32_t offset, void *dst, size_t len)
{
    esp_err_t ret = pldata_ensure_part();
    if (ret != ESP_OK) {
        return ret;
    }
    if (dst == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > s_part->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 若已映射，直接从映射地址拷贝（省一次 Flash 读事务） */
    if (s_map_handle != 0) {
        memcpy(dst, (const uint8_t *)s_mapped_ptr + offset, len);
        return ESP_OK;
    }
    ret = esp_partition_read(s_part, offset, dst, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pldata_mmap(const void **out_ptr, size_t *out_size)
{
    esp_err_t ret = pldata_ensure_part();
    if (ret != ESP_OK) {
        return ret;
    }
    if (out_ptr == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 已映射则直接复用，避免重复映射消耗 MMU 表项 */
    if (s_map_handle != 0) {
        *out_ptr = s_mapped_ptr;
        *out_size = s_part->size;
        return ESP_OK;
    }

    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle = 0;
    ret = esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s (分区大小需为 64KB 整数倍)", esp_err_to_name(ret));
        return ret;
    }
    s_mapped_ptr = ptr;
    s_map_handle = handle;
    *out_ptr = ptr;
    *out_size = s_part->size;
    ESP_LOGI(TAG, "pldata mapped to %p (size=%u, 仅占 MMU/缓存不占堆 RAM)",
             ptr, (unsigned)s_part->size);
    return ESP_OK;
}

void pldata_munmap(void)
{
    if (s_map_handle != 0) {
        esp_partition_munmap(s_map_handle);
        s_map_handle = 0;
        s_mapped_ptr = NULL;
    }
}
