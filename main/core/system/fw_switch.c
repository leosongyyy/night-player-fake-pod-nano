#include "core/system/fw_switch.h"

#include <stdio.h>

#include "core/hardware/rtc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "fw_switch";

/* 与 bootloader_components/main/bootloader_start.c 的协议保持一致（改一处必须
   改两处）：PCF85063A RAM_byte(0x03) 是一个**位域**，跨 SoC 复位/跨固件保持：

     bit1:0  分区选择  0b00 = factory（表世界，默认）；0b01 = ota_0（里世界）
     bit2    充电界面  1 = 本次启动进充电界面（App 消费后立刻清 0）
     bit7:3  保留（恒 0）

   · bootloader 只读/清分区位，**不碰 bit2**；
   · 本模块写请求时都是"读-改-写"，不会顺手抹掉另一位。 */
#define RTC_BOOT_RAM_REG        0x03
#define RTC_BOOT_PART_MASK      0x03u
#define RTC_BOOT_PART_INNER     0x01u
#define RTC_BOOT_FLAG_CHARGE_UI 0x04u

/* 2026-11-16 起**全面迁移到 RTC 字节**（原来的 FW_SWITCH_USE_RTC_BOOT 开关已删除）：
   bootloader_components/main 是**必备件**，启动分区只由 PCF85063A 的
   RAM_byte(0x03) 决定，otadata 完全不参与（见 bootloader_start.c 的 rtc_boot_select）。
   · 请求里世界 = 分区位写 0b01（SRAM，0 次 flash 擦写）；
   · 回表世界 = 分区位清 0（同样 0 次擦写），bootloader 见 0b00 直接选 factory。
   ⚠ 如果把自定义 bootloader 换回 IDF 默认版本，本模块的切换会失效（默认
     bootloader 只认 otadata）——那时必须同时把切换实现改回 esp_ota_set_boot_partition()。 */

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

    /* 唯一路径（2026-11-16 起全面迁移到 RTC 字节，otadata 不再参与启动）：
       只写 RTC 分区位（SRAM，无擦写），再读回确认 → 这次切换 0 次 flash 擦除。
       ⚠ 读-改-写：只替换分区位，bit2（充电界面请求）与保留位原样保留。 */
    uint8_t cur = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &cur) != ESP_OK) {
        cur = 0;   /* 读失败按 0 处理（写成功与否由读回校验决定） */
    }
    const uint8_t want = (uint8_t)((cur & (uint8_t)~RTC_BOOT_PART_MASK)
                                   | RTC_BOOT_PART_INNER);
    esp_err_t rtc_err = rtc_write_reg(RTC_BOOT_RAM_REG, want);
    if (rtc_err != ESP_OK) {
        /* 没有 otadata 兜底了：写不进去就让调用方报"里世界固件不可用"，
           这比悄悄按 otadata 切换更安全（那会让 RTC 与 otadata 两套状态打架）。 */
        ESP_LOGE(TAG, "RTC boot request write failed: %s", esp_err_to_name(rtc_err));
        return rtc_err;
    }
    uint8_t rb = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &rb) != ESP_OK
            || (rb & (uint8_t)RTC_BOOT_PART_MASK) != RTC_BOOT_PART_INNER) {
        ESP_LOGE(TAG, "RTC boot request readback mismatch (0x%02X), abort switch", rb);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RTC boot request written (rtc03=0x%02X): next boot = ota_0; "
                  "no flash erase (otadata unused)", rb);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 启动标志：本次启动是否要进充电界面（bit2）                            */
/* ------------------------------------------------------------------ */

esp_err_t fw_switch_request_charge_ui(void)
{
    uint8_t b = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &b) != ESP_OK) {
        b = 0;   /* 读失败：从 0 起写（分区位为 0 = 不切分区，正是我们要的） */
    }
    const uint8_t want = (uint8_t)(b | RTC_BOOT_FLAG_CHARGE_UI);
    esp_err_t err = rtc_write_reg(RTC_BOOT_RAM_REG, want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "charge-ui request write failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t rb = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &rb) != ESP_OK
            || (rb & RTC_BOOT_FLAG_CHARGE_UI) == 0) {
        ESP_LOGE(TAG, "charge-ui request readback mismatch (0x%02X)", rb);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RTC boot flag set: next boot -> charging UI (rtc03=0x%02X)", rb);
    return ESP_OK;
}

