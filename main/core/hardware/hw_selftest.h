#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AXP2101 / PCF85063A 驱动上电自检（CONFIG_HW_SELFTEST 控制编译）
 *
 * 只做两类检查，串口打印 [PASS]/[FAIL]/[SKIP] 与一行汇总：
 *   1. **纯逻辑**：BCD 编解码、星期公式、时间字段合法性、ICC 电流编码、
 *      电压→SOC 插值、状态位解析、按键阈值编码、电量融合规则；
 *   2. **真机只读/无损**：I²C 通信、电压/电量/充放电状态、TS 电压、
 *      长按阈值寄存器、开机/关机原因、中断状态读+清；
 *      RTC 读时间/有效性、CLKOUT 已关、RAM_byte 写读回（会恢复原值）、
 *      走时 1 秒校验、寄存器转储。
 *
 * ⚠ **不测试断电功能**：不调用 axp2101_power_off()、不写 0x10、不碰电源轨
 *   （0x80-0x86 / 0x90-0x9A），也不修改充电参数（除非 CONFIG_PMU_CHARGE_CONFIG
 *   在初始化时已经下发）。自检失败只打印日志，不 abort、不影响开机。
 *
 * 调用位置：main.c 末尾（硬件都初始化完之后）。任务栈放 PSRAM（只做 I²C +
 * 延时，不写 flash）；整体耗时约 1~2 秒，在后台运行、不阻塞开机。
 */
void hw_selftest_run(void);

#ifdef __cplusplus
}
#endif
