#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config_store.h
 * @brief 用户配置存储（NVS 封装）
 *
 * 把用户可调配置持久化到 NVS（partitions.csv 的 nvs 分区，24KB），开机
 * 自动恢复。当前管理的配置项：
 *   - 音量 volume（0~30 档：0=静音，1~30 线性映射 20%~80%，默认 20）
 *   - 亮度 brightness（0~255，默认 0xFF 最亮，与 DISPLAY_BRIGHTNESS_DEFAULT 一致）
 *   - 循环模式 loop_mode（uint8，取值对应 player_loop_mode_t，默认列表循环）
 *   - 随机播放开关 shuffle（默认关闭=顺序播放；UI 切换并持久化，
 *     随机算法已接入播放引擎，见 core/player/shuffle_player）
 *   - 随机播放位置 shuffle_pos（uint64，无限确定性随机序列的逻辑位置），
 *     由 player_controller 在切歌/暂停/关机时保存，开机恢复随机序列现场
 *   - 睡眠定时的默认时长 sleep_default_min（分钟，默认 45）
 *   - 自动息屏超时 screen_off_s（秒，默认 30）：亮屏状态无操作（按键/触摸）
 *     超过该时长后自动息屏（见 core/utils/screen_sleep），设置页可调
 *   - 关机前播放状态 was_playing（bool，默认 true）：伪关机（普通/A 键长按）
 *     前记录"当时是否在播放"，开机按记忆恢复——关机前在播放则自动续播，
 *     已暂停/停止则只恢复现场不自动播放（避免开机突然出声）
 *   - 上次播放位置（歌曲全局下标 + 秒），由 player_controller 在暂停/切歌/
 *     关机时保存，开机读取跳转恢复
 *
 * ⚠ NightPlayer 原版机制：只记忆"当前歌曲"，不记忆秒数位置——pos_s 恒为 0，
 * 开机从歌曲开头开始（避免恢复播放位置带来的复杂度/体验差异）。
 *
 * 设计要点：
 *   - 所有 set_xxx 立即写 NVS 并通知已注册的变更回调（config_store_register_cb），
 *     UI 通过回调同步各页面的控件显示（例如通知栏亮度滑块与设置页共用配置源）；
 *   - 本模块只负责"存储 + 通知"，不直接操作硬件（音量/亮度/循环模式的
 *     "立即生效"由调用方在 LVGL 上下文中执行，如 display_set_brightness
 *     必须在持锁时调用）；
 *   - NVS 读写失败不阻塞启动：内存值保持默认，仅打日志。
 */

/** 各配置项默认值 */
#define CONFIG_STORE_VOLUME_DEFAULT     20u  /* 音量档位默认（NVS 无设置时） */
#define CONFIG_STORE_VOLUME_MIN         0u   /* 0 = 静音 */
#define CONFIG_STORE_VOLUME_MAX         30u  /* 上限：1~30 线性映射 20%~80% */
#define CONFIG_STORE_BRIGHTNESS_DEFAULT 0xFFu
#define CONFIG_STORE_LOOP_DEFAULT       2u   /* PLAYER_LOOP_LIST */
#define CONFIG_STORE_SLEEP_DEFAULT_MIN  45u
#define CONFIG_STORE_SCREEN_OFF_DEFAULT 30u  /* 自动息屏超时默认 30 秒 */

/** 配置变更回调（在调用 set_xxx 的线程上下文中同步执行） */
typedef void (*config_store_cb_t)(void *ctx);

/**
 * @brief 初始化配置存储：打开 NVS 并加载全部配置到内存
 *
 * 内部调用 nvs_flash_init()（首次自动格式化 nvs 分区），可在硬件初始化
 * 前调用。可重复调用，已初始化时直接返回。
 */
void config_store_init(void);

/* ---------- 读取（内存值，开机时已从 NVS 加载） ---------- */

/** @brief 当前音量档位（0~30，0=静音） */
uint8_t config_store_get_volume(void);

/** @brief 当前亮度（0~255） */
uint8_t config_store_get_brightness(void);

/** @brief 当前循环模式（uint8，取值对应 player_loop_mode_t） */
uint8_t config_store_get_loop_mode(void);

/** @brief 随机播放开关（true=随机播放，false=顺序播放） */
bool config_store_get_shuffle(void);

/**
 * @brief 读取保存的随机播放位置（无限随机序列的逻辑位置）
 *
 * @param[out] pos 随机位置（uint64）
 * @return true 有记录；false 无记录（首次开启随机 / 从未保存过）
 */
bool config_store_get_shuffle_pos(uint64_t *pos);

/** @brief 睡眠定时的默认时长（分钟，0=无默认） */
uint32_t config_store_get_sleep_default_min(void);

/** @brief 自动息屏超时（秒，0=不自动息屏；默认 30） */
uint32_t config_store_get_screen_off_s(void);

/** @brief 关机前是否在播放（true=播放中，开机自动续播；false=已暂停/停止，
 *          开机只恢复现场不自动播放）。无 NVS 记录时默认 true（保持旧行为）。 */
bool config_store_get_was_playing(void);

/**
 * @brief 读取上次播放位置
 * @param[out] global 上次播放歌曲全局下标
 * @param[out] pos_s  上次播放位置（秒）
 * @return true 有记录（NVS 中存在有效的上次播放记录）；false 无记录
 */
bool config_store_get_last_song(uint32_t *global, uint32_t *pos_s);

/* ---------- 写入（更新内存 + 写 NVS + 通知变更回调） ---------- */

/** @brief 设置音量档位并持久化（0~30，0=静音，越界自动钳位） */
void config_store_set_volume(uint8_t vol);

/** @brief 设置亮度并持久化（0~255） */
void config_store_set_brightness(uint8_t brightness);

/** @brief 设置循环模式并持久化（uint8，对应 player_loop_mode_t） */
void config_store_set_loop_mode(uint8_t mode);

/** @brief 设置随机播放开关并持久化 */
void config_store_set_shuffle(bool on);

/** @brief 保存随机播放位置并持久化（由 player_controller 调用） */
void config_store_set_shuffle_pos(uint64_t pos);

/** @brief 设置睡眠定时的默认时长并持久化（分钟，0=关闭） */
void config_store_set_sleep_default_min(uint32_t minutes);

/** @brief 设置自动息屏超时并持久化（秒，自动收敛到 5~3600，0=不自动息屏） */
void config_store_set_screen_off_s(uint32_t seconds);

/** @brief 设置关机前播放状态并持久化（由 player_controller 在伪关机保存
 *          播放位置时一并调用；UI 无展示，不通知回调） */
void config_store_set_was_playing(bool playing);

/** @brief 保存上次播放位置并持久化（由 player_controller 调用） */
void config_store_set_last_song(uint32_t global, uint32_t pos_s);

/**
 * @brief 注册配置变更回调（最多 4 个，重复注册会覆盖同名项）
 *
 * 回调在调用 set_xxx 的线程上下文中同步执行。UI 注册方需保证回调运行在
 * LVGL 任务上下文（持锁），例如滑块/按钮事件回调内触发 set_xxx 的场景。
 *
 * @param cb  回调函数（NULL 忽略）
 * @param ctx 透传给回调的用户上下文
 */
void config_store_register_cb(config_store_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
