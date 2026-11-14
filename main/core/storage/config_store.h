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
 *   - 睡眠定时的默认时长 sleep_default_min（分钟，默认 45）：**只是"开启时用的
 *     档位"**，本身不代表定时已开启
 *   - 睡眠定时开机自动开启模式 sleep_auto（uint8 枚举，默认 0=总是关闭）：
 *     - 0 = 总是关闭：开机一律不自动开启定时；
 *     - 1 = 按时间段：开机时若当前时刻落在 sleep_range_start/end 小时内则开启；
 *     - 2 = 总是开启：每次开机都用默认时长开启定时。
 *     判定统一收敛在 `sleep_timer_boot_autostart_min()`（main.c 启动定时、
 *     boot_flow 弹"定时已开启"Toast 共用同一判定，见 FUTURE_PLAN 第 27 点）。
 *     ⚠ 睡眠定时的**时长/模式/时间段/清醒感知都与配置槽位互相独立**（2026-11）：
 *     全是全局键，切换 SAVE1~SAVE4 既不改变也不重新开表（定时只在用户主动
 *     操作或开机判定时才变）。
 *   - 睡眠定时时间段 sleep_range_start / sleep_range_end（小时 0~23，默认
 *     22 / 6）：仅在「按时间段」模式下生效，两者**不可相同**（相同视为非法，
 *     判定时不自动开启；UI 的滚轮弹窗在保存时就会拦下）。区间为左闭右开，
 *     支持跨零点：start < end 时是 [start, end)，start > end 时是
 *     [start, 24) ∪ [0, end)（例：22 → 6 表示 22:00~05:59）。
 *   - 睡眠定时清醒感知 sleep_awake（bool，默认开）：睡眠定时开启期间，切歌/
 *     播放控制/点歌等用户动作按 NightPlayer 算法折算加时（见 core/utils/
 *     sleep_timer；音量调节不再加时）
 *   - 自动息屏超时 screen_off_s（秒，默认 30）：亮屏状态无操作（按键/触摸）
 *     超过该时长后自动息屏（见 core/utils/power_service），设置页可调。
 *     ⚠ 全局键（screenoff），**不进配置槽位**：切换 SAVE1~SAVE4 不会改变它。
 *   - 时区 tzoff（UTC 偏移分钟数，默认 480 = UTC+8）：设置菜单 → 时间与日期
 *     → 时区设置 可改；网络对时用它把 NTP 的 UTC 换算成本地时间。全局键，
 *     不进配置槽位。
 *   - 时间制式 time24h（bool，默认 1 = 24 小时制）：设置菜单 → 时间与日期 →
 *     时间格式 可切换；状态栏/下拉栏的时钟按它格式化（12h 不显示上下午）。
 *   - 虚拟 ClickWheel click_wheel（bool，默认**开**）：长列表右下角速滑按钮
 *     是否启用（见 core/ui/click_wheel）。设置菜单 →「虚拟 ClickWheel」子页
 *     切换，三项均立即生效（无需重启）：
 *     - click_wheel     开关（默认开）
 *     - click_wheel_rev 滑动方向（bool，默认**关**=正常：顺时针向下滚；
 *                       开 = 反转：顺时针向上滚）
 *     - click_wheel_spd 速度档位（uint8 enum，默认**普通**）：把圈数模型算出
 *                       的圈数乘一个 ≤1 的系数（0.25 一档均匀变化）——
 *                       普通 ×1.00（不计算）/ 快速 ×0.75 / 极速 ×0.50；
 *                       设置菜单里点击循环切换
 *   - 开发者选项（设置菜单 → 关于 → 开发者选项，均为 NVS 运行时开关，
 *     开机读取决定是否启动相关任务/创建调试对象，修改需重启生效）：
 *     - 触摸测试白点 touch_test（bool，默认关，见 core/ui/touch_test）
 *   - 回放增益总开关 replay_gain（bool，默认**开**）：关=所有歌曲按 0dB
 *     播放（.npd 元数据里的增益值不再应用），开=按元数据增益应用。播放设置页
 *     第 8 格切换并持久化；控制器轮询到变化后重新应用到音频链路（见
 *     player_controller 的开关同步），设置页底部文字随之显示"关闭"
 *   - 电池健康保护 batt_care（bool，默认**关**）：设置菜单 →「电源选项」切换。
 *     开启时把 AXP2101 的恒压充电目标从 4.2V 降到 **4.0V**（约 75~80% 电量，
 *     减少满电存放对电芯的损耗），关闭则写回 4.2V。真正写 PMU 寄存器由
 *     battery_set_health_protection() 完成（本模块只存储）；PMU 每次上电都会
 *     回默认 CV，故 main.c 开机时按本配置重新下发。全局键，不进配置槽位。
 *   - 关机前播放状态 was_playing（bool，默认 true）：伪关机（普通/A 键长按）
 *     前记录"当时是否在播放"，开机按记忆恢复——关机前在播放则自动续播，
 *     已暂停/停止则只恢复现场不自动播放（避免开机突然出声）
 *   - EQ 预设选择 eq_name + eq_data（.eq 文件名 + 解析参数，无 EQ 时名字为空：
 *     用户在播放设置页第 4 格选择 /sdcard/.musicfx 下的 EQ 文件后，把解析
 *     得到的参数数据一并存入 NVS，开机直接恢复 DSP 状态，不依赖 SD 卡上
 *     的 .eq 文件仍存在，见 FUTURE_PLAN 第 10 点 / core/hardware/audio/eq）
 *   - 上次播放位置（歌曲全局下标 + 秒），由 player_controller 在暂停/切歌/
 *     关机时保存，开机读取跳转恢复
 *
 * ⚠ NightPlayer 原版机制：只记忆"当前歌曲"，不记忆秒数位置——pos_s 恒为 0，
 * 开机从歌曲开头开始（避免恢复播放位置带来的复杂度/体验差异）。
 *
 * ---------- 配置槽位（2026-11，NightPlayer「配置插槽」功能） ----------
 * 「不同场景用不同配置」：音量 / 循环模式 / 随机开关 + 随机位置 / EQ 预设 /
 * 当前列表与歌曲 这 6 类字段**整体**属于"当前配置"，共 4 个槽位
 * （见 config_store_slot_count）各自独立保存，另有"当前活跃槽位"记忆。
 * ⚠ **睡眠定时（时长 + 开关）不属于槽位**，全局只有一份（见上方说明）。
 *
 * ⚠ **应用侧完全无感**：上面那些 get_xxx / set_xxx 签名与语义一字不变，只是
 * 内部改成"读/写当前活跃槽位的值"。切换槽位（config_store_switch_slot）会把
 * 当前活跃槽位整份换掉，此后 get_xxx 自然返回新槽位的值，set_xxx 也自然写进
 * 新槽位——**任何调用方都不需要知道槽位的存在**，新增可切换字段也只需在
 * config_store_slot_snapshot_t 与本文件内部加一处，不必改 UI。
 *
 * 保存时机（三条路，保证"切走再切回"与"断电重启"都不丢）：
 *   1. 切换槽位前：先把当前活跃槽位存回 NVS（config_store_switch_slot 内部）；
 *      调用方（core/storage/config_slots）必须在切换前把**播放现场**同步进来
 *      （当前歌曲/随机位置/列表名，见 player_controller_sync_config）——
 *      否则存下去的是"上一次落盘的歌曲"，表现为"切走再切回还是原来那首"；
 *   2. 每次 set_xxx：增量存回当前活跃槽位（同一 blob 反复覆盖，NVS 分页复用，
 *      不会无限增长；这样"改完直接长按关机/断电"也不会丢）；
 *   3. 伪关机：player_controller 记完最后播放位置后存回当前槽位。
 * 非切换字段（亮度 / 息屏超时 / ClickWheel / 开发者选项 / 回放增益 / 睡眠
 * 定时时长与开关 / 睡眠清醒感知）**不进槽位**，全局共用一份。
 *
 * 兼容性：槽位机制**不删除**旧全局键。活跃槽位从未保存过时，音量/循环/随机/
 * 最后播放位置仍走旧键（首次升级上来的机器读到的就是既有配置）；
 * 第一次保存（关机或切换）后全局键冻结、后续以槽位为准。
 * 睡眠定时时长（sleepdef）**始终**读写旧全局键，不随槽位冻结。
 *
 * 设计要点：
 *   - 所有 set_xxx 写 NVS，并**只置一个"待通知"标志**；真正的变更回调由
 *     LVGL 任务的定时器统一派发（config_store_notify_dispatch）——因此
 *     **任何任务都可以安全调用 set_xxx**，不会在非 LVGL 任务里碰 LVGL 对象
 *     （2026-11 事故：player_ctrl 任务里 set_shuffle 直接跑 UI 回调 → 卡死
 *     lv_inv_area → task_wdt）。UI 通过回调同步各页面的控件显示（例如通知栏
 *     亮度滑块与设置页共用配置源），延迟 ≤ 派发定时器周期（100ms）；
 *   - 本模块只负责"存储 + 通知"，不直接操作硬件（音量/亮度/循环模式的
 *     "立即生效"由调用方在 LVGL 上下文中执行，如 display_set_brightness
 *     必须在持锁时调用；槽位切换的运行时应用见 core/storage/config_slots）；
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
#define CONFIG_STORE_SLEEP_AUTO_OFF_DEFAULT 0u  /* 开机默认不自动开启睡眠定时 */
#define CONFIG_STORE_SLEEP_RANGE_START_DEFAULT 22u  /* 时间段默认 22:00 */
#define CONFIG_STORE_SLEEP_RANGE_END_DEFAULT    6u  /* 时间段默认到 06:00（跨零点） */
#define CONFIG_STORE_SLEEP_AWAKE_DEFAULT 1u   /* 睡眠清醒感知默认开启（NightPlayer 行为） */
#define CONFIG_STORE_SCREEN_OFF_DEFAULT 30u  /* 自动息屏超时默认 30 秒 */
/* 时区（UTC 偏移，单位分钟）：默认 +480 = UTC+8（北京时间）。
   设置菜单 → 时间与日期 → 时区设置 可改（滚轮，15 分钟粒度）。
   ⚠ 全局键（tzoff），不进配置槽位；RTC 里存的是**本地墙钟时间**，
     网络对时按该偏移把 NTP 的 UTC 换算成本地时间后写入 RTC。 */
