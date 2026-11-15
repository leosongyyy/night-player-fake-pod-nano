#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file fw_switch.h
 * @brief 双固件（表世界 / 里世界）切换
 *
 * 本工程在 flash 里放**两个 app**（见 `partitions.csv` 与
 * `ideas_and_exps/INNER_WORLD.md`）：
 *
 * | 角色 | 分区 | 偏移 | 谁启动它 |
 * |---|---|---|---|
 * | 表世界（播放器，完整功能，无 WiFi/USB 栈） | `factory` | 0x10000 | 默认启动（otadata 空） |
 * | 里世界（WiFi对时 + USB传歌，最小流程） | `ota_0` | 0x310000 | 表世界写 otadata 后启动 |
 *
 * 用的是 **IDF 标准 bootloader 的 OTA 槽机制**，不需要自定义 bootloader：
 * - `esp_ota_set_boot_partition(ota_0)` 会先**校验目标镜像**（未烧录/镜像坏 →
 *   `ESP_ERR_OTA_VALIDATE_FAILED`），再把"下次启动 ota_0"写进 otadata；
 * - `esp_ota_set_boot_partition(factory)` 等价于**擦掉 otadata**（此后任何复位
 *   都回表世界）——里世界开机第一件事就是做这件事。擦 otadata **只影响下次
 *   启动**，当前运行的里世界照常工作，所以「回到表世界」按钮只是 esp_restart()。
 *
 * ⚠ 本机 bootloader 开了 `CONFIG_BOOTLOADER_SKIP_VALIDATE_*`（开机不校验镜像，
 *   换开机速度），所以 bootloader **不会**替我们回退坏镜像——安全完全依赖
 *   "先擦 otadata 再干活"这条纪律与本模块的镜像校验，不要绕过 `fw_switch_*`
 *   直接改 otadata。
 */

/** @brief 当前固件角色：true = 里世界固件（`CONFIG_FIRMWARE_ROLE_SYNC=y`） */
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
 * @brief 把"下次启动"切到里世界固件（`ota_0`）——表世界侧调用
 *
 * 只写 otadata，**不重启**（重启由调用方决定，便于先把播放现场落盘）。
 *
 * @return ESP_OK 成功；
 *         ESP_ERR_OTA_VALIDATE_FAILED 目标槽没有有效镜像（里世界固件没烧 / 烧坏了）；
 *         ESP_ERR_NOT_FOUND 分区表里没有 ota_0（分区表没重刷）。
 */
esp_err_t fw_switch_to_sync(void);

/**
 * @brief 把"下次启动"切回表世界（`factory`）——里世界固件开机第一件事
 *
 * 实现上就是把 otadata 整块擦掉，所以之后任何复位/断电都回表世界。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 分区表里没有 factory/otadata。
 */
esp_err_t fw_switch_to_player(void);

#ifdef __cplusplus
}
#endif
