#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file battery_logger.h
 * @brief 电池电压采样日志（测试/功耗测量用，Kconfig 控制）
 *
 * 常驻任务（batt_log，Core1 低优先级）：每 CONFIG_BATTERY_LOGGER_MIN 分钟
 * 唤醒一次，在 10 秒窗口内以 1s 间隔读取 10 次电池电压，去掉一个最高值与
 * 一个最低值后求平均，把"时间戳 + 平均电压"追加写入 SD 卡文本文件
 * /sdcard/battery_log.txt（CSV；同一文件持续追加，不随开机新建）。
 * 每次开机时先写一行会话分隔行（# ===== session start =====），
 * 不同轮测试的数据段以分隔行为界；文件为空时先写一行表头。
 *
 * ⚠ 时间戳说明：本设备无 RTC/网络时钟，固件内没有墙上时间——日志时间戳
 * 采用"开机以来的运行时长"（elapsed 秒 + HH:MM:SS 双列），用于本机关机前
 * 的放电/功耗曲线分析；若需换算真实时刻，记录每次开机时刻后加偏移即可。
 *
 * ⚠ 写入频率极低（每 5 分钟几十字节），对功耗测量的影响可忽略；
 * 但会持续占用 SD 卡空间，测试完建议关闭（CONFIG_BATTERY_LOGGER_MIN=0）。
 *
 * 依赖：battery（电压）、sd_card（挂载判断 + /sdcard VFS）。
 * 需在 sd_card_init 之后调用 battery_logger_init；SD 未挂载时任务跳过写入。
 */

/**
 * @brief 初始化电压采样日志任务（幂等）
 *
 * CONFIG_BATTERY_LOGGER_MIN=0（Kconfig 默认关闭）时本函数直接返回，不建任务。
 * 任务每周期先等待一个完整间隔再首次采样（开机初期不立即写文件）。
 */
void battery_logger_init(void);

#ifdef __cplusplus
}
#endif