#define CONFIG_STORE_TZ_OFFSET_DEFAULT  480
#define CONFIG_STORE_TZ_OFFSET_MIN     (-720)   /* UTC-12:00 */
#define CONFIG_STORE_TZ_OFFSET_MAX      840     /* UTC+14:00 */
#define CONFIG_STORE_CLICK_WHEEL_DEFAULT 1u  /* 虚拟 ClickWheel 默认启用 */
#define CONFIG_STORE_CW_REVERSE_DEFAULT  0u  /* 滑动方向默认"正常"（顺时针向下滚） */
#define CONFIG_STORE_CW_SPEED_DEFAULT    0u  /* 速度档位默认"普通"（圈数不折减） */

/**
 * 虚拟 ClickWheel 速度档位（NVS 配置 clkwheel_spd）
 *
 * ⚠ 数值必须与 `core/ui/click_wheel.h` 的 `CLICK_WHEEL_SPEED_*` 一致
 *   （`ui/settings_menu.c` 里有 `_Static_assert` 兜底校验）：core/ui 不依赖
 *   config_store，两边各自定义同一组数值。
 */
typedef enum {
    CONFIG_STORE_CW_SPEED_NORMAL = 0,  /* 普通：圈数按模型原值（不计算） */
    CONFIG_STORE_CW_SPEED_FAST   = 1,  /* 快速：圈数 × 0.75 */
    CONFIG_STORE_CW_SPEED_TURBO  = 2,  /* 极速：圈数 × 0.50 */
    CONFIG_STORE_CW_SPEED_COUNT  = 3,
} config_store_cw_speed_t;

