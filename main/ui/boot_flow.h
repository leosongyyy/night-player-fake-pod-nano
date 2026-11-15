#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机封面（splash）：显示默认封面，让屏幕尽早亮起来
 *
 * 在 app_main **早期**调用：display_init() + lvgl_port_init() 之后、
 * touch/audio/pldata/button/SD 初始化之前，**必须持有 LVGL 锁**。
 * 本函数只创建"全屏黑底 + 默认封面（DEFAULT_COVER_IMG）"，调用方随后
 * lv_refr_now() 提交首帧、再 display_set_brightness() 点亮（防白屏顺序不变）。
 *
 * 之后 boot_flow_run() 的任一分支都会删除本封面容器并替换为对应界面
 * （低电 / 无卡 / 扫描中 / 无音乐 / 扫描失败 / 常规主界面），无需额外清理。
 */
void boot_splash_show(void);

/**
 * @brief 开机流程控制器（S31：SD 卡 / 播放列表缓存 双重检查）
 *
 * 在 lvgl_port_init() 之后、持有 LVGL 锁的情况下调用。本函数**只创建首屏
 * 并启动相关定时器/后台扫描任务，然后立即返回**（不阻塞），后续界面切换
 * 由 LVGL 定时器 / 按钮事件 / 扫描任务完成标志驱动。
 *
 * 开机决策顺序（详见 boot_flow.c）：
 *  1. **低电拒启**：电量 < 2% 且未在充电 → 只显示「电量过低，请充电」，
 *     5 秒后由 AXP2101 直接断电（**不停在此屏**）。
 *  2. 未插入 SD 卡 → 提示无卡 + 提供"清除扫描数据"（failsafe）按钮（停在此屏）。
 *  3. 有卡时核对播放列表缓存：
 *     - 无有效缓存 / 缓存元信息缺失 → 自动扫描；
 *     - 缓存记录的 SD 卡 ID 与当前卡不符 → 清空旧播放位置并自动扫描；
 *     - 曲库指纹（音频路径 + .pl 的大小/mtime）或文件数变化 → 自动扫描；
 *     - 全部一致 → 直接进入常规界面。
 *  4. 自动扫描后无音乐 → 显示"未发现音乐"；扫描/写入失败 → 显示"扫描失败"，
 *     两种异常都停在此屏，不进入空列表主界面。
 *
 * ⚠ 插充电器自动开机的「充电专用界面」不走本函数，而是
 *   boot_flow_run_charger_mode()（见下）。
 */
void boot_flow_run(void);

/**
 * @brief 充电专用界面（插充电器自动开机时使用）
 *
 * 由 main.c 在 boot_reason_is_charger_boot() 为 true 时调用（**持 LVGL 锁**），
 * 之后 app_main 直接返回：不初始化 RTC/音频/播放列表/SD/按键/电源服务，
 * 设备只显示"正在充电 + 当前电量"。
 *
 * 行为：
 *   · 每 1 秒刷新电量与充电状态（AXP2101 0x01[2:0] DONE → "已充满"）；
 *   · **拔掉充电器**：宽限 1.5 秒后显示"正在关机..."并写 0x10[0]=1 由 PMU
 *     真断电（本状态没有播放/SD/NVS 写入，断电安全）；
 *   · **短按上键**：esp_restart() 进入正常系统——重启后主控复位原因是
 *     ESP_RST_SW，boot_reason_is_charger_boot() 自然为 false，不会再回到本界面。
 */
void boot_flow_run_charger_mode(void);

#ifdef __cplusplus
}
#endif
