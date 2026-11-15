#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"   /* sdmmc_card_t：原始块设备模式要把卡句柄交出去 */

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

/** @brief 以**原始块设备**方式初始化 SDMMC（不挂载 FATFS）
 *
 * 只给「里世界」固件的 USB 传歌用（core/system/usb_msc.c）：电脑要直接读写
 * 卡的扇区，固件这边就不能再挂 FATFS。顺序：
 *   1. 若当前已挂载 FATFS，先 sd_card_deinit() 卸载；
 *   2. 按与 sd_card_init() **完全相同**的引脚/总线参数初始化 SDMMC 主机与卡；
 *   3. 返回卡句柄，之后由调用方（TinyUSB MSC）独占访问。
 *
 * ⚠ 调用后不要再使用任何 /sdcard 下的文件接口（会访问到未挂载的 FATFS）。
 * ⚠ 本函数不注册文件系统、不打印目录；卡句柄由本模块持有，进程重启前不释放。
 *
 * @param out_card 输出卡句柄（成功时非 NULL）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已处于 raw 模式；
 *         其余为 SDMMC 主机/卡初始化错误
 */
esp_err_t sd_card_raw_init(sdmmc_card_t **out_card);

#ifdef __cplusplus
}
#endif