/**
 * 睡眠定时「开机自动开启」模式（NVS 键 sleepauto）
 *
 * 取代原来的布尔开关 sleep_def_on（旧键仍会在首次升级时读取一次做迁移：
 * 老值为 1 → 总是开启，0 → 总是关闭）。
 */
typedef enum {
    CONFIG_STORE_SLEEP_AUTO_OFF   = 0,  /* 总是关闭：开机不自动开启定时 */
    CONFIG_STORE_SLEEP_AUTO_RANGE = 1,  /* 按时间段：开机时在时间段内才开启 */
    CONFIG_STORE_SLEEP_AUTO_ALWAYS = 2, /* 总是开启：每次开机都开启 */
    CONFIG_STORE_SLEEP_AUTO_COUNT = 3,
} config_store_sleep_auto_t;
#define CONFIG_STORE_TOUCH_TEST_DEFAULT   0u  /* 触摸测试白点默认关（开发者选项开启） */
#define CONFIG_STORE_REPLAY_GAIN_DEFAULT  1u  /* 回放增益总开关默认开（保持既有行为） */
#define CONFIG_STORE_BATT_CARE_DEFAULT    0u  /* 电池健康保护默认关（标准 4.2V 满电） */
#define CONFIG_STORE_TIME_24H_DEFAULT     1u  /* 时间显示默认 24 小时制 */

