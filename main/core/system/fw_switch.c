#include "core/system/fw_switch.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"

static const char *TAG = "fw_switch";

/* NVS：跨固件结果交接（对时固件写 → 播放器开机读一次并清除） */
#define FW_NVS_NS       "tsync"
#define FW_NVS_KEY_RES  "res"

bool fw_role_is_sync(void)
{
#ifdef CONFIG_FIRMWARE_ROLE_SYNC
    return true;
#else
    return false;
#endif
}

const char *fw_role_name(void)
{
    return fw_role_is_sync() ? "sync" : "player";
}

const char *fw_running_partition_label(void)
{
    /* esp_partition_t 的 label 是指向分区表 mmap 区的指针，拷出来更安全 */
    static char s_label[20];
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p == NULL) {
        snprintf(s_label, sizeof(s_label), "?");
    } else {
        snprintf(s_label, sizeof(s_label), "%s", p->label);
    }
    return s_label;
}

/* 按 subtype 找 app 分区（不按名字：名字改过也能用；subtype 才是语义） */
static const esp_partition_t *find_app_partition(esp_partition_subtype_t subtype)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                        subtype, NULL);
    if (p == NULL) {
        ESP_LOGE(TAG, "app partition (subtype 0x%02x) not found —— 分区表没重刷？",
                 (unsigned)subtype);
    }
    return p;
}

esp_err_t fw_switch_to_sync(void)
{
    const esp_partition_t *p = find_app_partition(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "switch next boot -> %s @0x%06x (size 0x%x)", p->label,
             (unsigned)p->address, (unsigned)p->size);

    /* ⚠ 会先做 ESP_IMAGE_VERIFY：槽里是 0xFF（没烧）或镜像损坏时返回
       ESP_ERR_OTA_VALIDATE_FAILED，此时 otadata 不会被改动，播放器不受影响 */
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s", p->label,
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "otadata updated: next boot = %s", p->label);
    return ESP_OK;
}

esp_err_t fw_switch_to_player(void)
{
    const esp_partition_t *p = find_app_partition(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    if (p == NULL) {
        /* factory 不存在时才退而求其次：擦 otadata 同样让 bootloader 回默认分区 */
        const esp_partition_t *ota = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
        if (ota == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        return esp_partition_erase_range(ota, 0, ota->size);
    }
    ESP_LOGI(TAG, "switch next boot -> %s @0x%06x (擦 otadata)", p->label,
             (unsigned)p->address);

    /* 目标为 factory 时 IDF 的实现就是"擦掉 otadata"（见 esp_ota_ops.c 的
       esp_ota_set_boot_partition_internal），所以之后任何复位都回播放器。 */
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s", p->label,
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "otadata erased: next boot = %s", p->label);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 跨固件结果交接                                                       */
/* ------------------------------------------------------------------ */

void fw_sync_result_set_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(FW_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s): %s （对时结果无法交接给播放器）",
                 FW_NVS_NS, esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, FW_NVS_KEY_RES, text);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store sync result failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sync result stored: %s", text);
    }
    nvs_close(h);
}

bool fw_sync_result_take_text(char *buf, size_t len)
{
    if (buf == NULL || len < 2) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(FW_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return false;   /* 没有命名空间 = 从来没对时过，正常情况 */
    }

    size_t need = len;
    err = nvs_get_str(h, FW_NVS_KEY_RES, buf, &need);
    if (err == ESP_OK) {
        /* 取到就清掉：只 Toast 一次 */
        nvs_erase_key(h, FW_NVS_KEY_RES);
        nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK);
}
