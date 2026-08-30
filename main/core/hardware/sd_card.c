#include "core/hardware/sd_card.h"

#include <dirent.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_defs.h"
#include "sd_protocol_types.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

/* SD_MMC 引脚（FakePodNano 板载 SD 卡槽，4-bit 模式）。
   DAT0-3 与 CMD 经 10k 上拉到 3V3，CLK 直连 MCU。 */
#define SD_MMC_CLK_PIN GPIO_NUM_16
#define SD_MMC_CMD_PIN GPIO_NUM_17
#define SD_MMC_D0_PIN  GPIO_NUM_15
#define SD_MMC_D1_PIN  GPIO_NUM_14
#define SD_MMC_D2_PIN  GPIO_NUM_21
#define SD_MMC_D3_PIN  GPIO_NUM_18

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

    /* SDMMC 主机：slot 1，4-bit，40MHz 高速 */
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
