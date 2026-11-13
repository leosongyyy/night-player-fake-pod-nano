#include "core/storage/config_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config_store";

/* NVS 命名空间与键名（键名 ≤ 15 字符） */
#define CONFIG_NVS_NS        "config"
#define KEY_VOLUME           "volume"
#define KEY_BRIGHTNESS       "bright"
#define KEY_LOOP_MODE        "loop"
#define KEY_SHUFFLE          "shuffle"
#define KEY_SHUFFLE_POS_HI   "shuf_pos_hi"
#define KEY_SHUFFLE_POS_LO   "shuf_pos_lo"
#define KEY_SHUFFLE_POS_VAL  "shuf_pos_val"
#define KEY_SLEEP_DEFAULT    "sleepdef"
#define KEY_SLEEP_DEF_ON     "sleepdefon"
#define KEY_SLEEP_AWAKE      "sleepawake"
#define KEY_SCREEN_OFF       "screenoff"
#define KEY_CLICK_WHEEL      "clkwheel"
#define KEY_CW_REVERSE       "clkwheel_rev"   /* 滑动方向：0=正常，1=反转 */
#define KEY_CW_SPEED         "clkwheel_spd"   /* 速度档位：0=普通，1=快速，2=极速 */
/* ⚠ 不兼容旧键 `clkwheel_fast`（旧版 bool 速度）：速度档位功能尚未发布，
   最终用户拿到的固件只会有新键 `clkwheel_spd`，无需迁移逻辑 */
#define KEY_TOUCH_TEST       "touchtest"
#define KEY_WAS_PLAYING      "was_play"
#define KEY_LAST_GLOBAL      "last_glob"
#define KEY_LAST_VALID       "last_valid"
/* ⚠ 旧键 "last_pos"（播放秒数）已废弃：NightPlayer 机制只记"当前歌曲"，秒数
   位置恒为 0，本项目**不读取也不写入**该键（历史值 0，留着无害） */
#define KEY_EQ_NAME          "eqname"
#define KEY_EQ_DATA          "eqdata"
#define KEY_REPLAY_GAIN      "replaygain"   /* 回放增益总开关：0=关，1=开 */

/* ---------- 配置槽位（SAVE1~SAVE4，见 config_store.h 文件头"配置槽位"） ----------
   ⚠ 上面那批"可切换字段"的旧全局键**保留不删**：活跃槽位从未保存过时仍读写
   它们（升级兼容，见 s_slot_active）；首次保存槽位后全局键冻结，后续以槽位为准。
   槽位内字段（音量/循环/随机/随机位置/EQ/最后歌曲）不再单独写全局键，
   避免一次 set 两次 nvs_commit（每槽位整块 blob 一次写完）。
   ⚠ 睡眠定时（时长 sleepdef + 开关 sleepdefon）**不进槽位**，始终读写全局键，
   与"配置插槽"互相独立（2026-11 调整）。 */
#define KEY_ACTIVE_SLOT      "slot_active"                   /* 活跃槽位下标 */
#define SLOT_KEY_PREFIX      "slot"                          /* 槽位 blob 前缀：slot0..slot3 */
#define SLOT_BLOB_VERSION    1u                              /* 槽位 blob 结构版本（布局改了要 +1，
                                                               旧版本 blob 自动视为空槽位） */

/* 变更回调容量（设置页 + 通知栏等，够用） */
#define CONFIG_STORE_MAX_CB  4

/* ---------- 内存中的配置值（开机由 NVS 加载） ---------- */

static bool s_inited = false;
static nvs_handle_t s_nvs = 0;

/* ========== 可切换字段（属于"当前配置"，随配置槽位整体切换） ==========

   ⚠ 这一块是"配置插槽"的核心：下面这些值不再各自独立持久化，而是打包进
   "当前活跃槽位"的 blob（见 config_store_save_active_slot）。应用侧照旧用
   get_xxx / set_xxx 访问，完全无感（见 config_store.h 文件头说明）。 */
typedef struct {
    uint8_t  volume;                /* 音量档位（0~30，0=静音） */
    uint8_t  loop_mode;             /* 循环模式（player_loop_mode_t 取值） */
    bool     shuffle;               /* 随机播放开关 */
    bool     shuf_pos_valid;        /* 随机序列位置是否有记录 */
    uint64_t shuf_pos;              /* 随机序列逻辑位置（uint64 无限序列） */
    bool     last_valid;            /* 是否有"最后播放歌曲"记录 */
    uint32_t last_global;           /* 最后播放歌曲全局下标 */
    bool     eq_valid;              /* 是否选择了 EQ 预设 */
    char     eq_name[EQ_FILE_NAME_MAX];   /* .eq 文件名（不含路径） */
    eq_preset_t eq_data;            /* 解析后的 EQ 参数（eq_valid 时有效） */
    /* "最后播放歌曲"所在的目录/歌单名（纯 UI 展示用：槽位列表显示
       "SAVE1 <列表名>"）。⚠ 与 last_global 同属一个槽位，必须一起打包/解包：
       2026-11 修复前它只存在槽位缓存里，且 config_store_set_slot_list_name()
       要求"槽位已有数据"——于是**首次保存一个空槽位时名字被丢掉**（列表里
       只剩 "SAVE2"，看起来像"没保存"）。放进 s_live 后，任何一次保存都会
       连同列表名一起落盘，不再依赖缓存状态。 */
    char     list_name[CONFIG_STORE_SLOT_NAME_MAX];
} config_live_t;

static config_live_t s_live = {
    .volume            = CONFIG_STORE_VOLUME_DEFAULT,
    .loop_mode         = CONFIG_STORE_LOOP_DEFAULT,
    .shuffle           = false,
    .shuf_pos_valid    = false,
    .shuf_pos          = 0,
    .last_valid        = false,
    .last_global       = 0,
    .eq_valid          = false,
    .eq_name           = "",
    .list_name         = "",
};

/* ========== 不可切换字段（全局共用一份，不随配置槽位切换） ========== */

static uint8_t  s_brightness  = CONFIG_STORE_BRIGHTNESS_DEFAULT;
/* 睡眠定时（2026-11 起与配置槽位**互相独立**，全局只有一份）：
   - s_sleep_default_min：记忆的定时时长（分钟，0=无默认），是"开启时用的档位"；
   - s_sleep_def_on：开机默认开启的总开关。
   两者都不进槽位 blob，切换 SAVE1~SAVE4 不会改变睡眠定时的时长/开关。 */
static uint32_t s_sleep_default_min = CONFIG_STORE_SLEEP_DEFAULT_MIN;
static bool     s_sleep_def_on = CONFIG_STORE_SLEEP_DEF_ON_DEFAULT;
static bool     s_sleep_awake = CONFIG_STORE_SLEEP_AWAKE_DEFAULT;
static uint32_t s_screen_off  = CONFIG_STORE_SCREEN_OFF_DEFAULT;   /* 息屏超时：全局键
                                                                     screenoff，与配置槽位无关 */
/* 虚拟 ClickWheel 速滑按钮（默认开）：设置菜单 →「虚拟 ClickWheel 选项」子页
   切换。三项配套配置：开关 / 滑动方向（正常=顺时针向下滚，反转=顺时针向上滚）/
   速度档位（普通 ×1.00 / 快速 ×0.75 / 极速 ×0.50，点击循环），均立即生效 */