bool fw_switch_charge_ui_requested(void)
{
    uint8_t b = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &b) != ESP_OK) {
        return false;
    }
    return (b & RTC_BOOT_FLAG_CHARGE_UI) != 0;
}

void fw_switch_clear_charge_ui_request(void)
{
    uint8_t b = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &b) != ESP_OK) {
        return;
    }
    if ((b & RTC_BOOT_FLAG_CHARGE_UI) == 0) {
        return;
    }
    const uint8_t want = (uint8_t)(b & (uint8_t)~RTC_BOOT_FLAG_CHARGE_UI);
    if (rtc_write_reg(RTC_BOOT_RAM_REG, want) != ESP_OK) {
        ESP_LOGW(TAG, "clear charge-ui flag failed (still 0x%02X)", b);
    } else {
        ESP_LOGI(TAG, "charge-ui flag consumed (rtc03 0x%02X -> 0x%02X)", b, want);
    }
}

void fw_switch_clear_boot_flags(void)
{
    /* 兜底清掉所有一次性标志（分区位 + 充电界面位），保留 bit7:3 给以后用。
       正常情况 bootloader 已消费分区位、App 已消费 bit2，这里只是防止陈旧值
       把下一次复位带错（例如 bootloader 读 RTC 失败时没消费）。 */
    uint8_t b = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &b) != ESP_OK) {
        return;
    }
    const uint8_t want = (uint8_t)(b & (uint8_t)~(RTC_BOOT_PART_MASK
                                                  | RTC_BOOT_FLAG_CHARGE_UI));
    if (want != b && rtc_write_reg(RTC_BOOT_RAM_REG, want) == ESP_OK) {
        ESP_LOGI(TAG, "stale boot flags cleared (rtc03 0x%02X -> 0x%02X)", b, want);
    }
}

esp_err_t fw_switch_to_player(void)
{
    const esp_partition_t *p = find_app_partition(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    if (p == NULL) {
        /* 分区表里没有 factory：bootloader 也就没有"表世界"可回，直接报错。
           （旧实现在这里擦 otadata 兜底，但 otadata 已不参与启动决策。） */
        ESP_LOGE(TAG, "factory partition missing: cannot switch back to player");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2026-11-16 起**不再擦 otadata**：bootloader 只认 RTC 字节（bit1:0=0b00 →
       factory），"回表世界"只需把分区位清 0（读-改-写，保留 bit2 等标志）。
       以前要判断 otadata 是否为空、非空就擦 8KB —— 现在完全没必要；而且 otadata
       里可能还躺着旧固件写下的"下次启动 ota_0"，新 bootloader 已经不看它了。 */
    uint8_t b = 0;
    if (rtc_read_reg(RTC_BOOT_RAM_REG, &b) != ESP_OK) {
        /* RTC 读不到：bootloader 也读不到 → 它同样走 factory，降级方向一致。 */
        ESP_LOGW(TAG, "RTC unavailable: bootloader will fall back to factory anyway");
        return ESP_OK;
    }
    if ((b & (uint8_t)RTC_BOOT_PART_MASK) == 0) {
        ESP_LOGI(TAG, "rtc03=0x%02X already has no partition request; next boot = "
                      "factory @0x%06x (no otadata erase)", b, (unsigned)p->address);
        return ESP_OK;
    }
    const uint8_t want = (uint8_t)(b & (uint8_t)~RTC_BOOT_PART_MASK);
    esp_err_t err = rtc_write_reg(RTC_BOOT_RAM_REG, want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clear partition request failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "switch next boot -> factory: rtc03 0x%02X -> 0x%02X "
                  "(no flash erase)", b, want);
    return ESP_OK;
}

/* 2026-11 删除「跨固件结果交接」（NVS "tsync/res"）：
   表世界不再在开机时确认 WiFi 对时结果（用户要求），对时结果只在里世界
   列表里显示，因此不需要跨固件传递文本了。 */