/* ------------------------------------------------------------------ */
/* 配置槽位（SAVE1~SAVE4）                                             */
/* ------------------------------------------------------------------ */

/**
 * 配置槽位数量（NightPlayer 原版功能「配置插槽」）
 *
 * 每个槽位独立保存一份"当前配置"：音量 / 循环模式 / 随机开关 / 随机位置 /
 * 睡眠定时时长 / EQ 预设（文件名 + 参数）/ 最后播放的歌曲全局下标。
 * 另有"当前活跃槽位"索引单独记忆（不属于任何槽位）。
 *
 * ⚠ 修改本值只需改这一处：NVS 键名按下标生成（slot0~slot3），UI 列表按
 *   config_store_slot_count() 生成，无其它硬编码。
 */
#define CONFIG_STORE_SLOT_COUNT  4u

/** 槽位里的播放列表名缓冲大小（UI 显示 SAVE1 <列表名> 用）。
 *  目录/歌单名上限 128（player_controller_status_t::dir_name 同款） */
#define CONFIG_STORE_SLOT_NAME_MAX  128

/**
 * @brief 单个配置槽位的内容摘要（供 UI 列出 「SAVE1 <列表名>」）
 *
 * ⚠ 结构体里的字段与"活跃槽位无关"，读的是**指定槽位**已保存的内容；
 *   只有 has_data=true 时其余字段才有意义（否则该槽位从未保存过）。
 */
typedef struct {
    bool     has_data;       /* 该槽位是否已保存过数据（false = 空槽位，
                                界面显示"未保存"，切换时按默认值起步） */
    bool     last_valid;     /* 槽位内是否有"最后播放歌曲"记录 */
    uint32_t last_global;    /* 最后播放的歌曲全局下标（last_valid 时有效） */
    char     list_name[CONFIG_STORE_SLOT_NAME_MAX];  /* 该歌曲所在目录/歌单名
                                                        （保存时记下的快照，
                                                        播放列表数据变化后由
                                                        切换逻辑按下标回退修正） */
} config_store_slot_info_t;

/**
 * @brief 一个配置槽位的完整内容快照（切换配置时由 config_slots 读取并应用）
 *
 * 这是"配置插槽"要切换的全部字段，也是"运行时应用"的输入。应用侧一般不需要
 * 直接用它——只走 config_store_switch_slot + config_slots_switch。
 */
typedef struct {
    bool     has_data;                        /* 槽位是否已保存过（见上） */
    bool     last_valid;                      /* 是否有最后播放歌曲记录 */
    uint32_t last_global;                     /* 最后播放歌曲全局下标 */
    uint8_t  volume;                          /* 音量档位（0~30） */
    uint8_t  loop_mode;                       /* 循环模式（player_loop_mode_t） */
    bool     shuffle;                         /* 随机播放开关 */
    bool     shuffle_pos_valid;               /* 随机位置是否有记录 */
    uint64_t shuffle_pos;                     /* 随机序列逻辑位置 */
    bool     eq_valid;                        /* 是否带 EQ 预设 */
    char     eq_name[EQ_FILE_NAME_MAX];       /* .eq 文件名 */
    eq_preset_t eq_data;                      /* 解析后的 EQ 参数（eq_valid 时有效） */
    char     list_name[CONFIG_STORE_SLOT_NAME_MAX];  /* 保存时记下的目录/歌单名
                                                        （列表名快照，供 UI 显示） */
} config_store_slot_snapshot_t;

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

/** @brief 睡眠定时的默认时长（分钟，0=无默认）。全局一份，与配置槽位无关；是否自动开启由 sleep_auto 决定 */
uint32_t config_store_get_sleep_default_min(void);

/** @brief 睡眠定时「开机自动开启」模式（总是关闭 / 按时间段 / 总是开启） */
config_store_sleep_auto_t config_store_get_sleep_auto_mode(void);

/** @brief 睡眠定时时间段-开始小时（0~23；仅「按时间段」模式生效） */
uint8_t config_store_get_sleep_range_start_hour(void);