static bool     s_click_wheel = (CONFIG_STORE_CLICK_WHEEL_DEFAULT != 0);
static bool     s_cw_reverse  = (CONFIG_STORE_CW_REVERSE_DEFAULT != 0);
static uint8_t  s_cw_speed    = CONFIG_STORE_CW_SPEED_DEFAULT;
/* 开发者选项（运行时开关，开机检查、重启生效）：触摸测试白点（默认关） */
static bool     s_touch_test  = (CONFIG_STORE_TOUCH_TEST_DEFAULT != 0);
/* 关机前播放状态：默认 true（无记录时开机自动播放，保持旧行为） */
static bool     s_was_playing = true;
/* 回放增益总开关（默认开）：关=所有歌按 0dB 播放，开=按 .npd 元数据增益。
   设置页第 5 格切换并持久化；控制器轮询到变化后重新应用（见
   player_controller 的开关同步），故通知回调让设置页立即刷新文本 */
static bool     s_replay_gain = (CONFIG_STORE_REPLAY_GAIN_DEFAULT != 0);

/* ---------- 延迟落盘（NVS 写预算，见 ideas_and_exps/NVS_WRITE_BUDGET.md） ----------
 *
 * 背景：一次 nvs_commit() 要写一整页 flash（4KB）并关掉两颗核的外部缓存
 * 5~20ms。亮度/音量竖条拖动时值每帧都在变（亮度约 30 次/秒），每次落盘都会
 * 吃掉 LVGL 的 33ms 帧预算 → 用户可见的剧烈卡顿；NVS 又只有 6 页，会被
 * “追加写 + 页满 GC/擦除”反复磨损。
 *
 * 做法：拖动中只调 *_deferred（只更新内存 + 置脏），落盘统一由
 * config_store_flush_pending() 在松手/静默 500ms/息屏/伪关机/切槽位时做一次。
 *
 * ⚠ 脏标志只由 LVGL 任务与少数几个任务读写，单字节 bool 原子的即可；
 *   时间戳只在置脏（写入方）与 flush（LVGL 任务）里用，值偏差无害。 */
#define CONFIG_STORE_FLUSH_QUIET_US  (500 * 1000)   /* 静默 500ms 后保底落盘 */
static volatile bool s_bright_dirty = false;
static int64_t       s_bright_dirty_us = 0;   /* 最后一次亮度修改时刻（esp_timer） */
static volatile bool s_volume_dirty = false;
static int64_t       s_volume_dirty_us = 0;   /* 最后一次音量修改时刻（esp_timer） */

/* ========== 配置槽位状态 ========== */

/**
 * 单个槽位的 NVS blob 布局（POD，直接 nvs_set_blob / nvs_get_blob）
 *
 * ⚠ 只放**纯数据**：eq_preset_t 是固定布局的纯数据（128 字节），可整块存；
 *   字符串用定长数组。sizeof 见文件末尾的 _Static_assert 兜底（改动布局要
 *   同步 bump SLOT_BLOB_VERSION，旧 blob 会被当作空槽位忽略）。
 */
typedef struct {
    uint32_t version;                       /* = SLOT_BLOB_VERSION，版本不符视为空槽位 */
    uint32_t last_global;                   /* 最后播放歌曲全局下标 */
    uint32_t reserved_sleep;                /* 原 sleep_default_min 字段：睡眠定时时长
                                               2026-11 起不再属于槽位（改用全局键
                                               sleepdef），此 4 字节仅作占位以保持
                                               blob 布局/大小/版本不变——老槽位照常
                                               读出（该字段被忽略），不会因升级丢槽位 */
    uint64_t shuffle_pos;                   /* 随机序列逻辑位置 */
    uint8_t  volume;                        /* 音量档位 0~30 */
    uint8_t  loop_mode;                     /* 循环模式 */
    uint8_t  shuffle;                       /* 随机开关 0/1 */
    uint8_t  shuffle_pos_valid;             /* 随机位置有效 0/1 */
    uint8_t  last_valid;                    /* 最后歌曲记录有效 0/1 */
    uint8_t  eq_valid;                      /* 有 EQ 预设 0/1 */
    uint8_t  eq_band_count;                 /* EQ 段数（与 eq_valid 冗余，便于校验） */
    uint8_t  reserved;                      /* 对齐填充（显式写出，避免结构体隐式填充差异） */
    char     eq_name[EQ_FILE_NAME_MAX];     /* .eq 文件名 */
    eq_preset_t eq_data;                    /* EQ 参数（128B） */
    char     last_list_name[CONFIG_STORE_SLOT_NAME_MAX];  /* 最后歌曲所在目录/歌单名
                                                              （只用于 UI 显示 SAVE1 <列表名>） */
} config_slot_blob_t;

_Static_assert(sizeof(config_slot_blob_t) < 512,
               "slot blob too large for NVS (24KB partition, 4 slots)");
/* 4 个槽位 + 全局键必须能装进 nvs 分区（24KB，留足余量避免半写） */
_Static_assert(sizeof(config_slot_blob_t) * CONFIG_STORE_SLOT_COUNT < 4096,
               "all config slots must fit comfortably in the 24KB nvs partition");

/* 各槽位已读到内存的 blob（开机 lazy 读取并缓存；NULL/version 不符 = 空槽位）。
   ⚠ 只缓存"本会话读过的槽位"，最多 4 份 × ~320B ≈ 1.3KB，栈外的 .bss 常驻。 */
static config_slot_blob_t s_slot_cache[CONFIG_STORE_SLOT_COUNT];
static bool               s_slot_cache_valid[CONFIG_STORE_SLOT_COUNT] = { false };

/* 当前活跃槽位下标（0 起）与其内容是否已保存过。
   s_slot_active_saved=false 表示"还没落过盘"——此时可切换字段仍写旧全局键
   （升级兼容），首次保存后冻结。 */
static uint32_t s_slot_active = 0;
static bool     s_slot_active_saved = false;

/* 变更回调表 */
typedef struct {
    config_store_cb_t cb;
    void *ctx;
} config_cb_entry_t;

static config_cb_entry_t s_cbs[CONFIG_STORE_MAX_CB];
static uint8_t s_cb_count = 0;

/* ------------------------------------------------------------------ */
/* 内部辅助                                                            */
/* ------------------------------------------------------------------ */

/* 槽位 blob 的 NVS 键名（"slot0".."slot3"；NVS 键名以 '\0' 结尾且 ≤15 字符） */
static void slot_key(char *buf, size_t size, uint32_t idx)
{
    snprintf(buf, size, "%s%u", SLOT_KEY_PREFIX, (unsigned)idx);
}

/* 把内存中的可切换字段打包成槽位 blob（含 UI 显示的列表名快照） */
static void slot_pack(config_slot_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version           = SLOT_BLOB_VERSION;
    b->last_global       = s_live.last_global;
    b->shuffle_pos       = s_live.shuf_pos;
    b->volume            = s_live.volume;
    b->loop_mode         = s_live.loop_mode;
    b->shuffle           = s_live.shuffle ? 1u : 0u;
    b->shuffle_pos_valid = s_live.shuf_pos_valid ? 1u : 0u;
    b->last_valid        = s_live.last_valid ? 1u : 0u;
    b->eq_valid          = s_live.eq_valid ? 1u : 0u;
    b->eq_band_count     = s_live.eq_valid ? s_live.eq_data.band_count : 0u;
    if (s_live.eq_valid) {
        snprintf(b->eq_name, sizeof(b->eq_name), "%s", s_live.eq_name);
        b->eq_data = s_live.eq_data;
    }
    snprintf(b->last_list_name, sizeof(b->last_list_name), "%s", s_live.list_name);
}

