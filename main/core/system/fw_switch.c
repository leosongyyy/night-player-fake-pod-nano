#include "core/system/fw_switch.h"

#include <stdio.h>

#include "core/hardware/rtc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "fw_switch";

/* 与 bootloader_components/main/bootloader_start.c 的协议保持一致：
   PCF85063A RAM_byte(0x03)：0xA5=下次启动 ota_0（里世界），0x5A=factory，
   其它=不干预。bootloader 读完会清 0，所以这里的写是一次性请求。 */
#define RTC_BOOT_RAM_REG     0x03
#define RTC_BOOT_REQ_INNER   0xA5

/* 1 = 自定义 bootloader（bootloader_components/main）在场：RTC 主路径，
       切换不写/不擦 otadata（0 次 flash 擦除）；
   0 = 纯 otadata 路径（原来的行为）。

   ⚠ 这两个开关必须和 bootloader 的实际烧录内容匹配：
      · 保留 bootloader_components/main 时用 1；
      · 移除自定义 bootloader（恢复 IDF 默认 bootloader）时必须改 0，
        否则 App 只写 RTC、默认 bootloader 不认，切换会静默失败（重启回表世界）。 */
#define FW_SWITCH_USE_RTC_BOOT  1

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

    /* ⚠ 必须先完整校验 ota_0 镜像（只读，不写 flash）：
       本机 sdkconfig 开了 CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON/ALWAYS=y，
       bootloader 在 power-on / always 模式下**不做镜像校验**（见
       esp_image_format.c 的 bootloader_load_image）。旧路径靠
       esp_ota_set_boot_partition() 内部的全量 verify 兜底；RTC 路径如果只查
       4 字节头，损坏的镜像会被 bootloader 直接加载 → 崩在里世界。
       这里用 ESP_IMAGE_VERIFY_SILENT 做同样的全量校验，仍然只读。 */
    esp_partition_pos_t pos = {
        .offset = p->address,
        .size   = p->size,
    };
    esp_image_metadata_t meta;
    esp_err_t verr = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (verr != ESP_OK) {
        ESP_LOGE(TAG, "ota_0 image verify failed: %s", esp_err_to_name(verr));
        return verr;
    }

#if FW_SWITCH_USE_RTC_BOOT
    /* 主路径：只写 RTC 的一次性启动请求（SRAM，无擦写），再读回确认。
       成功就**完全不碰 otadata** —— 这次切换 0 次 flash 擦除。 */
    esp_err_t rtc_err = rtc_write_reg(RTC_BOOT_RAM_REG, RTC_BOOT_REQ_INNER);
    if (rtc_err == ESP_OK) {
        uint8_t rb = 0;
        if (rtc_read_reg(RTC_BOOT_RAM_REG, &rb) == ESP_OK && rb == RTC_BOOT_REQ_INNER) {
            ESP_LOGI(TAG, "RTC boot request 0x%02X written; otadata untouched "
                          "(no flash erase)", RTC_BOOT_REQ_INNER);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "RTC boot request readback mismatch, falling back to otadata");
    } else {
        ESP_LOGW(TAG, "RTC boot request write failed (%s), falling back to otadata",
                 esp_err_to_name(rtc_err));
    }
#endif  /* FW_SWITCH_USE_RTC_BOOT */

    /* 兜底：RTC 不可用时才走原来的 otadata 写入（擦 1 个 4KB 扇区）。
       ⚠ 会先做 ESP_IMAGE_VERIFY；槽损坏时返回 ESP_ERR_OTA_VALIDATE_FAILED，
       otadata 不会被改动，表世界不受影响。 */
    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s) failed: %s", p->label,
                 esp_err_to_name(err));
        /* 撤销刚写的 RTC 请求，避免下次启动被一个失效请求带进 ota_0 */
#if FW_SWITCH_USE_RTC_BOOT
        rtc_write_reg(RTC_BOOT_RAM_REG, 0x00);
#endif
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

    /* RTC 主路径不会写 otadata：里世界开机时它已经是空的。
       只有 otadata 非空（上次是 RTC 失败后的兜底切换）才需要擦 8KB。
       每 4KB 扇区只读前 32 字节（otadata entry 的固定位置）判断是否为空。 */
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (ota != NULL) {
        bool blank = true;
        for (uint32_t off = 0; off < ota->size; off += ota->erase_size) {
            uint8_t entry[32];
            if (esp_partition_read(ota, off, entry, sizeof(entry)) != ESP_OK) {
                blank = false;
                break;
            }
            for (size_t i = 0; i < sizeof(entry); i++) {
                if (entry[i] != 0xFF) {
                    blank = false;
                    break;
                }
            }
            if (!blank) {
                break;
            }
        }
        if (blank) {
            ESP_LOGI(TAG, "otadata already empty (RTC path), skip erase");
            return ESP_OK;
        }
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

/* 2026-11 删除「跨固件结果交接」（NVS "tsync/res"）：
   表世界不再在开机时确认 WiFi 对时结果（用户要求），对时结果只在里世界
   列表里显示，因此不需要跨固件传递文本了。 */
