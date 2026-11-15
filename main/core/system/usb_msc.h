#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file usb_msc.h
 * @brief USB 传歌：把 SD 卡作为 USB Mass Storage（U 盘）暴露给电脑
 *
 * 2026-11 新增，只编进**里世界**固件（矩阵 role=inner，见
 * ideas_and_exps/INNER_WORLD.md）。播放器固件一个符号都不引用它。
 *
 * 为什么必须放在独立固件里：
 *   - USB 设备栈（TinyUSB + MSC）要常驻一部分内部 RAM，而且把 SD 卡当
 *     U 盘时**不能**同时挂 FATFS（两边抢同一张卡），与播放器"一直在读卡
 *     放歌"的模型冲突；
 *   - 里世界本来就是一个"停下来做维护"的固件（对时/传歌），退出靠重启，
 *     天然满足"传歌时独占 SD 卡"。
 *
 * 使用顺序（整个里世界生命周期只调用一次）：
 *   1. 若播放器/里世界已经挂载了 FATFS → 先 sd_card_deinit() 卸载；
 *   2. usb_msc_start() 重新以**原始块设备**方式初始化 SDMMC（不挂 FATFS）
 *      并安装 TinyUSB MSC；
 *   3. 之后**不要**再调用任何 SD 卡文件系统接口（卡已交给电脑）；
 *   4. 退出方式是 esp_restart()（otadata 已被擦 → 回表世界）。
 *
 * ⚠ 一个容易踩的硬件事实：FakePodNano Pro 的 USB-C 正反插接的是**两个不同
 *   控制器**——默认方向是 USB 串口（烧录/日志），翻面才是 USB OTG（本模块
 *   与官方固件的 UAC/MSC 都走这面）。固件无法同时驱动两面，所以界面必须
 *   提示"若电脑未识别，请把插头翻面"。
 */

/** usb_msc_start() 的结果 */
typedef enum {
    USB_MSC_OK = 0,        /* 已启动：电脑应能识别为可移动磁盘 */
    USB_MSC_ERR_STATE,     /* 已经启动过（重复调用） */
    USB_MSC_ERR_NO_SD,     /* SD 卡初始化失败：未插卡 / 卡损坏 / 接触不良 */
    USB_MSC_ERR_NO_MEM,    /* 卡结构或 TinyUSB 缓冲分配失败 */
    USB_MSC_ERR_USB,       /* TinyUSB 设备栈安装失败 */
} usb_msc_result_t;

/**
 * @brief 启动 USB 传歌模式（阻塞，约几百 ms；只成功一次）
 *
 * 内部顺序：卸载已有 FATFS → 原始 SDMMC 初始化 → 注册 MSC 存储 → 安装
 * TinyUSB 设备栈。返回 USB_MSC_OK 后，UI 只需要显示状态并等用户重启。
 *
 * ⚠ 必须在 LVGL 锁外调用（会做 SD 初始化与 USB 安装，耗时且可能打日志）。
 * @return 结果码
 */
usb_msc_result_t usb_msc_start(void);

/** @brief 是否已经成功进入 USB 传歌模式（此后 SD 卡被电脑独占） */
bool usb_msc_is_active(void);

/** @brief 电脑是否已经挂载本机（TinyUSB 的 tud_mounted()） */
bool usb_msc_host_mounted(void);

/** @brief SD 卡容量（字节；未进入模式/无卡时返回 0） */
uint64_t usb_msc_capacity_bytes(void);

/** @brief 结果码对应的中文说明（UI 直接显示） */
const char *usb_msc_result_text(usb_msc_result_t result);

#ifdef __cplusplus
}
#endif