/* 把槽位 blob 解包进内存可切换字段（切换槽位/开机加载时） */
static void slot_unpack(const config_slot_blob_t *b)
{
    s_live.last_global       = b->last_global;
    s_live.shuf_pos          = b->shuffle_pos;
    s_live.volume            = b->volume;
    s_live.loop_mode         = b->loop_mode;
    s_live.shuffle           = (b->shuffle != 0);
    s_live.shuf_pos_valid    = (b->shuffle_pos_valid != 0);
    s_live.last_valid        = (b->last_valid != 0);
    s_live.eq_valid          = (b->eq_valid != 0) &&
                               (b->eq_band_count > 0) &&
                               (b->eq_band_count <= EQ_MAX_BANDS);
    s_live.eq_name[0]        = '\0';
    memset(&s_live.eq_data, 0, sizeof(s_live.eq_data));
    if (s_live.eq_valid) {
        snprintf(s_live.eq_name, sizeof(s_live.eq_name), "%s", b->eq_name);
        s_live.eq_data = b->eq_data;
    }
    /* 列表名快照（纯展示用，与 last_global 成对） */
    snprintf(s_live.list_name, sizeof(s_live.list_name), "%s", b->last_list_name);
    /* 音量/循环模式做一次范围防御（手改 NVS / 旧数据） */
    if (s_live.volume > CONFIG_STORE_VOLUME_MAX) {
        s_live.volume = CONFIG_STORE_VOLUME_MAX;
    }
    if (s_live.loop_mode > 2u) {   /* player_loop_mode_t 最大 LOOP_LIST=2 */
        s_live.loop_mode = CONFIG_STORE_LOOP_DEFAULT;
    }
}

/* 把槽位内容复位为"出厂默认 + 无位置记录"（切到空槽位时用） */
static void slot_apply_defaults(void)
{
    s_live.volume            = CONFIG_STORE_VOLUME_DEFAULT;
    s_live.loop_mode         = CONFIG_STORE_LOOP_DEFAULT;
    s_live.shuffle           = false;
    s_live.shuf_pos_valid    = false;
    s_live.shuf_pos          = 0;
    s_live.last_valid        = false;
    s_live.last_global       = 0;
    s_live.eq_valid          = false;
    s_live.eq_name[0]        = '\0';
    memset(&s_live.eq_data, 0, sizeof(s_live.eq_data));
    s_live.list_name[0]      = '\0';
}

/* 从 NVS 读槽位 blob 到缓存；返回 true = 有效槽位（含版本校验） */
static bool slot_cache_load(uint32_t idx)
{
    if (idx >= CONFIG_STORE_SLOT_COUNT) {
        return false;
    }
    if (s_slot_cache_valid[idx]) {
        return true;
    }
    if (s_nvs == 0) {
        return false;
    }
    char key[16];
    slot_key(key, sizeof(key), idx);
    config_slot_blob_t b;
    size_t len = sizeof(b);
    if (nvs_get_blob(s_nvs, key, &b, &len) != ESP_OK) {
        return false;   /* 空槽位（键不存在） */
    }
    if (len != sizeof(b) || b.version != SLOT_BLOB_VERSION) {
        ESP_LOGW(TAG, "slot %u blob incompatible (len=%u ver=%u), treat as empty",
                 (unsigned)idx, (unsigned)len, (unsigned)b.version);
        return false;
    }
    s_slot_cache[idx] = b;
    s_slot_cache_valid[idx] = true;
    return true;
}

/* 把槽位 blob 写入 NVS 并更新缓存。
   commit=false 时只改 NVS 的 RAM 页缓存，由调用方随后 cfg_commit() 一次落盘
   （切槽位时 blob 与 slot_active 一起提交，省一次 4KB 页写）。 */
static void slot_cache_store(uint32_t idx, const config_slot_blob_t *b, bool commit)
{
    if (idx >= CONFIG_STORE_SLOT_COUNT || s_nvs == 0) {
        return;
    }
    char key[16];
    slot_key(key, sizeof(key), idx);
    esp_err_t err = nvs_set_blob(s_nvs, key, b, sizeof(*b));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
        return;
    }
    if (commit) {
        err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", key, esp_err_to_name(err));
            return;
        }
    }
    s_slot_cache[idx] = *b;
    s_slot_cache_valid[idx] = true;
}

/* ------------------------------------------------------------------ */
/* NVS 读写辅助                                                        */
/* ------------------------------------------------------------------ */

/* ⚠ 写 flash 的代价与“数据多大”无关：NVS 的写单位是页（4KB），每次 commit
   都会把脏页刷盘并关外部缓存 5~20ms。所以批量场景一律“多次 set、一次
   commit”——下面的 cfg_set_* 只改 NVS 的 RAM 页缓存，cfg_commit() 才落盘。
   写入失败不致命：仅打日志，内存值仍有效（本次会话内生效，重启后丢失）。 */

