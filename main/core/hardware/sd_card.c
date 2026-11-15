#include "core/hardware/sd_card.h"

#include <dirent.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_defs.h"
#include "sd_protocol_types.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

/* SDMMC 引脚（ESP32-S31 / FakePodNano Pro V1，4-bit 模式）。
   来源：ideas_and_exps/reference_docs/hardware_v1.md §8。
   S31 的 SDMMC 槽位 0 默认就是这一组 IOMUX 引脚。 */
#define SD_MMC_CLK_PIN GPIO_NUM_24
#define SD_MMC_CMD_PIN GPIO_NUM_25
#define SD_MMC_D0_PIN  GPIO_NUM_20
#define SD_MMC_D1_PIN  GPIO_NUM_21
#define SD_MMC_D2_PIN  GPIO_NUM_22
#define SD_MMC_D3_PIN  GPIO_NUM_23

/* 是否已挂载（避免重复调用时二次挂载报错） */
static bool s_mounted = false;
/* 挂载得到的卡句柄（卸载时传给 esp_vfs_fat_sdcard_unmount） */
static sdmmc_card_t *s_card = NULL;

/* 列出目录内容（最多 20 项），用于验证挂载后可读 */
static void sd_card_list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir %s failed", path);
        return;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 20) {
        ESP_LOGI(TAG, "  %s", entry->d_name);
        count++;
    }
    closedir(dir);
}

