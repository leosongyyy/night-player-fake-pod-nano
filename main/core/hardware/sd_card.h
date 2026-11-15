#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SD 卡驱动（SDMMC 4-bit 模式 + FAT 文件系统挂载）
 *
 * 硬件连接（FakePodNano）：
 *  - 使用 ESP32-S3 SDMMC 外设（slot 1），4-bit 模式
 *  - DAT0/1/2/3 与 CMD 经 10k 上拉到 3V3，CLK 直连 MCU
 *  - 供电端有电容滤波（标准去耦）
 *
 * 挂载成功后文件系统位于 SD_MOUNT_POINT（FAT）。
 */

/** SD 卡挂载点（后续音乐播放通过此路径访问文件） */
#define SD_MOUNT_POINT "/sdcard"

/**
 * @brief 初始化 SD 卡（探测 → 4-bit 初始化 → 挂载 FAT → 打印卡信息 + 列根目录）
 *
 * @return ESP_OK 成功；失败返回错误码（不阻塞其它外设启动）
 */
esp_err_t sd_card_init(void);

/**
 * @brief 查询 SD 卡是否已成功挂载
 *
 * @return true 已挂载（sd_card_init 成功且未卸载）
 */
bool sd_card_is_mounted(void);

/**
 * @brief 读取 SD 卡身份（CID 派生的 64 位散列）
 *
 * 用 CID 的厂商/型号/序列号/生产日期等字段做 FNV-1a 64 位散列，
 * 供播放列表缓存判断"是否换了一张卡"。未挂载时返回 0。
 *
 * @return 64 位卡身份；未挂载返回 0
 */
uint64_t sd_card_get_id(void);

/**
 * @brief 卸载 SD 卡（FAT 文件系统 + SDMMC 主机释放），供深度休眠前调用
 *
 * @return ESP_OK 成功
 */
esp_err_t sd_card_deinit(void);

#ifdef __cplusplus
}
#endif