static esp_err_t cfg_set_u8(const char *key, uint8_t value)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u8(s_nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cfg_set_u32(const char *key, uint32_t value)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_u32(s_nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cfg_set_str(const char *key, const char *value)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_str(s_nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cfg_set_blob(const char *key, const void *data, size_t len)
{
    if (s_nvs == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_blob(s_nvs, key, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* 提交所有脏页（一次 flash 写，与脏了多少键无关） */
static void cfg_commit(void)
{
    if (s_nvs == 0) {
        return;
    }
    esp_err_t err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
}

/* 单键写入 + 立即提交（频次低的场景用；批量场景用 cfg_set_* + cfg_commit） */
static void cfg_write_u8(const char *key, uint8_t value)
{
    if (cfg_set_u8(key, value) == ESP_OK) {
        cfg_commit();
    }
}

static void cfg_write_u32(const char *key, uint32_t value)
{
    if (cfg_set_u32(key, value) == ESP_OK) {
        cfg_commit();
    }
}

/* 删键；key 不存在时返回 ESP_ERR_NVS_NOT_FOUND，属正常情况，忽略。
   commit=false 时由调用方统一提交（批量场景）。 */
static void cfg_erase_key_ex(const char *key, bool commit)
{
    if (s_nvs == 0) {
        return;
    }
    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
        return;
    }
    if (commit) {
        cfg_commit();
    }
}

/* ------------------------------------------------------------------ */
/* 变更通知（延迟派发）                                                 */
/* ------------------------------------------------------------------ */

/* 待派发的变更通知标志（多任务写、LVGL 任务读并清）。
   ⚠⚠ 这里是**唯一**能让 UI 回调安全执行的设计：`set_xxx` 可能在任何任务里
   被调用（尤其是播放控制器的 player_ctrl 任务），而回调方（设置页/状态栏/
   通知栏）直接操作 LVGL 控件。2026-11 真实事故：player_ctrl 调
   config_store_set_shuffle → 回调里 lv_label_set_text 在**非 LVGL 任务**里
   改 LVGL 对象树，与同核的 LVGL 渲染并发 → 卡死在 lv_inv_area，
   task_wdt 触发（CPU0: player_ctrl）。所以 set_xxx 一律只置脏标记，
   由 LVGL 任务的定时器统一派发（见 ui/app.c 的 config_notify_timer_cb）。 */
static volatile bool s_notify_pending = false;

/* 请求一次变更通知（只置标志，绝不在这里调回调） */
static void config_notify(void)
{
    s_notify_pending = true;
}

bool config_store_notify_pending(void)
{
    return s_notify_pending;
}

void config_store_notify_dispatch(void)
{
    if (!s_notify_pending) {
        return;
    }
    /* 先清标志再回调：回调里若又发生 set_xxx，会在下一轮再派发一次，
       不会丢通知也不会无限递归 */
    s_notify_pending = false;
    for (uint8_t i = 0; i < s_cb_count; i++) {
        if (s_cbs[i].cb != NULL) {
            s_cbs[i].cb(s_cbs[i].ctx);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 槽位持久化辅助                                                      */
/* ------------------------------------------------------------------ */

/* 前向声明：下面的"旧键兼容写入"会用到 */
void config_store_save_active_slot(void);

/**
 * @brief 可切换字段改动后的统一持久化出口（所有 set_xxx 末尾调用）
 *
 * 两条路（见 config_store.h 文件头"配置槽位"）：
 *   1. 槽位从未保存过（升级上来的机器）→ 额外写一份旧全局键，保持与旧版本
 *      固件/旧 NVS 布局的兼容；
 *   2. 一律把当前配置整块写回当前活跃槽位（同一 blob 覆盖写，NVS 分页复用）。
 *
 * ⚠ 兼容层原本是"每个键各 commit 一次"（9 次 4KB 页写！），现改为
 *   “多次 set + 一次 commit”：NVS 的写单位是页，一次提交能覆盖所有脏键。
 */
static void slot_persist_live(void)
{
    /* 下面的 blob 一定包含当前音量，延迟落盘标记作废 */
    s_volume_dirty = false;

    if (!s_slot_active_saved) {
        /* 兼容层：首次保存槽位之前，把可切换字段同步写进旧全局键 */
        cfg_set_u8(KEY_VOLUME, s_live.volume);
        cfg_set_u8(KEY_LOOP_MODE, s_live.loop_mode);
        cfg_set_u8(KEY_SHUFFLE, s_live.shuffle ? 1u : 0u);
        cfg_set_u32(KEY_LAST_GLOBAL, s_live.last_global);
        /* ⚠ 秒数位置不保存（NightPlayer 机制：只记"当前歌曲"，开机从歌
           曲开头开始）——旧键 last_pos 保持不写，其历史值为 0 */
        cfg_set_u8(KEY_LAST_VALID, s_live.last_valid ? 1u : 0u);
        if (s_live.shuf_pos_valid) {
            cfg_set_u32(KEY_SHUFFLE_POS_HI, (uint32_t)(s_live.shuf_pos >> 32));
            cfg_set_u32(KEY_SHUFFLE_POS_LO, (uint32_t)(s_live.shuf_pos & 0xFFFFFFFFu));
            cfg_set_u8(KEY_SHUFFLE_POS_VAL, 1u);
        } else {
            cfg_set_u8(KEY_SHUFFLE_POS_VAL, 0u);
        }
        if (s_live.eq_valid) {
            cfg_set_str(KEY_EQ_NAME, s_live.eq_name);
            cfg_set_blob(KEY_EQ_DATA, &s_live.eq_data, sizeof(s_live.eq_data));
        } else {
            cfg_erase_key_ex(KEY_EQ_NAME, false);
            cfg_erase_key_ex(KEY_EQ_DATA, false);
        }
        /* 兼容层全部 set 完，随下面的槽位 blob 一次提交落盘 */
    }
    config_store_save_active_slot();
}

/* 载入指定槽位到内存（含"空槽位 → 出厂默认"分支）。
   调用方负责随后 config_notify()（本函数不改活跃下标）。
   log_why：日志前缀，用于区分"切槽位时载入新槽位"与其它时机，便于串口排查。 */
static void slot_load_into_live(uint32_t idx, bool mark_saved, const char *log_why)
{
    if (slot_cache_load(idx)) {
        slot_unpack(&s_slot_cache[idx]);
        s_slot_active_saved = mark_saved;
        ESP_LOGI(TAG, "%s: SAVE%u -> live (vol=%u loop=%u shuffle=%d "
                      "song=%s(%u))",
                 log_why, (unsigned)(idx + 1u), (unsigned)s_live.volume,
                 (unsigned)s_live.loop_mode, (int)s_live.shuffle,
                 s_live.last_valid ? "yes" : "no", (unsigned)s_live.last_global);
    } else {
        slot_apply_defaults();
        s_slot_active_saved = false;   /* 空槽位：仍处于兼容态（写旧全局键） */
        ESP_LOGI(TAG, "%s: SAVE%u empty -> defaults", log_why,
                 (unsigned)(idx + 1u));
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void config_store_init(void)
{
    if (s_inited) {
        return;
    }

    /* 初始化 NVS flash（首次烧录无分区时自动格式化） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区损坏/版本不兼容：擦除后重新初始化（用户配置会丢失，仅首次） */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s, config stays default",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_open(CONFIG_NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s, config stays default",
                 CONFIG_NVS_NS, esp_err_to_name(err));
        return;
    }

    /* 逐个加载配置；缺失或类型不匹配时保持默认值。
       音量：范围 0~30 档（0=静音，1~30 线性映射 20%~80%）。旧版本存的是
       0~100 百分比，读出的值若超范围按"20%~80% → 1~30 档"线性迁移并写回，
       保证老设备升级后音量平滑过渡（开发期升级）。 */
    if (nvs_get_u8(s_nvs, KEY_VOLUME, &s_live.volume) != ESP_OK) {
        s_live.volume = CONFIG_STORE_VOLUME_DEFAULT;
    } else if (s_live.volume > CONFIG_STORE_VOLUME_MAX) {
        uint8_t old_pct = s_live.volume;   /* 旧版本存储的 0~100 百分比 */
        if (old_pct == 0) {
            s_live.volume = CONFIG_STORE_VOLUME_MIN;
        } else if (old_pct <= 20) {
            s_live.volume = 1;             /* 旧 1~20% 全部归到最小档 1（原配置下几乎无声） */
        } else if (old_pct >= 80) {
            s_live.volume = CONFIG_STORE_VOLUME_MAX;
        } else {
            s_live.volume = (uint8_t)(((uint32_t)(old_pct - 20) * 29u / 60u) + 1u);
        }
        cfg_write_u8(KEY_VOLUME, s_live.volume);   /* 写回迁移结果，下次直接读到新档位 */
    }
    if (nvs_get_u8(s_nvs, KEY_BRIGHTNESS, &s_brightness) != ESP_OK) {
        s_brightness = CONFIG_STORE_BRIGHTNESS_DEFAULT;
    }
    /* 亮度下限：历史 NVS 可能存过极低亮度（如调试期滑到 0），
       钳制到 CONFIG_STORE_BRIGHTNESS_MIN，避免开机屏幕完全看不清 */
    if (s_brightness < CONFIG_STORE_BRIGHTNESS_MIN) {
        s_brightness = CONFIG_STORE_BRIGHTNESS_MIN;
    }
    if (nvs_get_u8(s_nvs, KEY_LOOP_MODE, &s_live.loop_mode) != ESP_OK) {
        s_live.loop_mode = CONFIG_STORE_LOOP_DEFAULT;
    }
    uint8_t shuffle = 0;
    if (nvs_get_u8(s_nvs, KEY_SHUFFLE, &shuffle) != ESP_OK) {
        s_live.shuffle = false;
    } else {
        s_live.shuffle = (shuffle != 0);
    }
    /* 随机播放位置：valid 标志为 1 才读取高/低 32 位 */
    uint8_t shuf_pos_valid = 0;
    if (nvs_get_u8(s_nvs, KEY_SHUFFLE_POS_VAL, &shuf_pos_valid) == ESP_OK
            && shuf_pos_valid != 0) {
        uint32_t hi = 0, lo = 0;
        s_live.shuf_pos_valid = true;
        if (nvs_get_u32(s_nvs, KEY_SHUFFLE_POS_HI, &hi) != ESP_OK) {
            hi = 0;
        }
        if (nvs_get_u32(s_nvs, KEY_SHUFFLE_POS_LO, &lo) != ESP_OK) {
            lo = 0;
        }
        s_live.shuf_pos = ((uint64_t)hi << 32) | lo;
    } else {
        s_live.shuf_pos_valid = false;
        s_live.shuf_pos = 0;
    }
    /* 睡眠定时时长：全局键，与配置槽位无关（切槽位不改变它）。
       ⚠ 旧版本的槽位 blob 里也存过一份该值，现已忽略（见 reserved_sleep）。 */
    if (nvs_get_u32(s_nvs, KEY_SLEEP_DEFAULT, &s_sleep_default_min) != ESP_OK) {
        s_sleep_default_min = CONFIG_STORE_SLEEP_DEFAULT_MIN;
    }
    uint8_t sleep_def_on = 0;
    if (nvs_get_u8(s_nvs, KEY_SLEEP_DEF_ON, &sleep_def_on) != ESP_OK) {
        s_sleep_def_on = (CONFIG_STORE_SLEEP_DEF_ON_DEFAULT != 0);
    } else {
        s_sleep_def_on = (sleep_def_on != 0);
    }
    uint8_t sleep_awake = 1;
    if (nvs_get_u8(s_nvs, KEY_SLEEP_AWAKE, &sleep_awake) != ESP_OK) {
        s_sleep_awake = (CONFIG_STORE_SLEEP_AWAKE_DEFAULT != 0);
    } else {
        s_sleep_awake = (sleep_awake != 0);
    }
    if (nvs_get_u32(s_nvs, KEY_SCREEN_OFF, &s_screen_off) != ESP_OK) {
        s_screen_off = CONFIG_STORE_SCREEN_OFF_DEFAULT;
    }
    /* 虚拟 ClickWheel：无记录（首次烧录/升级）时默认启用 */
    uint8_t click_wheel = 0;
    if (nvs_get_u8(s_nvs, KEY_CLICK_WHEEL, &click_wheel) != ESP_OK) {
        s_click_wheel = (CONFIG_STORE_CLICK_WHEEL_DEFAULT != 0);
    } else {
        s_click_wheel = (click_wheel != 0);
    }
    /* 滑动方向（正常/反转）与速度档位（普通/快速/极速）：无记录时用默认值 */
    uint8_t cw_reverse = 0;
    if (nvs_get_u8(s_nvs, KEY_CW_REVERSE, &cw_reverse) != ESP_OK) {
        s_cw_reverse = (CONFIG_STORE_CW_REVERSE_DEFAULT != 0);
    } else {
        s_cw_reverse = (cw_reverse != 0);
    }
    /* 速度档位（普通/快速/极速）：无记录时用默认值；越界按脏数据钳到"普通" */
    if (nvs_get_u8(s_nvs, KEY_CW_SPEED, &s_cw_speed) != ESP_OK) {
        s_cw_speed = CONFIG_STORE_CW_SPEED_DEFAULT;
    }
    if (s_cw_speed >= CONFIG_STORE_CW_SPEED_COUNT) {
        s_cw_speed = CONFIG_STORE_CW_SPEED_DEFAULT;
    }
    /* 开发者选项：无记录（首次烧录/升级）时默认关闭（见 CONFIG_STORE_*_DEFAULT） */
    uint8_t touch_test = 0;
    if (nvs_get_u8(s_nvs, KEY_TOUCH_TEST, &touch_test) != ESP_OK) {
        s_touch_test = (CONFIG_STORE_TOUCH_TEST_DEFAULT != 0);
    } else {
        s_touch_test = (touch_test != 0);
    }
    /* 回放增益总开关：无记录（首次烧录/升级）时默认开启（保持既有行为） */
    uint8_t replay_gain = 0;
    if (nvs_get_u8(s_nvs, KEY_REPLAY_GAIN, &replay_gain) != ESP_OK) {
        s_replay_gain = (CONFIG_STORE_REPLAY_GAIN_DEFAULT != 0);
    } else {
        s_replay_gain = (replay_gain != 0);
    }
    /* 关机前播放状态：无记录（首次烧录/升级）时保持默认 true（自动播放） */
    uint8_t was_play = 1;
    if (nvs_get_u8(s_nvs, KEY_WAS_PLAYING, &was_play) != ESP_OK) {
        s_was_playing = true;
    } else {
        s_was_playing = (was_play != 0);
    }
    uint8_t valid = 0;
    /* ⚠ 秒数位置（旧键 last_pos）不读取也不保存：NightPlayer 机制只记"当前
       歌曲"，开机从歌曲开头开始（该键历史上的值恒为 0） */
    if (nvs_get_u8(s_nvs, KEY_LAST_VALID, &valid) != ESP_OK || valid == 0) {
        s_live.last_valid = false;
    } else {
        s_live.last_valid = true;
        if (nvs_get_u32(s_nvs, KEY_LAST_GLOBAL, &s_live.last_global) != ESP_OK) {
            s_live.last_global = 0;
        }
    }

    /* EQ 预设选择：文件名 + 参数 blob 成对读取；只有一边（半写/手动改坏）
       视为无记录（后续用户重选会覆盖，不阻塞启动） */
    s_live.eq_name[0] = '\0';
    s_live.eq_valid = false;
    memset(&s_live.eq_data, 0, sizeof(s_live.eq_data));
    size_t len = EQ_FILE_NAME_MAX;
    if (nvs_get_str(s_nvs, KEY_EQ_NAME, s_live.eq_name, &len) == ESP_OK &&
        s_live.eq_name[0] != '\0') {
        size_t dlen = sizeof(s_live.eq_data);
        if (nvs_get_blob(s_nvs, KEY_EQ_DATA, &s_live.eq_data, &dlen) == ESP_OK &&
            dlen == sizeof(s_live.eq_data) &&
            s_live.eq_data.band_count <= EQ_MAX_BANDS &&
            s_live.eq_data.band_count > 0) {
            s_live.eq_valid = true;
        } else {
            s_live.eq_name[0] = '\0';   /* 数据缺失/长度不符：视为无 EQ */
        }
    } else {
        s_live.eq_name[0] = '\0';
    }

    /* 配置槽位：把"当前活跃槽位"的内容整份覆盖到上面读到的可切换字段上
       （空槽位/首次升级无槽位记录时保持旧全局键的值，见 config_store.h） */
    config_store_load_active_slot();

    s_inited = true;
    ESP_LOGI(TAG, "config loaded: vol=%u bright=%u loop=%u shuffle=%d "
                  "sleep_def=%u min def_on=%d awake=%d screen_off=%u s "
                  "clkwheel=%d(rev=%d spd=%d) "
                  "touch_test=%d was_playing=%d last=%s(global=%u) shuf_pos=%s eq=%s(%u bands) slot=%u(%s)",
             (unsigned)s_live.volume, (unsigned)s_brightness,
             (unsigned)s_live.loop_mode,
             (int)s_live.shuffle, (unsigned)s_sleep_default_min,
             (int)s_sleep_def_on,
             (int)s_sleep_awake, (unsigned)s_screen_off,
             (int)s_click_wheel, (int)s_cw_reverse, (unsigned)s_cw_speed,
             (int)s_touch_test,
             (int)s_was_playing,
             s_live.last_valid ? "yes" : "no",
             (unsigned)s_live.last_global,
             s_live.shuf_pos_valid ? "yes" : "no",
             s_live.eq_valid ? s_live.eq_name : "none",
             s_live.eq_valid ? (unsigned)s_live.eq_data.band_count : 0u,
             (unsigned)(s_slot_active + 1u),
             s_slot_active_saved ? "saved" : "legacy/empty");
}

/* ------------------------------------------------------------------ */
/* 配置槽位（SAVE1~SAVE4）：存储层实现                                  */
/* ------------------------------------------------------------------ */

uint32_t config_store_slot_count(void)
{
    return CONFIG_STORE_SLOT_COUNT;
}

uint32_t config_store_get_active_slot(void)
{
    return s_slot_active;
}

void config_store_set_active_slot(uint32_t idx)
{
    if (idx >= CONFIG_STORE_SLOT_COUNT || idx == s_slot_active) {
        return;
    }
    s_slot_active = idx;
    cfg_write_u8(KEY_ACTIVE_SLOT, (uint8_t)idx);
    ESP_LOGI(TAG, "active slot -> %u", (unsigned)(idx + 1u));
}

void config_store_load_active_slot(void)
{
    uint8_t idx = 0;
    if (nvs_get_u8(s_nvs, KEY_ACTIVE_SLOT, &idx) != ESP_OK ||
        idx >= CONFIG_STORE_SLOT_COUNT) {
        idx = 0;   /* 无记录/脏数据：默认回到槽位 1 */
    }
    s_slot_active = idx;
    /* 载入槽位内容；空槽位时保持 config_store_init 读到的旧全局键值。
       ⚠ 不改活跃下标（已直接赋值），故不写 NVS、不通知——本函数在
       config_store_init 内部调用，此时尚无回调注册。 */
    if (slot_cache_load(idx)) {
        slot_unpack(&s_slot_cache[idx]);
        s_slot_active_saved = true;
        ESP_LOGI(TAG, "active slot %u restored (vol=%u loop=%u shuffle=%d "
                      "last=%s(%u) eq=%s)",
                 (unsigned)(idx + 1u), (unsigned)s_live.volume,
                 (unsigned)s_live.loop_mode, (int)s_live.shuffle,
                 s_live.last_valid ? "yes" : "no", (unsigned)s_live.last_global,
                 s_live.eq_valid ? s_live.eq_name : "none");
    } else {
        s_slot_active_saved = false;
        ESP_LOGI(TAG, "active slot %u empty: keep legacy global keys",
                 (unsigned)(idx + 1u));
    }
}

bool config_store_get_slot_info(uint32_t idx, config_store_slot_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (idx >= CONFIG_STORE_SLOT_COUNT) {
        return false;
    }
    if (!slot_cache_load(idx)) {
        return false;
    }
    const config_slot_blob_t *b = &s_slot_cache[idx];
    if (out != NULL) {
        out->has_data     = true;
        out->last_valid   = (b->last_valid != 0);
        out->last_global  = b->last_global;
        snprintf(out->list_name, sizeof(out->list_name), "%s", b->last_list_name);
    }
    return true;
}

void config_store_get_slot_snapshot(uint32_t idx, config_store_slot_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (idx >= CONFIG_STORE_SLOT_COUNT || !slot_cache_load(idx)) {
        out->has_data = false;
        return;
    }
    const config_slot_blob_t *b = &s_slot_cache[idx];
    out->has_data           = true;
    out->last_valid         = (b->last_valid != 0);
    out->last_global        = b->last_global;
    out->volume             = b->volume;
    out->loop_mode          = b->loop_mode;
    out->shuffle            = (b->shuffle != 0);
    out->shuffle_pos_valid  = (b->shuffle_pos_valid != 0);
    out->shuffle_pos        = b->shuffle_pos;
    out->eq_valid           = (b->eq_valid != 0) && (b->eq_band_count > 0) &&
                              (b->eq_band_count <= EQ_MAX_BANDS);
    snprintf(out->list_name, sizeof(out->list_name), "%s", b->last_list_name);
    if (out->eq_valid) {
        snprintf(out->eq_name, sizeof(out->eq_name), "%s", b->eq_name);
        out->eq_data = b->eq_data;
    }
}

/* 把内存配置整块写回活跃槽位。
   commit=false 时只 set 不落盘，由调用方与其它键合并成一次 commit
   （切槽位：旧槽位 blob + slot_active 一次提交，省一次 4KB 页写）。 */
static void slot_save_active(bool commit)
{
    if (s_nvs == 0 || s_slot_active >= CONFIG_STORE_SLOT_COUNT) {
        return;
    }
    /* 列表名快照已经和 last_global 一起放在 s_live 里（由
       config_store_set_slot_list_name 写入），这里整块打包即可——
       不再从槽位缓存里取名字，也不受"槽位是否已有数据"影响 */
    config_slot_blob_t b;
    slot_pack(&b);
    slot_cache_store(s_slot_active, &b, commit);
    s_slot_active_saved = true;
    ESP_LOGI(TAG, "SAVE%u saved to NVS: vol=%u loop=%u shuffle=%d "
                  "last=%u(%s) list=%s eq=%s",
             (unsigned)(s_slot_active + 1u), (unsigned)b.volume,
             (unsigned)b.loop_mode, (int)b.shuffle,
             (unsigned)b.last_global,
             b.last_valid ? "valid" : "none",
             b.last_list_name[0] ? b.last_list_name : "-",
             b.eq_valid ? b.eq_name : "none");
}

void config_store_save_active_slot(void)
{
    /* 写入的 blob 已包含当前音量，延迟落盘标记作废（否则 flush 会再写一遍） */
    s_volume_dirty = false;
    slot_save_active(true);
}

void config_store_set_slot_list_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || s_slot_active >= CONFIG_STORE_SLOT_COUNT) {
        return;
    }
    /* ⚠ 只改内存里的活跃配置（纯展示字段），**不写 NVS**：避免每次切歌都写
       flash；随下一次 config_store_save_active_slot 一起落盘。
       2026-11 修复：旧实现把名字写进"槽位缓存"，且要求槽位已存在数据，
       导致空槽位第一次保存时列表名丢失（弹窗只剩 "SAVE2"）。 */
    if (strcmp(s_live.list_name, name) == 0) {
        return;
    }
    snprintf(s_live.list_name, sizeof(s_live.list_name), "%s", name);
}

void config_store_switch_slot(uint32_t idx)
{
    if (idx >= CONFIG_STORE_SLOT_COUNT || idx == s_slot_active) {
        return;
    }
    /* 0. 延迟落盘的亮度先写进 NVS 的 RAM 页缓存（不提交，稍后与下面两个键
          合并成一次 flash 写）；音量标记作废——旧槽位 blob 里会写入当前音量。 */
    if (s_bright_dirty) {
        s_bright_dirty = false;
        cfg_set_u8(KEY_BRIGHTNESS, s_brightness);
    }
    s_volume_dirty = false;

    /* 1. 先把当前配置存回旧槽位（blob 只 set，不提交） */
    slot_save_active(false);
    /* 2. 换活跃下标并记忆到 NVS（与上面的 blob 一次提交，省一次 4KB 页写） */
    s_slot_active = idx;
    cfg_set_u8(KEY_ACTIVE_SLOT, (uint8_t)idx);
    cfg_commit();
    /* 3. 载入目标槽位内容（空槽位 → 出厂默认 + 无位置记录） */
    slot_load_into_live(idx, true, "switch: load new slot");
    /* 4. 通知 UI（设置页等按新槽位的值刷新） */
    config_notify();
}

uint8_t config_store_get_volume(void)
{
    return s_live.volume;
}

uint8_t config_store_get_brightness(void)
{
    return s_brightness;
}

uint8_t config_store_get_loop_mode(void)
{
    return s_live.loop_mode;
}

bool config_store_get_shuffle(void)
{
    return s_live.shuffle;
}

bool config_store_get_shuffle_pos(uint64_t *pos)
{
    if (!s_live.shuf_pos_valid) {
        return false;
    }
    if (pos != NULL) {
        *pos = s_live.shuf_pos;
    }
    return true;
}

uint32_t config_store_get_sleep_default_min(void)
{
    return s_sleep_default_min;
}

bool config_store_get_sleep_def_on(void)
{
    return s_sleep_def_on;
}

bool config_store_get_sleep_awake(void)
{
    return s_sleep_awake;
}

uint32_t config_store_get_screen_off_s(void)
{
    return s_screen_off;
}

bool config_store_get_click_wheel(void)
{
    return s_click_wheel;
}

bool config_store_get_click_wheel_reverse(void)
{
    return s_cw_reverse;
}

uint8_t config_store_get_click_wheel_speed(void)
{
    return s_cw_speed;
}

bool config_store_get_replay_gain(void)
{
    return s_replay_gain;
}

bool config_store_get_touch_test(void)
{
    return s_touch_test;
}

bool config_store_get_was_playing(void)
{
    return s_was_playing;
}

bool config_store_get_last_song(uint32_t *global)
{
    if (!s_live.last_valid) {
        return false;
    }
    if (global != NULL) {
        *global = s_live.last_global;
    }
    return true;
}

void config_store_set_volume(uint8_t vol)
{
    if (vol > CONFIG_STORE_VOLUME_MAX) {
        vol = CONFIG_STORE_VOLUME_MAX;
    }
    /* 值没变且没有挂着的延迟落盘 → 无事可做；若挂着延迟标记（拖动中刚改过
       内存），则本次调用正是"立即落盘"的时机，继续往下走。 */
    if (vol == s_live.volume && !s_volume_dirty) {
        return;
    }
    const bool changed = (vol != s_live.volume);
    s_live.volume = vol;
    s_volume_dirty = false;   /* 本函数立即落盘 */
    slot_persist_live();
    if (changed) {
        config_notify();
    }
}

/* 延迟版：只改内存 + 置脏（拖动中每帧调用），落盘交给 flush_pending。
   见 config_store.h 的“延迟落盘”说明与 ideas_and_exps/NVS_WRITE_BUDGET.md */
void config_store_set_volume_deferred(uint8_t vol)
{
    if (vol > CONFIG_STORE_VOLUME_MAX) {
        vol = CONFIG_STORE_VOLUME_MAX;
    }
    if (vol == s_live.volume) {
        return;
    }
    s_live.volume = vol;
    s_volume_dirty = true;
    s_volume_dirty_us = esp_timer_get_time();
    config_notify();
}

/* 亮度钳位（下限防全黑看不清） */
static uint8_t brightness_clamp(uint8_t brightness)
{
    return (brightness < CONFIG_STORE_BRIGHTNESS_MIN)
               ? (uint8_t)CONFIG_STORE_BRIGHTNESS_MIN
               : brightness;
}

void config_store_set_brightness(uint8_t brightness)
{
    /* 亮度下限：不允许存到 CONFIG_STORE_BRIGHTNESS_MIN 以下（防全黑看不清） */
    brightness = brightness_clamp(brightness);
    if (brightness == s_brightness && !s_bright_dirty) {
        return;
    }
    const bool changed = (brightness != s_brightness);
    s_brightness = brightness;
    s_bright_dirty = false;   /* 本函数立即落盘 */
    cfg_write_u8(KEY_BRIGHTNESS, brightness);
    if (changed) {
        config_notify();
    }
}

/* 延迟版：只改内存 + 置脏（亮度条拖动中调用），落盘交给 flush_pending */
void config_store_set_brightness_deferred(uint8_t brightness)
{
    brightness = brightness_clamp(brightness);
    if (brightness == s_brightness) {
        return;
    }
    s_brightness = brightness;
    s_bright_dirty = true;
    s_bright_dirty_us = esp_timer_get_time();
    config_notify();
}

/* 把延迟落盘的字段写入 NVS。
   每次真正落盘打一行日志（低频：一次拖动只会在松手/静默后打一次），用于
   从串口确认“拖动期间没有写 flash、只在松手后写了一次”。
   ⚠ 必须在栈位于内部 RAM 的任务里调用（写 flash 会关外部缓存，PSRAM 栈
     会命中 IDF 断言），见 FLASH_WRITE_VS_PSRAM.md §五。 */
void config_store_flush_pending(bool force)
{
    const int64_t now = esp_timer_get_time();

    if (s_bright_dirty &&
        (force || (now - s_bright_dirty_us) >= CONFIG_STORE_FLUSH_QUIET_US)) {
        /* 先清标志再写：写的过程中若有新的 deferred 调用，会重新置脏，
           下一轮再写，不会丢 */
        s_bright_dirty = false;
        ESP_LOGI(TAG, "flush deferred brightness=%u (%s)",
                 (unsigned)s_brightness, force ? "force" : "quiet");
        cfg_write_u8(KEY_BRIGHTNESS, s_brightness);
    }

    if (s_volume_dirty &&
        (force || (now - s_volume_dirty_us) >= CONFIG_STORE_FLUSH_QUIET_US)) {
        s_volume_dirty = false;
        ESP_LOGI(TAG, "flush deferred volume=%u (%s)",
                 (unsigned)s_live.volume, force ? "force" : "quiet");
        slot_persist_live();   /* 整块写回活跃槽位（兼容态下同时写旧全局键） */
    }
}

void config_store_set_loop_mode(uint8_t mode)
{
    if (mode == s_live.loop_mode) {
        return;
    }
    s_live.loop_mode = mode;
    slot_persist_live();
    config_notify();
}

void config_store_set_shuffle(bool on)
{
    if (on == s_live.shuffle) {
        return;
    }
    s_live.shuffle = on;
    slot_persist_live();
    config_notify();
}

/* 只改内存的 random 位置 setter（合并写用，见 config_store.h 说明） */
bool config_store_set_shuffle_pos_ram(uint64_t pos)
{
    if (s_live.shuf_pos_valid && s_live.shuf_pos == pos) {
        return false;
    }
    s_live.shuf_pos = pos;
    s_live.shuf_pos_valid = true;
    return true;
}

void config_store_set_shuffle_pos(uint64_t pos)
{
    /* 值+有效标志都没变就直接返回（2026-11）：本 setter 是**每次切槽位/暂停/
       关机都会调**的高频接口，而每次调用都会整块重写槽位 blob。切槽位时
       player_controller 的 CMD_RESTORE_CONFIG 会连续两次写入同一个随机位置
       （先写快照值、再写 adopt 后的值），没有这条短路就会白写两遍 flash。 */
    if (config_store_set_shuffle_pos_ram(pos)) {
        /* 内部状态（随机序列现场），UI 不展示，无需通知回调；随槽位一起落盘 */
        slot_persist_live();
    }
}

void config_store_set_sleep_default_min(uint32_t minutes)
{
    if (minutes == s_sleep_default_min) {
        return;
    }
    s_sleep_default_min = minutes;
    /* 全局键（不进槽位 blob）：睡眠定时时长与配置槽位互相独立 */
    cfg_write_u32(KEY_SLEEP_DEFAULT, minutes);
    config_notify();
}

void config_store_set_sleep_def_on(bool on)
{
    if (on == s_sleep_def_on) {
        return;
    }
    s_sleep_def_on = on;
    cfg_write_u8(KEY_SLEEP_DEF_ON, on ? 1 : 0);
    config_notify();
}

void config_store_set_sleep_awake(bool on)
{
    if (on == s_sleep_awake) {
        return;
    }
    s_sleep_awake = on;
    cfg_write_u8(KEY_SLEEP_AWAKE, on ? 1 : 0);
    config_notify();
}

void config_store_set_screen_off_s(uint32_t seconds)
{
    /* 收敛到合理范围：5 秒 ~ 1 小时；0 表示不自动息屏 */
    if (seconds != 0) {
        if (seconds < 5) seconds = 5;
        if (seconds > 3600) seconds = 3600;
    }
    if (seconds == s_screen_off) {
        return;
    }
    s_screen_off = seconds;
    cfg_write_u32(KEY_SCREEN_OFF, seconds);
    config_notify();
}

/* 开发者选项 setter：只写内存 + NVS，不通知回调——开关是"开机检查、重启
   生效"的启动项，界面上无其它实时同步方（与 set_was_playing 的处理一致；
   开发选项页自身的状态文字由点击回调就地刷新）。 */

void config_store_set_touch_test(bool on)
{
    if (on == s_touch_test) {
        return;
    }
    s_touch_test = on;
    cfg_write_u8(KEY_TOUCH_TEST, on ? 1 : 0);
}

/* 回放增益总开关：立即生效（控制器轮询到变化后重新应用到音频链路，且设置页
   文本随之刷新），故通知回调 */
void config_store_set_replay_gain(bool on)
{
    if (on == s_replay_gain) {
        return;
    }
    s_replay_gain = on;
    cfg_write_u8(KEY_REPLAY_GAIN, on ? 1 : 0);
    config_notify();
}

/* 虚拟 ClickWheel 开关：立即生效（UI 侧回调隐藏/显示速滑按钮），故通知回调 */
void config_store_set_click_wheel(bool on)
{
    if (on == s_click_wheel) {
        return;
    }
    s_click_wheel = on;
    cfg_write_u8(KEY_CLICK_WHEEL, on ? 1 : 0);
    config_notify();
}

/* 滑动方向（true=反转）：立即生效（UI 侧回调 click_wheel_set_reverse），通知回调 */
void config_store_set_click_wheel_reverse(bool reverse)
{
    if (reverse == s_cw_reverse) {
        return;
    }
    s_cw_reverse = reverse;
    cfg_write_u8(KEY_CW_REVERSE, reverse ? 1 : 0);
    config_notify();
}

/* 速度档位（普通/快速/极速）：立即生效（UI 侧回调 click_wheel_set_speed），通知回调 */
void config_store_set_click_wheel_speed(uint8_t speed)
{
    if (speed >= CONFIG_STORE_CW_SPEED_COUNT) {
        speed = CONFIG_STORE_CW_SPEED_DEFAULT;   /* 越界钳到"普通" */
    }
    if (speed == s_cw_speed) {
        return;
    }
    s_cw_speed = speed;
    cfg_write_u8(KEY_CW_SPEED, speed);
    config_notify();
}

void config_store_set_was_playing(bool playing)
{
    if (playing == s_was_playing) {
        return;
    }
    s_was_playing = playing;
    /* 非切换字段（全局共用）：仍写旧全局键，不属于任何槽位 */
    cfg_write_u8(KEY_WAS_PLAYING, playing ? 1 : 0);
}

/* 只改内存的 last_song setter（合并写用，见 config_store.h 说明） */
bool config_store_set_last_song_ram(uint32_t global)
{
    if (s_live.last_valid && s_live.last_global == global) {
        return false;
    }
    s_live.last_global = global;
    s_live.last_valid = true;
    return true;
}

void config_store_set_last_song(uint32_t global)
{
    /* 值+有效标志都没变就直接返回（2026-11）：切槽位前/伪关机前的
       player_controller_sync_config() 会无条件调本函数，若歌曲没变就没有
       必要重写整个槽位 blob（真正必须落盘的那次由
       config_store_switch_slot / save_active_slot 自己完成）。 */
    if (config_store_set_last_song_ram(global)) {
        /* 位置类记录无 UI 展示，不通知回调；随槽位一起落盘 */
        slot_persist_live();
    }
}

bool config_store_get_eq(char *name, size_t name_size, eq_preset_t *preset_out)
{
    if (name != NULL && name_size > 0) {
        snprintf(name, name_size, "%s", s_live.eq_name);
    }
    if (preset_out != NULL) {
        if (s_live.eq_valid) {
            *preset_out = s_live.eq_data;
        } else {
            memset(preset_out, 0, sizeof(*preset_out));
        }
    }
    return s_live.eq_valid;
}

void config_store_set_eq(const char *name, const eq_preset_t *preset)
{
    /* 无 EQ 语义：name 空/NULL 或预设为空（band_count=0）→ 清除记录 */
    if (preset == NULL || preset->band_count == 0 || name == NULL ||
        name[0] == '\0') {
        bool had = s_live.eq_valid;
        s_live.eq_name[0] = '\0';
        s_live.eq_valid = false;
        memset(&s_live.eq_data, 0, sizeof(s_live.eq_data));
        slot_persist_live();   /* 槽位落盘；兼容态下同时清掉旧全局键 */
        if (had) {
            ESP_LOGI(TAG, "EQ cleared");
            config_notify();
        }
        return;
    }

    /* 截断超长文件名（防御：UI 扫描已保证 ≤ EQ_FILE_NAME_MAX-1） */
    char name_buf[EQ_FILE_NAME_MAX];
    snprintf(name_buf, sizeof(name_buf), "%s", name);

    bool changed = !s_live.eq_valid ||
                   strcmp(s_live.eq_name, name_buf) != 0 ||
                   memcmp(&s_live.eq_data, preset, sizeof(s_live.eq_data)) != 0;

    snprintf(s_live.eq_name, sizeof(s_live.eq_name), "%s", name_buf);
    s_live.eq_data = *preset;
    s_live.eq_valid = true;

    slot_persist_live();
    if (changed) {
        ESP_LOGI(TAG, "EQ set: %s (%u bands)", s_live.eq_name,
                 (unsigned)s_live.eq_data.band_count);
        config_notify();
    }
}

void config_store_reset_position(void)
{
    /* 内存标志全部置"无记录"，下次开机走"从第一首/重新随机"分支。
       位置类记录无 UI 展示，不通知变更回调 */
    s_live.last_valid = false;
    s_live.last_global = 0;
    s_live.shuf_pos_valid = false;
    s_live.shuf_pos = 0;
    /* 写回槽位（空槽位仍处于兼容态：写旧全局键的 valid=0）。保留旧值可在
       误操作后人工找回，不影响行为 */
    slot_persist_live();
    ESP_LOGI(TAG, "position reset: last song & shuffle pos cleared");
}

void config_store_register_cb(config_store_cb_t cb, void *ctx)
{
    if (cb == NULL || s_cb_count >= CONFIG_STORE_MAX_CB) {
        return;
    }
    /* 同名回调+上下文重复注册：覆盖旧项，避免重复通知 */
    for (uint8_t i = 0; i < s_cb_count; i++) {
        if (s_cbs[i].cb == cb && s_cbs[i].ctx == ctx) {
            return;
        }
    }
    s_cbs[s_cb_count].cb = cb;
    s_cbs[s_cb_count].ctx = ctx;
    s_cb_count++;
}