/** @brief 睡眠定时时间段-结束小时（0~23；左闭右开，可与开始小时构成跨零点区间） */
uint8_t config_store_get_sleep_range_end_hour(void);

/** @brief 睡眠定时清醒感知开关（true=切歌/调音量等动作按 NightPlayer 算法加时） */
bool config_store_get_sleep_awake(void);

/** @brief 自动息屏超时（秒，0=不自动息屏；默认 30） */
uint32_t config_store_get_screen_off_s(void);

/**
 * @brief 时区（UTC 偏移，单位分钟；默认 +480 = UTC+8）
 *
 * RTC 存的是本地墙钟时间，网络对时按 `UTC + 本偏移` 写入；手动修改时间/日期
 * 直接按用户看到的本地时间写 RTC。修改本值时由 UI 负责同步平移 RTC（保持
 * 绝对时刻不变），见 ui/time_date_popup.c。
 */
int config_store_get_tz_offset_min(void);

/** @brief 虚拟 ClickWheel 速滑按钮开关（true=长列表显示速滑按钮；默认开） */
bool config_store_get_click_wheel(void);

/** @brief 虚拟 ClickWheel 滑动方向（true=反转：顺时针向上滚；默认正常） */
bool config_store_get_click_wheel_reverse(void);

/** @brief 虚拟 ClickWheel 速度档位（取值见 config_store_cw_speed_t，默认普通） */
uint8_t config_store_get_click_wheel_speed(void);

/** @brief 触摸测试白点开关（true=开机创建跟随手指的白色圆点；默认关） */
bool config_store_get_touch_test(void);

/** @brief 回放增益总开关（true=按 .npd 元数据增益应用；false=全部按 0dB 播放。
 *          默认开） */
bool config_store_get_replay_gain(void);

/**
 * @brief 电池健康保护开关（true=PMU 恒压目标 4.0V，约 75~80% 电量；默认关）
 *
 * 读取后由 main.c 在开机时调用 battery_set_health_protection() 下发到 PMU；
 * 设置菜单切换时也由页面回调立即下发。PMU 每次上电都会回 4.2V，故不能只
 * 依赖"上次写过"。
 */
bool config_store_get_batt_care(void);

/** @brief 时间显示是否 24 小时制（true=24h，默认；false=12h，不显示上下午） */
bool config_store_get_time_24h(void);

/** @brief 关机前是否在播放（true=播放中，开机自动续播；false=已暂停/停止，
 *          开机只恢复现场不自动播放）。无 NVS 记录时默认 true（保持旧行为）。 */
bool config_store_get_was_playing(void);

/**
 * @brief 读取上次播放歌曲（只记歌曲，**不记秒数位置**）
 *
 * ⚠ NightPlayer 原版机制：只记忆"当前歌曲"，不记忆秒数位置——开机从歌曲
 *   开头开始（避免恢复播放位置带来的复杂度/体验差异）。故本接口没有位置
 *   输出参数。
 *
 * @param[out] global 上次播放歌曲全局下标（可 NULL）
 * @return true 有记录（NVS 中存在有效的上次播放记录）；false 无记录
 */
bool config_store_get_last_song(uint32_t *global);

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

/* ---------- 延迟落盘（高频 UI 调节专用，见 NVS_WRITE_BUDGET.md） ----------
 *
 * 背景：一次 nvs_commit() 要写一整页 flash（4KB）并关掉两颗核的外部缓存
 * 5~20ms。亮度/音量竖条拖动时值每帧都在变（亮度约 30 次/秒），若每次 set
 * 都落盘，LVGL 的 33ms 帧预算会被反复吃光 → 用户可见的剧烈卡顿，且 NVS
 * 只 6 页、会被“追加写 + 页满 GC/擦除”反复磨损。
 *
 * 做法：拖动期间只调 *_deferred 版本（只更新内存 + 置脏标志），落盘统一由
 * config_store_flush_pending() 在“松手 / 静默 500ms / 息屏 / 伪关机 / 切槽位”
 * 时做一次。硬件（DAC 音量、屏幕亮度）仍由调用方每帧实时写，不受影响。
 */

