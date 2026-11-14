#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hardware/audio/eq.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file bt_store.h
 * @brief 蓝牙设备配置存储（NVS，最多 5 个设备）
 *
 * 每个保存过的蓝牙设备各有一份独立配置，**优先级高于全局配置槽位**：
 *   - volume    ：音量档位（0~30，与 audio/UI 同一档位体系）。连接期间音量键/
 *                 控制中心改的是本设备的音量，断开后回到配置槽位的音量；
 *   - eq_name/eq：该设备的 EQ 预设（文件名 + 解析参数，直接入 NVS，不依赖
 *                 SD 卡上的 .eq 文件仍存在）。连接期间 DSP 用本设备的 EQ，
 *                 断开后恢复配置槽位的 EQ；
 *   - quiet_mode：安静模式开关（睡眠定时到点时：不真正断电，只关屏/关外设、
 *                 保留蓝牙连接；默认关，绑定到具体设备）。
 *
 * 另有全局的「蓝牙开关上次状态」（bt_enabled）：开机按它决定是否启动蓝牙栈。
 * 主机侧**不主动回连**任何设备（用户要求的开机行为）；但蓝牙开着时空闲态
 * 保持 page scan（可连接、不可发现），已配对耳机自己开机可以主动连回来。
 *
 * 存储布局：
 *   - NVS namespace "btdev"（≤15 字符）；
 *   - 键 "en"   ：u8，蓝牙开关记忆；
 *   - 键 "devs" ：5 × bt_device_t 的整块 blob（固定布局，删除设备时后项前移）。
 *     ⚠ 改动 bt_device_t 布局会让旧 blob 失配（init 时用 size 校验，失配直接
 *       丢弃重建，不会读到脏配置）。
 *
 * 落盘策略：
 *   - 设备增删/改名/换 EQ/切安静开关：立即写（低频操作）；
 *   - 音量：只改内存 + 置脏，由 bt_store_flush_pending() 在 LVGL 的 100ms
 *     定时器里静默 500ms 后写一次（与 config_store 的延迟落盘同款，避免拖动
 *     音量竖条时反复 nvs_commit 卡 UI，见 NVS_WRITE_BUDGET.md）。
 *
 * 线程安全：所有接口可在任意任务调用（内部用临界区保护内存数组；NVS 写
 * 在临界区外）。⚠ 写 flash 的调用方栈必须在内部 RAM：bt_store_flush_pending
 * 只应由 LVGL 任务/按键任务等内部栈上下文调用（与 config_store_flush_pending
 * 同一约束）。
 */

/** 最多保存的蓝牙设备数（超出后配对新设备会提示先删除一个） */
#define BT_STORE_MAX_DEVICES  5

/** 设备名最大长度（含结尾 '\0'；蓝牙 GAP 名字最长 248 字节，这里只存显示用短名） */
#define BT_STORE_NAME_MAX     64

/** 单个蓝牙设备的完整配置（NVS blob 直存，字段布局固定） */
typedef struct {
    bool        valid;                     /* 该槽位是否保存过设备 */
    uint8_t     bda[6];                    /* 蓝牙地址（大端，与 esp_bd_addr_t 同序） */
    char        name[BT_STORE_NAME_MAX];   /* 设备名（UI 显示用） */
    uint8_t     volume;                    /* 音量档位 0~30（设备独立记忆） */
    bool        eq_valid;                  /* 是否带该设备的 EQ */
    char        eq_name[EQ_FILE_NAME_MAX]; /* .eq 文件名（eq_valid 时有效） */
    eq_preset_t eq;                        /* 解析后的 EQ 参数（eq_valid 时有效） */
    bool        quiet_mode;                /* 安静模式（睡眠定时不断电、只留蓝牙） */
} bt_device_t;

/** 初始化（打开 NVS 并加载设备表 + 蓝牙开关记忆）；可重复调用 */
void bt_store_init(void);

/* ---------------- 全局蓝牙开关记忆 ---------------- */

/** @brief 上次的蓝牙开关状态（true = 开机应启动蓝牙栈；默认关） */
bool bt_store_get_enabled(void);

/** @brief 记下蓝牙开关状态并立即写 NVS（切换开关时调用） */
void bt_store_set_enabled(bool on);

/* ---------------- 设备表 ---------------- */

/** @brief 已保存的设备数（0~BT_STORE_MAX_DEVICES） */
int bt_store_count(void);

/**
 * @brief 读取指定槽位的设备配置（越界/空槽返回 false）
 *
 * @param idx 0 起的槽位下标
 * @param out 输出（可为 NULL，仅查询是否有效）
 */
bool bt_store_get(int idx, bt_device_t *out);

/** @brief 按地址查找设备，返回槽位下标；未找到返回 -1 */
int bt_store_find(const uint8_t bda[6]);

/**
 * @brief 新增一个设备（同地址已存在时只更新名字，不重复添加）
 *
 * @param bda    设备地址（6 字节）
 * @param name   设备名（NULL/空串用 "蓝牙设备" 兜底）
 * @param volume 初始音量（0~30）
 * @return 槽位下标；设备表已满（且是新设备）返回 -1
 */
int bt_store_add(const uint8_t bda[6], const char *name, uint8_t volume);

/** @brief 更新设备名（连接后拿到更完整的名字时用），立即写 NVS */
void bt_store_update_name(int idx, const char *name);

/** @brief 设置设备音量：**只改内存 + 置脏**，落盘由 flush_pending 完成 */
void bt_store_set_volume(int idx, uint8_t vol);

/** @brief 设置设备 EQ（name/preset 为 NULL 或 band_count=0 = 关闭），立即写 NVS */
void bt_store_set_eq(int idx, const char *name, const eq_preset_t *preset);

/** @brief 设置安静模式开关，立即写 NVS */
void bt_store_set_quiet(int idx, bool on);

/** @brief 删除设备（后项前移），立即写 NVS */
void bt_store_remove(int idx);

/**
 * @brief 把延迟落盘的音量写入 NVS
 *
 * @param force true=立即写（断开/切设备/关机等必须落盘的时刻）；
 *              false=距最后一次修改静默 ≥500ms 才写（100ms 定时器保底）
 */
void bt_store_flush_pending(bool force);

#ifdef __cplusplus
}
#endif
