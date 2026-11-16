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
 * | 表世界（播放器，完整功能，无 WiFi/USB 栈） | `factory` | 0x10000 | RTC 分区位 `0b00`（默认） |
 * | 里世界（WiFi对时 + USB传歌，最小流程） | `ota_0` | 0xC00000 | 表世界写 RTC 分区位 `0b01` 后启动 |
 *
 * **2026-11-16 起全面迁移到 RTC 字节**（`bootloader_components/main` 是必备件）：
 * - 启动分区**只**由 PCF85063A 的 `RAM_byte(0x03)` 决定，otadata 完全不参与；
 * - 请求里世界：校验 `ota_0` 镜像后把分区位写 `0b01`（SRAM，**0 次 flash 擦写**）；
 * - 回表世界：把分区位清 0（同样 0 次擦写），bootloader 见 `0b00` 直接选 `factory`；
 * - RTC 读失败：bootloader 直接选 `factory`（表世界是主固件），不会卡在里世界。
 *
 * ⚠ 本机 bootloader 开了 `CONFIG_BOOTLOADER_SKIP_VALIDATE_*`（开机不校验镜像，
 *   换开机速度），所以 bootloader **不会**替我们回退坏镜像——安全完全依赖
 *   `fw_switch_to_sync()` 里的全量 `esp_image_verify()`，不要绕过本模块直接改
 *   RTC 字节。
 * ⚠ otadata 分区（`otadata`@0x5FE000）仍保留在分区表里，但**不再被读写**；
 *   IDF 的 OTA API 也不再用（`esp_ota_set_boot_partition` 已从本模块移除）。
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
 * 先全量校验 `ota_0` 镜像（只读），再写 RTC 分区位并读回确认；**不重启**
 * （重启由调用方决定，便于先把播放现场落盘）。**不碰 otadata、不擦任何 flash**。
 *
 * @return ESP_OK 成功；
 *         ESP_ERR_OTA_VALIDATE_FAILED 目标槽没有有效镜像（里世界固件没烧 / 烧坏了）；
 *         ESP_ERR_NOT_FOUND 分区表里没有 ota_0（分区表没重刷）；
 *         其它 = RTC 写入/读回失败（调用方应提示"里世界固件不可用"）。
 */
esp_err_t fw_switch_to_sync(void);

/**
 * @brief 把"下次启动"切回表世界（`factory`）
 *
 * 实现上只把 RTC 的分区位清 0；bootloader 见 `0b00` 就选 `factory`。
 * **不再擦 otadata**（2026-11-16 起），所以这一步是 0 次 flash 擦写。
 *
 * 备注：正常流程已不需要它——分区位是 bootloader 消费的一次性请求，进里世界时
 * 就被清 0 了。保留它是为了"构建角色串了/手动兜底"时能显式声明回表世界。
 *
 * @return ESP_OK 成功（RTC 读不到也算成功：bootloader 同样会回 factory）；
 *         ESP_ERR_NOT_FOUND 分区表里没有 factory。
 */
esp_err_t fw_switch_to_player(void);

/* ------------------------------------------------------------------ */
/* 跨复位启动标志（PCF85063A RAM_byte 0x03 的位域）                     */
/* ------------------------------------------------------------------ */
/*
 * 该字节由 PMU 的 RTCLDO 供电，跨 SoC 复位、跨固件（表世界/里世界）保持。
 * 布局（bootloader 与 App 必须一致，改一处必须改两处）：
 *
 *   bit1:0  分区选择  0b00 = factory（表世界，默认）；0b01 = ota_0（里世界）
 *   bit2    充电界面  1 = 本次启动进充电界面（App 消费后立刻清 0）
 *   bit7:3  保留（恒 0，留给以后的一次性短消息）
 *
 * ⚠ bootloader 只读/清分区位，**不碰 bit2**；本模块写任何一位都是读-改-写。
 * 用它的场景：表世界"插着充电器关机"时不再 PMU 断电（断电后固件消失，
 * 限充与电量计检测都会失效），改为保存现场后置 bit2 并重启，由
 * 开机流程看到标志进充电界面。
 */

/**
 * @brief 置位"下次启动进充电界面"（bit2），不重启
 *
 * 调用方负责保存现场（播放位置/NVS）与优雅关音频，然后再 esp_restart()。
 * 表世界开机时消费该标志并立刻清零；里世界开机只清除、不进充电界面。
 *
 * @return ESP_OK 写入并读回校验通过；其它 = RTC 不可用/写失败（调用方应退回真关机）
 */
esp_err_t fw_switch_request_charge_ui(void);

/** @brief 当前是否挂着"进充电界面"标志（只读，不消费） */
bool fw_switch_charge_ui_requested(void);

/** @brief 消费"进充电界面"标志（清 bit2，其它位不动） */
void fw_switch_clear_charge_ui_request(void);

/**
 * @brief 兜底清掉全部一次性启动标志（分区位 + 充电界面位，保留 bit7:3）
 *
 * 正常流程里 bootloader 已清分区位、App 已清 bit2；本函数只用于"读到陈旧值
 * 也不能把下次复位带错"的保险（例如 bootloader 读 RTC 失败没能消费）。
 */
void fw_switch_clear_boot_flags(void);

#ifdef __cplusplus
}
#endif