/**
 * @brief 设置亮度：**只改内存 + 通知，不写 NVS**（高频拖动专用）
 *
 * 仅供亮度竖条的 LV_EVENT_PRESSING 使用；需要“立即落盘”的场景（开机的
 * 无卡亮度拉满等）继续用 config_store_set_brightness()。
 * 落盘由 config_store_flush_pending() 完成。
 *
 * @param brightness 0~255，低于 CONFIG_STORE_BRIGHTNESS_MIN 自动钳位
 */
void config_store_set_brightness_deferred(uint8_t brightness);

/**
 * @brief 设置音量：**只改内存 + 通知，不写 NVS**（高频拖动专用）
 *
 * 语义与 config_store_set_volume() 相同（0~30，越界钳位），但把“写槽位
 * blob”推迟到 config_store_flush_pending()。仅供音量竖条 PRESSING 使用。
 *
 * ⚠ 调用方仍须**先** audio_set_volume_level() 再调本函数：变更回调读的是
 *   audio 的当前值，顺序反了状态栏显示会滞后一档。
 */
void config_store_set_volume_deferred(uint8_t vol);

/**
 * @brief 把“延迟落盘”的字段写入 NVS（亮度和音量）
 *
 * - force = false：距最后一次修改已静默 ≥ 500ms 才写（保底，由 ui/app.c 的
 *   100ms 定时器周期调用）；
 * - force = true：立刻写（松手 / 息屏 / 伪关机 / 切槽位等必须落盘的时刻）。
 *
 * 无脏字段时立即返回，不产生任何 flash 操作。
 *
 * ⚠ 发起任务必须**栈在内部 RAM**（写 flash 会关外部缓存）：LVGL 任务、
 *   deep_worker 可以；power_svc 等 PSRAM 栈任务不行，见
 *   FLASH_WRITE_VS_PSRAM.md §五。
 */
void config_store_flush_pending(bool force);

/** @brief 设置循环模式并持久化（uint8，对应 player_loop_mode_t） */
void config_store_set_loop_mode(uint8_t mode);

/** @brief 设置随机播放开关并持久化 */
void config_store_set_shuffle(bool on);

/**
 * @brief 保存随机播放位置并持久化（由 player_controller 调用）
 *
 * ⚠ 值（且 valid 标志）未变时**直接返回、不写 NVS**：切槽位/暂停/关机会反复
 *   调本接口，整块重写槽位 blob 没必要（见 CONFIG_SLOTS.md §七.3）。
 */
void config_store_set_shuffle_pos(uint64_t pos);

/**
 * @brief 设置睡眠定时的默认时长并持久化（分钟，0=关闭）
 *
 * ⚠ 全局键（sleepdef），**不进配置槽位**：切换 SAVE1~SAVE4 不会改变它，
 *   也不会把定时重新开表（睡眠定时与配置槽位互相独立）。
 */
void config_store_set_sleep_default_min(uint32_t minutes);

/** @brief 设置睡眠定时「开机自动开启」模式并持久化（全局键，同样不进槽位） */
void config_store_set_sleep_auto_mode(config_store_sleep_auto_t mode);

/**
 * @brief 设置睡眠定时时间段的开始/结束小时并持久化
 *
 * 自动收敛：参数钳到 0~23；两者相等时把结束小时改为 (start + 1) % 24
 * （时间段不可为空，与 UI 弹窗的"不可相同"校验一致）。
 */
void config_store_set_sleep_range_hours(uint8_t start_hour, uint8_t end_hour);

/** @brief 设置睡眠清醒感知开关并持久化 */
void config_store_set_sleep_awake(bool on);

/** @brief 设置自动息屏超时并持久化（秒，自动收敛到 5~3600，0=不自动息屏） */
void config_store_set_screen_off_s(uint32_t seconds);

/**
 * @brief 设置时区偏移并持久化（分钟，自动收敛到 -720~840，按 15 分钟取整）
 *
 * ⚠ 本函数只改配置，不移动 RTC：需要"改时区后钟面跟着变"的调用方
 *   （ui/time_date_popup.c）负责把 RTC 平移 (new - old) 分钟。
 */
void config_store_set_tz_offset_min(int minutes);

/** @brief 设置虚拟 ClickWheel 开关并持久化（设置菜单 → 虚拟 ClickWheel 子页）。
 *          与"重启生效"的开发者选项不同，本项立即生效：调用方随后
 *          click_wheel_set_enabled() 隐藏/显示速滑按钮；同时通知变更回调 */
void config_store_set_click_wheel(bool on);

