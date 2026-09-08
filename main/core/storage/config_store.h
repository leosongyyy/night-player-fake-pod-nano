#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hardware/audio/eq.h"

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
 *   - 开机默认开启睡眠定时 sleep_def_on（bool，默认关）：开机自动用默认时长
 *     开启睡眠定时器（在 main.c 的 sleep_timer_init 之后应用）；开启时进入
 *     主界面会弹 Toast 提醒"定时已开启"（boot_flow，见 FUTURE_PLAN 第 27 点）
 *   - 睡眠定时清醒感知 sleep_awake（bool，默认开）：睡眠定时开启期间，切歌/
 *     播放控制/点歌等用户动作按 NightPlayer 算法折算加时（见 core/utils/
 *     sleep_timer；音量调节不再加时）
 *   - 自动息屏超时 screen_off_s（秒，默认 30）：亮屏状态无操作（按键/触摸）
 *     超过该时长后自动息屏（见 core/utils/power_service），设置页可调
 *   - 开发者选项（设置菜单 → 关于 → 开发者选项，均为 NVS 运行时开关，
 *     开机读取决定是否启动相关任务/创建调试对象，修改需重启生效）：
 *     - 电池电压采样日志 battery_log（bool，默认关）+ 采样间隔
 *       batt_log_min（分钟，默认 5，可调 1/2/3/5/10，见 core/utils/battery_logger）
 *     - 触摸测试白点 touch_test（bool，默认关，见 core/ui/touch_test）
 *   - 关机前播放状态 was_playing（bool，默认 true）：伪关机（普通/A 键长按）
 *     前记录"当时是否在播放"，开机按记忆恢复——关机前在播放则自动续播，
 *     已暂停/停止则只恢复现场不自动播放（避免开机突然出声）
 *   - EQ 预设选择 eq_name + eq_data（.eq 文件名 + 解析参数，无 EQ 时名字为空：
 *     用户在播放设置页第 7 格选择 /sdcard/.musicfx 下的 EQ 文件后，把解析
 *     得到的参数数据一并存入 NVS，开机直接恢复 DSP 状态，不依赖 SD 卡上
 *     的 .eq 文件仍存在，见 FUTURE_PLAN 第 10 点 / core/hardware/audio/eq）
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
#define CONFIG_STORE_BRIGHTNESS_MIN     16u  /* 亮度下限（0~255 总量程）：防止屏幕完全看不清 */
#define CONFIG_STORE_LOOP_DEFAULT       2u   /* PLAYER_LOOP_LIST */
#define CONFIG_STORE_SLEEP_DEFAULT_MIN  45u
#define CONFIG_STORE_SLEEP_DEF_ON_DEFAULT 0u  /* 开机默认不自动开启睡眠定时 */
#define CONFIG_STORE_SLEEP_AWAKE_DEFAULT 1u   /* 睡眠清醒感知默认开启（NightPlayer 行为） */
#define CONFIG_STORE_SCREEN_OFF_DEFAULT 30u  /* 自动息屏超时默认 30 秒 */
#define CONFIG_STORE_BATT_LOG_DEFAULT     0u  /* 电池电压日志默认关（开发者选项开启） */
#define CONFIG_STORE_BATT_LOG_MIN_DEFAULT 5u  /* 日志采样间隔默认 5 分钟 */
#define CONFIG_STORE_BATT_LOG_MIN_MAX     120u /* 日志间隔上限（分钟，防越界） */
#define CONFIG_STORE_TOUCH_TEST_DEFAULT   0u  /* 触摸测试白点默认关（开发者选项开启） */

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

/** @brief 开机默认是否开启睡眠定时器（true=开机自动用默认时长开启） */
bool config_store_get_sleep_def_on(void);

/** @brief 睡眠定时清醒感知开关（true=切歌/调音量等动作按 NightPlayer 算法加时） */
bool config_store_get_sleep_awake(void);

/** @brief 自动息屏超时（秒，0=不自动息屏；默认 30） */
uint32_t config_store_get_screen_off_s(void);

/** @brief 电池电压采样日志开关（true=开机创建 batt_log 任务；默认关） */
bool config_store_get_battery_log(void);

/** @brief 电池电压采样间隔（分钟，默认 5，可调 1/2/3/5/10） */
uint8_t config_store_get_battery_log_min(void);

/** @brief 触摸测试白点开关（true=开机创建跟随手指的白色圆点；默认关） */
bool config_store_get_touch_test(void);

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

/**
 * @brief 读取当前 EQ 预设选择（.eq 文件名 + 解析参数，NVS 持久化）
 *
 * 无 EQ 记录时返回 false，name 置空串。对应 FUTURE_PLAN 第 10 点：
 * 参数数据随文件名一并入 NVS，开机恢复 DSP 无需再读 SD 卡。
 *
 * @param[out] name        .eq 文件名缓冲（EQ_FILE_NAME_MAX 字节；可 NULL）
 * @param[in]  name_size   name 缓冲大小
 * @param[out] preset_out  解析参数（可 NULL）；有记录时填充
 * @return true 当前选择了 EQ；false 无 EQ（name 置空）
 */
bool config_store_get_eq(char *name, size_t name_size, eq_preset_t *preset_out);

/**
 * @brief 设置当前 EQ 预设选择并持久化（触发变更回调，设置页 EQ 格同步）
 *
 * @param name   .eq 文件名（不含路径，长度须 < EQ_FILE_NAME_MAX）
 * @param preset 解析参数（band_count 有效）；name 或 preset 为 NULL 时清除
 *               （无 EQ）
 */
void config_store_set_eq(const char *name, const eq_preset_t *preset);

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

/** @brief 设置开机默认开启睡眠定时器并持久化 */
void config_store_set_sleep_def_on(bool on);

/** @brief 设置睡眠清醒感知开关并持久化 */
void config_store_set_sleep_awake(bool on);

/** @brief 设置自动息屏超时并持久化（秒，自动收敛到 5~3600，0=不自动息屏） */
void config_store_set_screen_off_s(uint32_t seconds);

/** @brief 设置电池电压日志开关并持久化（开发者选项；开机检查，重启生效，
 *          仅持久化不通知回调——无 UI 需要实时同步） */
void config_store_set_battery_log(bool on);

/** @brief 设置电池电压采样间隔并持久化（分钟，自动收敛到 1~120；
 *          与 battery_log 同属开发者选项，重启生效） */
void config_store_set_battery_log_min(uint8_t minutes);

/** @brief 设置触摸测试白点开关并持久化（开发者选项；开机检查，重启生效，
 *          仅持久化不通知回调） */
void config_store_set_touch_test(bool on);

/** @brief 设置关机前播放状态并持久化（由 player_controller 在伪关机保存
 *          播放位置时一并调用；UI 无展示，不通知回调） */
void config_store_set_was_playing(bool playing);

/** @brief 保存上次播放位置并持久化（由 player_controller 调用） */
void config_store_set_last_song(uint32_t global, uint32_t pos_s);

/** @brief 清除"上次播放位置"与"随机播放位置"记录（failsafe 界面"重置歌曲位置"
 *          按钮用）：内存标志置无效并写回 NVS（valid 标志=0），下次开机视为
 *          无位置记录 → 从第一首顺序播放 / 随机模式重新生成随机起点。
 *          不清除音量/循环模式等其它用户配置。 */
void config_store_reset_position(void);

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