esp_err_t sd_card_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    /* SDMMC 主机：S31 默认槽位 0（IOMUX 引脚 20~25），4-bit，40MHz 高速 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40MHz */

    /* SDMMC 槽位：4-bit 引脚配置（DAT0-3 与 CMD 已有外部 10k 上拉，
       故不启用内部弱上拉，保持总线特性与硬件一致） */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_MMC_CLK_PIN;
    slot_config.cmd = SD_MMC_CMD_PIN;
    slot_config.d0 = SD_MMC_D0_PIN;
    slot_config.d1 = SD_MMC_D1_PIN;
    slot_config.d2 = SD_MMC_D2_PIN;
    slot_config.d3 = SD_MMC_D3_PIN;

    /* FAT 挂载配置：不自动格式化（避免误删用户数据） */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 0,      /* 0 = 按扇区大小自动选择簇大小 */
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "filesystem mount failed (卡不存在或未格式化为 FAT?)");
        } else {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    s_card = card;

    sdmmc_card_print_info(stdout, card);

    /* 列出根目录，验证可读 */
    ESP_LOGI(TAG, "SD card mounted at %s, listing root:", SD_MOUNT_POINT);
    sd_card_list_dir(SD_MOUNT_POINT);

    s_mounted = true;
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

/* CID 字段做 FNV-1a 64 位散列：厂商/型号/序列号/生产日期都参与，
   避免只取 serial（部分卡 serial 为 0）导致不同卡撞 ID。 */
uint64_t sd_card_get_id(void)
{
    if (!s_mounted || s_card == NULL) {
        return 0;
    }

    uint64_t h = 1469598103934665603ULL;   /* FNV-1a offset basis */
    const uint8_t *p;
    size_t n;

    p = (const uint8_t *)&s_card->cid.mfg_id;
    n = sizeof(s_card->cid.mfg_id);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    p = (const uint8_t *)&s_card->cid.oem_id;
    n = sizeof(s_card->cid.oem_id);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    /* name 是定长 8 字节，可能没有 NUL，不能按字符串处理 */
    for (size_t i = 0; i < sizeof(s_card->cid.name); i++) {
        h ^= (uint8_t)s_card->cid.name[i];
        h *= 1099511628211ULL;
    }
    p = (const uint8_t *)&s_card->cid.revision;
    n = sizeof(s_card->cid.revision);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    p = (const uint8_t *)&s_card->cid.serial;
    n = sizeof(s_card->cid.serial);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    p = (const uint8_t *)&s_card->cid.date;
    n = sizeof(s_card->cid.date);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

esp_err_t sd_card_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    /* 深睡前先取消选中卡（CMD7, RCA=0）→ 卡进入 Standby 状态，显著降低卡
       在 Transfer/active 态的高待命电流（实测无卡深睡比插卡低 ~15mA，
       卡未进 Standby 是主因）。唤醒后 sd_card_init() 重新初始化
       （含重新选中）自动恢复。 */
    sdmmc_command_t cmd = {
        .opcode = MMC_SELECT_CARD,
        .arg = 0,                 /* RCA=0 = 取消选中 */
        .flags = SCF_RSP_R1,
        .timeout_ms = 1000,
    };
    esp_err_t ret = s_card->host.do_transaction(s_card->host.slot, &cmd);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card deselected (CMD7, standby)");
    } else {
        ESP_LOGW(TAG, "CMD7 deselect failed: %s", esp_err_to_name(ret));
    }

    /* 卸载 FAT 文件系统并释放 SDMMC 主机（deepsleep 前降低外设功耗） */
    ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card unmount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    sdmmc_host_deinit();
    s_mounted = false;
    s_card = NULL;
    ESP_LOGI(TAG, "SD card unmounted, sdmmc host deinit");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 原始块设备模式（只有里世界的 USB 传歌用）                            */
/* ------------------------------------------------------------------ */

/* 原始模式下的卡句柄：本模块持有，不随 FATFS 挂载/卸载释放 */
static sdmmc_card_t *s_raw_card = NULL;

esp_err_t sd_card_raw_init(sdmmc_card_t **out_card)
{
    if (out_card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_card = NULL;

    if (s_raw_card != NULL) {
        /* 同一次里世界生命周期里重复进入 USB 模式不会发生（进入后只能重启），
           这里返回已有句柄并报状态错误，避免第二次 host.init 踩到已释放的资源 */
        ESP_LOGW(TAG, "SD 卡已处于原始块设备模式，复用已有句柄");
        *out_card = s_raw_card;
        return ESP_ERR_INVALID_STATE;
    }

    /* 已挂载 FATFS 时先卸载：电脑要直接读写扇区，固件这边不能再访问同一个
       文件系统（两边同时读写会把 FAT 表写坏） */
    if (s_mounted) {
        esp_err_t err = sd_card_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "卸载 FATFS 失败，无法切换到原始模式：%s", esp_err_to_name(err));
            return err;
        }
    }

    /* 与 sd_card_init() 完全相同的总线参数（引脚/位宽/频率）：
       两处必须保持一致，否则同一张卡在两种模式下行为不同 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40MHz */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_MMC_CLK_PIN;
    slot_config.cmd = SD_MMC_CMD_PIN;
    slot_config.d0 = SD_MMC_D0_PIN;
    slot_config.d1 = SD_MMC_D1_PIN;
    slot_config.d2 = SD_MMC_D2_PIN;
    slot_config.d3 = SD_MMC_D3_PIN;

    esp_err_t ret = host.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        goto fail_host;
    }

    /* 卡结构必须放内部 RAM：sdmmc 驱动会把它当作 DMA 描述信息的来源，
       放 PSRAM 会在关缓存窗口/中断里踩到取指断言 */
    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(sdmmc_card_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (card == NULL) {
        ESP_LOGE(TAG, "为 sdmmc_card_t 分配内存失败");
        ret = ESP_ERR_NO_MEM;
        goto fail_host;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡初始化失败（未插卡/坏卡/接触不良）：%s",
                 esp_err_to_name(ret));
        heap_caps_free(card);
        goto fail_host;
    }

    s_raw_card = card;
    *out_card = card;
    ESP_LOGI(TAG, "SD 卡已切换到原始块设备模式：%s，%d 个 512 字节扇区",
             card->cid.name, card->csd.capacity);
    return ESP_OK;

fail_host:
    /* 与 IDF 示例一致：S31 的 sdmmc host 需要 deinit_p(slot) 才完整释放 */
    if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host.deinit_p(host.slot);
    } else if (host.deinit != NULL) {
        (*host.deinit)();
    }
    return ret;
}