/** @brief 设置虚拟 ClickWheel 滑动方向并持久化（true=反转）。
 *          立即生效：调用方随后 click_wheel_set_reverse()；同时通知变更回调 */
void config_store_set_click_wheel_reverse(bool reverse);

/** @brief 设置虚拟 ClickWheel 速度档位并持久化（取值见 config_store_cw_speed_t，
 *          越界钳到"普通"）。立即生效：调用方随后
 *          click_wheel_set_speed()；同时通知变更回调 */
void config_store_set_click_wheel_speed(uint8_t speed);

/** @brief 设置触摸测试白点开关并持久化（开发者选项；开机检查，重启生效，
 *          仅持久化不通知回调） */
void config_store_set_touch_test(bool on);

/** @brief 设置回放增益总开关并持久化（播放设置页第 8 格）。
 *          立即生效：音频链路由控制器轮询到变化后重新应用增益；同时通知
 *          变更回调，让设置页底部文字立即显示"关闭"/增益值 */
void config_store_set_replay_gain(bool on);

/** @brief 设置电池健康保护开关并持久化（设置菜单「电源选项」）。
 *          ⚠ 只存 NVS：PMU 的 CV 写入由调用方随后调用
 *            battery_set_health_protection()（或开机时 main.c）完成 */
void config_store_set_batt_care(bool on);

/** @brief 设置时间显示制式并持久化（true=24h，false=12h；状态栏/下拉栏立即刷新） */
void config_store_set_time_24h(bool on);

/** @brief 设置关机前播放状态并持久化（由 player_controller 在伪关机保存
 *          播放位置时一并调用；UI 无展示，不通知回调） */
void config_store_set_was_playing(bool playing);

/** @brief 保存上次播放歌曲并持久化（由 player_controller 调用）。
 *          ⚠ 只记歌曲下标，不记秒数位置（NightPlayer 机制，见 get_last_song）
 *          ⚠ 值与 valid 标志未变时**直接返回、不写 NVS**：切槽位前/伪关机前的
 *            player_controller_sync_config() 会无条件调它，歌曲没变就不该重写
 *            槽位 blob（真正必须落盘的那次由 switch_slot / save_active_slot 做） */
void config_store_set_last_song(uint32_t global);

/**
 * @brief 保存上次播放歌曲：**只改内存，不写 NVS**（返回内存值是否变化）
 *
 * 用于把“同一逻辑操作里的多次槽位写”合并成一次：调用方在改完所有字段后
 * 显式调用一次 config_store_save_active_slot() 即可（见 player_controller
 * 的 player_sync_config / player_persist_position）。
 * ⚠ 单独调用本函数而不跟 save_active_slot，位置变化只在本次会话内有效。
 */
bool config_store_set_last_song_ram(uint32_t global);

/**
 * @brief 保存随机播放位置：**只改内存，不写 NVS**（返回内存值是否变化）
 *
 * 语义同 config_store_set_last_song_ram（合并写用）。
 */
bool config_store_set_shuffle_pos_ram(uint64_t pos);

/** @brief 清除"上次播放位置"与"随机播放位置"记录（failsafe 界面"重置歌曲位置"
 *          按钮用）：内存标志置无效并写回 NVS（valid 标志=0），下次开机视为
 *          无位置记录 → 从第一首顺序播放 / 随机模式重新生成随机起点。
 *          不清除音量/循环模式等其它用户配置。 */
void config_store_reset_position(void);

/* ------------------------------------------------------------------ */
/* 配置槽位 API（应用侧一般只用 config_store_switch_slot）              */
/* ------------------------------------------------------------------ */

/** @brief 配置槽位数量（= CONFIG_STORE_SLOT_COUNT，UI 列表按它生成） */
uint32_t config_store_slot_count(void);

/**
 * @brief 开机加载活跃槽位（app_main 中 config_store_init 之后立刻调用）
 *
 * 读取"当前活跃槽位"索引（无记录 = 槽位 1），若该槽位已保存过数据，则把它整份
 * 载入内存（此后 get_xxx 返回的就是该槽位的值）。槽位为空时保持 config_store_init
 * 读到的旧全局键值（升级兼容）。
 *
 * ⚠ 必须在**任何读取槽位内字段的动作之前**调用：player_controller_init 要按
 *   last_song/loop/shuffle 恢复现场，main.c 要按它应用音量/EQ。
 */
