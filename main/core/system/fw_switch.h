#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fw_switch.h
 * @brief 双固件（播放器 / WiFi对时）切换与跨固件结果交接
 *
 * 本工程在 flash 里放**两个 app**（见 `partitions.csv` 与
 * `ideas_and_exps/DUAL_APP_TIME_SYNC.md`）：
 *
 * | 角色 | 分区 | 偏移 | 谁启动它 |
 * |---|---|---|---|
 * | 播放器（完整功能，无 WiFi 协议栈） | `factory` | 0x10000 | 默认启动（otadata 空） |
 * | 对时固件（最小流程，含 WiFi） | `ota_0` | 0x310000 | 播放器写 otadata 后启动 |
 *
 * 用的是 **IDF 标准 bootloader 的 OTA 槽机制**，不需要自定义 bootloader：
 * - `esp_ota_set_boot_partition(ota_0)` 会先**校验目标镜像**（未烧录/镜像坏 →
 *   `ESP_ERR_OTA_VALIDATE_FAILED`），再把"下次启动 ota_0"写进 otadata；
 * - `esp_ota_set_boot_partition(factory)` 等价于**擦掉 otadata**（此后任何复位
 *   都回播放器）——对时固件开机第一件事就是做这件事，所以它中途崩溃/断电/
 *   看门狗复位都不会把设备卡在对时固件里。
 *
 * ⚠ 本机 bootloader 开了 `CONFIG_BOOTLOADER_SKIP_VALIDATE_*`（开机不校验镜像，
 *   换开机速度），所以 bootloader **不会**替我们回退坏镜像——安全完全依赖
 *   "先擦 otadata 再干活"这条纪律与本模块的镜像校验，不要绕过 `fw_switch_*`
 *   直接改 otadata。
 */

/** @brief 当前固件角色：true = 对时固件（`CONFIG_FIRMWARE_ROLE_SYNC=y`） */
bool fw_role_is_sync(void);

/** @brief 角色名（"player" / "sync"），只用于日志 */
const char *fw_role_name(void);

/**
 * @brief 正在运行的 app 分区标签（`factory` / `ota_0`）
 *
 * 启动日志与"关于"页用。分区找不到（理论上不会）时返回 `"?"`。
 */
const char *fw_running_partition_label(void);

/**
 * @brief 把"下次启动"切到对时固件（`ota_0`）——播放器侧调用
 *
 * 只写 otadata，**不重启**（重启由调用方决定，便于先把播放现场落盘）。
 *
 * @return ESP_OK 成功；
 *         ESP_ERR_OTA_VALIDATE_FAILED 目标槽没有有效镜像（对时固件没烧 / 烧坏了）；
 *         ESP_ERR_NOT_FOUND 分区表里没有 ota_0（分区表没重刷）。
 */
esp_err_t fw_switch_to_sync(void);

/**
 * @brief 把"下次启动"切回播放器（`factory`）——对时固件开机第一件事
 *
 * 实现上就是把 otadata 整块擦掉，所以之后任何复位/断电都回播放器。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 分区表里没有 factory/otadata。
 */
esp_err_t fw_switch_to_player(void);

/* ------------------------------------------------------------------ */
/* 跨固件结果交接（NVS，命名空间 "tsync"）                              */
/* ------------------------------------------------------------------ */

/* 结果文本上限（含 NUL）。够放"对时失败：找不到该 WiFi，请检查名称与信号。" */
#define FW_SYNC_RESULT_MAX  96

/**
 * @brief 对时固件写一条结果文本，供播放器下次开机 Toast 展示
 *
 * 为什么传文本而不是错误码：播放器固件里**不能**引用 `wifi_time_sync.c`
 * 的任何符号（那会把 WiFi 协议栈拉进播放器镜像），所以错误码→文案的映射
 * 留在对时固件侧，播放器只做"读出来弹个 Toast"。
 */
void fw_sync_result_set_text(const char *text);

/**
 * @brief 取出（并清除）上一条对时结果文本
 *
 * @param buf 输出缓冲（建议 `FW_SYNC_RESULT_MAX`）
 * @param len 缓冲长度
 * @return true 取到了（buf 已填好）；false 没有记录
 */
bool fw_sync_result_take_text(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
