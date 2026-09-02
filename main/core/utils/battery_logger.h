#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file battery_logger.h
 * @brief 电池电压采样日志（测试/续航测量用，开发者选项运行时控制）
 *
 * 常驻任务（batt_log，Core1 低优先级）：每 N 分钟唤醒一次，在 10 秒窗口内
 * 以 1s 间隔读取 10 次电池电压，去掉一个最高值与一个最低值后求平均，把
 * "时间戳 + 平均电压"追加写入 SD 卡文本文件 /sdcard/battery_log.txt
 * （CSV；同一文件持续追加，不随开机新建）。每次开机时先写一行会话分隔行
 * （# ===== session start =====），不同轮测试的数据段以分隔行为界；
 * 文件为空时先写一行表头。
 *
 * 开关与间隔均为运行时配置（NVS，config_store 持久化，默认关闭）：
 *   - 开关 battery_log：设置菜单 → 关于 → 开发者选项；开机调用
 *     battery_logger_init 时检查，关闭则不创建任务（修改需重启生效）；
 *   - 间隔 batt_log_min：开发者选项档位 1/2/3/5/10 分钟（默认 5），
 *     同样开机读取生效。
 *
 * ⚠ 时间戳说明：本设备无 RTC/网络时钟，固件内没有墙上时间——日志时间戳
 * 采用"开机以来的运行时长"（elapsed 秒 + HH:MM:SS 双列），用于本机关机前
 * 的放电/功耗曲线分析；若需换算真实时刻，记录每次开机时刻后加偏移即可。
 *
 * ⚠ 写入频率极低（每 5 分钟几十字节），对功耗测量的影响可忽略；
 * 但会持续占用 SD 卡空间，测试完建议在开发者选项中关闭。
 *
 * 依赖：battery（电压）、sd_card（挂载判断 + /sdcard VFS）。
 * 需在 config_store_init、sd_card_init 之后调用；SD 未挂载时任务跳过写入。
 */

/**
 * @brief 初始化电压采样日志任务（幂等）
 *
 * 读取 config_store：battery_log 为 false（默认关）时直接返回，不建任务；
 * 开启时按 batt_log_min 的间隔创建任务。任务每周期先等待一个完整间隔再
 * 首次采样（开机初期不立即写文件）。
 */
void battery_logger_init(void);

#ifdef __cplusplus
}
#endif