void config_store_load_active_slot(void);

/** @brief 当前活跃槽位下标（0 起；范围恒为 [0, count)） */
uint32_t config_store_get_active_slot(void);

/**
 * @brief 直接设置活跃槽位索引（不加载内容、不切换运行时）
 *
 * 只写"记忆用的索引"，用于开机初始化。运行时要真正换一套配置请用 ui 层的
 * config_slots_switch（内部走 config_store_switch_slot）。
 *
 * @param idx 槽位下标，越界忽略
 */
void config_store_set_active_slot(uint32_t idx);

/**
 * @brief 读取指定槽位的内容摘要（UI 列表用；越界返回 false 并清空 out）
 *
 * @param idx 槽位下标（0 起）
 * @param out 输出摘要（可为 NULL，仅查询是否存在）
 * @return true 槽位已保存过数据；false 空槽位/越界
 */
bool config_store_get_slot_info(uint32_t idx, config_store_slot_info_t *out);

/**
 * @brief 读取指定槽位的完整内容快照（越界/空槽位时 has_data=false）
 *
 * @param idx 槽位下标（0 起）
 * @param out 输出快照
 */
void config_store_get_slot_snapshot(uint32_t idx, config_store_slot_snapshot_t *out);

/**
 * @brief 把当前内存中的配置存回**当前活跃槽位**（写 NVS）
 *
 * 调用点：切换槽位前、伪关机时（player_controller 保存播放位置之后）。
 */
void config_store_save_active_slot(void);

/**
 * @brief 记录"当前列表名"快照（UI 显示 SAVE1 <列表名> 用，由 UI/播放侧调用）
 *
 * 存进**当前活跃槽位**的内存配置（与 last_global 同属一个槽位，下次
 * config_store_save_active_slot 时随 blob 一起落盘；本函数**不写 NVS**）。
 * config_store 不认识 playlist，故名字由调用方提供。
 *
 * @param name 目录/歌单名（NULL 或空串忽略）
 */
void config_store_set_slot_list_name(const char *name);

/**
 * @brief 是否有尚未派发的配置变更通知（调试/可选查询用）
 *
 * set_xxx 只置"待通知"标志，真正的回调派发见 config_store_notify_dispatch。
 */
bool config_store_notify_pending(void);

/**
 * @brief 派发待处理的配置变更通知（**必须在 LVGL 任务上下文调用**）
 *
 * 由 ui/app.c 的一个 100ms lv_timer 周期调用。所有已注册回调（设置页/状态栏/
 * 通知栏）都在这里、在 LVGL 任务里执行，因此可以安全操作 LVGL 控件。
 *
 * 无待通知时立即返回；派发期间若有新的 set_xxx 发生，会在下一轮再派发一次
 * （不丢通知、不递归）。
 */
void config_store_notify_dispatch(void);

/**
 * @brief 切换活跃槽位（存储层）：先保存当前槽位 → 再载入目标槽位到内存
 *
 * ⚠ 只改内存与 NVS，**不触碰运行时**（音量/EQ/播放器/睡眠定时都不动）——
 *   运行时的应用由 ui 层的 config_slots_switch 统一负责，避免本模块反向依赖
 *   硬件与播放器。
 *
 * 切换后会用 config_notify() 通知一次（设置页等 UI 随即刷新为新槽位的值）。
 * 目标槽位为空（从未保存过）时载入本模块的默认值与"无位置记录"（等同新机
 * 配置），随后切换逻辑会自动从第一首开始播放。
 *
 * @param idx 目标槽位下标；越界或与当前相同则直接返回（不写盘、不通知）
 */
void config_store_switch_slot(uint32_t idx);

/**
 * @brief 注册配置变更回调（最多 4 个，重复注册会覆盖同名项）
 *
 * ⚠ **回调不在 set_xxx 里同步执行**：set_xxx 只置"待通知"标志，回调统一由
 *   config_store_notify_dispatch() 在 **LVGL 任务上下文**里派发（ui/app.c 的
 *   100ms 定时器），因此回调内可以安全操作 LVGL 控件，且任何任务都可以调用
 *   set_xxx（不会在非 LVGL 任务里碰 UI 对象树）。
 *
 * @param cb  回调函数（NULL 忽略）
 * @param ctx 透传给回调的用户上下文
 */
void config_store_register_cb(config_store_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
