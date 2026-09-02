#include "core/storage/config_store.h"

#include "esp_log.h"
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
#define KEY_BATT_LOG         "battlog"
#define KEY_BATT_LOG_MIN     "battmin"
#define KEY_TOUCH_TEST       "touchtest"
#define KEY_WAS_PLAYING      "was_play"
#define KEY_LAST_GLOBAL      "last_glob"
#define KEY_LAST_POS         "last_pos"
#define KEY_LAST_VALID       "last_valid"

/* 变更回调容量（设置页 + 通知栏等，够用） */
#define CONFIG_STORE_MAX_CB  4

/* ---------- 内存中的配置值（开机由 NVS 加载） ---------- */

static bool s_inited = false;
static nvs_handle_t s_nvs = 0;

static uint8_t  s_volume      = CONFIG_STORE_VOLUME_DEFAULT;
static uint8_t  s_brightness  = CONFIG_STORE_BRIGHTNESS_DEFAULT;
static uint8_t  s_loop_mode   = CONFIG_STORE_LOOP_DEFAULT;
static bool     s_shuffle     = false;
/* 随机播放位置（uint64 拆高/低 32 位存储；valid 标志区分"无记录"） */
static uint32_t s_shuf_pos_hi = 0;
static uint32_t s_shuf_pos_lo = 0;
static bool     s_shuf_pos_valid = false;
static uint32_t s_sleep_def   = CONFIG_STORE_SLEEP_DEFAULT_MIN;
static bool     s_sleep_def_on = CONFIG_STORE_SLEEP_DEF_ON_DEFAULT;
static bool     s_sleep_awake = CONFIG_STORE_SLEEP_AWAKE_DEFAULT;
static uint32_t s_screen_off  = CONFIG_STORE_SCREEN_OFF_DEFAULT;
/* 开发者选项（运行时开关，开机检查、重启生效）：电池电压日志（默认关）
   及其采样间隔（分钟，默认 5）、触摸测试白点（默认关） */
static bool     s_batt_log    = (CONFIG_STORE_BATT_LOG_DEFAULT != 0);
static uint8_t  s_batt_log_min = CONFIG_STORE_BATT_LOG_MIN_DEFAULT;
static bool     s_touch_test  = (CONFIG_STORE_TOUCH_TEST_DEFAULT != 0);
/* 关机前播放状态：默认 true（无记录时开机自动播放，保持旧行为） */
static bool     s_was_playing = true;
static uint32_t s_last_global = 0;
static uint32_t s_last_pos    = 0;
static bool     s_last_valid  = false;

/* 变更回调表 */
typedef struct {
    config_store_cb_t cb;
    void *ctx;
} config_cb_entry_t;

static config_cb_entry_t s_cbs[CONFIG_STORE_MAX_CB];
static uint8_t s_cb_count = 0;

/* ------------------------------------------------------------------ */
/* NVS 读写辅助                                                        */
/* ------------------------------------------------------------------ */

/* 写入失败不致命：仅打日志，内存值仍有效（本次会话内生效，重启后丢失） */
static void cfg_write_u8(const char *key, uint8_t value)
{
    if (s_nvs == 0) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(err));
        return;
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", key, esp_err_to_name(err));
    }
}

static void cfg_write_u32(const char *key, uint32_t value)
{
    if (s_nvs == 0) {
        return;
    }
    esp_err_t err = nvs_set_u32(s_nvs, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(%s) failed: %s", key, esp_err_to_name(err));
        return;
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", key, esp_err_to_name(err));
    }
}

/* 通知所有已注册的变更回调（在调用者线程上下文同步执行） */
static void config_notify(void)
{
    for (uint8_t i = 0; i < s_cb_count; i++) {
        if (s_cbs[i].cb != NULL) {
            s_cbs[i].cb(s_cbs[i].ctx);
        }
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
    if (nvs_get_u8(s_nvs, KEY_VOLUME, &s_volume) != ESP_OK) {
        s_volume = CONFIG_STORE_VOLUME_DEFAULT;
    } else if (s_volume > CONFIG_STORE_VOLUME_MAX) {
        uint8_t old_pct = s_volume;   /* 旧版本存储的 0~100 百分比 */
        if (old_pct == 0) {
            s_volume = CONFIG_STORE_VOLUME_MIN;
        } else if (old_pct <= 20) {
            s_volume = 1;             /* 旧 1~20% 全部归到最小档 1（原配置下几乎无声） */
        } else if (old_pct >= 80) {
            s_volume = CONFIG_STORE_VOLUME_MAX;
        } else {
            s_volume = (uint8_t)(((uint32_t)(old_pct - 20) * 29u / 60u) + 1u);
        }
        cfg_write_u8(KEY_VOLUME, s_volume);   /* 写回迁移结果，下次直接读到新档位 */
    }
    if (nvs_get_u8(s_nvs, KEY_BRIGHTNESS, &s_brightness) != ESP_OK) {
        s_brightness = CONFIG_STORE_BRIGHTNESS_DEFAULT;
    }
    /* 亮度下限：历史 NVS 可能存过极低亮度（如调试期滑到 0），
       钳制到 CONFIG_STORE_BRIGHTNESS_MIN，避免开机屏幕完全看不清 */
    if (s_brightness < CONFIG_STORE_BRIGHTNESS_MIN) {
        s_brightness = CONFIG_STORE_BRIGHTNESS_MIN;
    }
    if (nvs_get_u8(s_nvs, KEY_LOOP_MODE, &s_loop_mode) != ESP_OK) {
        s_loop_mode = CONFIG_STORE_LOOP_DEFAULT;
    }
    uint8_t shuffle = 0;
    if (nvs_get_u8(s_nvs, KEY_SHUFFLE, &shuffle) != ESP_OK) {
        s_shuffle = false;
    } else {
        s_shuffle = (shuffle != 0);
    }
    /* 随机播放位置：valid 标志为 1 才读取高/低 32 位 */
    uint8_t shuf_pos_valid = 0;
    if (nvs_get_u8(s_nvs, KEY_SHUFFLE_POS_VAL, &shuf_pos_valid) == ESP_OK
            && shuf_pos_valid != 0) {
        s_shuf_pos_valid = true;
        if (nvs_get_u32(s_nvs, KEY_SHUFFLE_POS_HI, &s_shuf_pos_hi) != ESP_OK) {
            s_shuf_pos_hi = 0;
        }
        if (nvs_get_u32(s_nvs, KEY_SHUFFLE_POS_LO, &s_shuf_pos_lo) != ESP_OK) {
            s_shuf_pos_lo = 0;
        }
    } else {
        s_shuf_pos_valid = false;
    }
    if (nvs_get_u32(s_nvs, KEY_SLEEP_DEFAULT, &s_sleep_def) != ESP_OK) {
        s_sleep_def = CONFIG_STORE_SLEEP_DEFAULT_MIN;
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
    /* 开发者选项：无记录（首次烧录/升级）时默认关闭（见 CONFIG_STORE_*_DEFAULT） */
    uint8_t batt_log = 0;
    if (nvs_get_u8(s_nvs, KEY_BATT_LOG, &batt_log) != ESP_OK) {
        s_batt_log = (CONFIG_STORE_BATT_LOG_DEFAULT != 0);
    } else {
        s_batt_log = (batt_log != 0);
    }
    if (nvs_get_u8(s_nvs, KEY_BATT_LOG_MIN, &s_batt_log_min) != ESP_OK) {
        s_batt_log_min = CONFIG_STORE_BATT_LOG_MIN_DEFAULT;
    }
    /* 间隔越界防护：历史脏数据钳制到 1~120 分钟 */
    if (s_batt_log_min < 1) {
        s_batt_log_min = 1;
    } else if (s_batt_log_min > CONFIG_STORE_BATT_LOG_MIN_MAX) {
        s_batt_log_min = CONFIG_STORE_BATT_LOG_MIN_MAX;
    }
    uint8_t touch_test = 0;
    if (nvs_get_u8(s_nvs, KEY_TOUCH_TEST, &touch_test) != ESP_OK) {
        s_touch_test = (CONFIG_STORE_TOUCH_TEST_DEFAULT != 0);
    } else {
        s_touch_test = (touch_test != 0);
    }
    /* 关机前播放状态：无记录（首次烧录/升级）时保持默认 true（自动播放） */
    uint8_t was_play = 1;
    if (nvs_get_u8(s_nvs, KEY_WAS_PLAYING, &was_play) != ESP_OK) {
        s_was_playing = true;
    } else {
        s_was_playing = (was_play != 0);
    }
    uint8_t valid = 0;
    if (nvs_get_u8(s_nvs, KEY_LAST_VALID, &valid) != ESP_OK || valid == 0) {
        s_last_valid = false;
    } else {
        s_last_valid = true;
        if (nvs_get_u32(s_nvs, KEY_LAST_GLOBAL, &s_last_global) != ESP_OK) {
            s_last_global = 0;
        }
        if (nvs_get_u32(s_nvs, KEY_LAST_POS, &s_last_pos) != ESP_OK) {
            s_last_pos = 0;
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "config loaded: vol=%u bright=%u loop=%u shuffle=%d "
                  "sleep_def=%u min def_on=%d awake=%d screen_off=%u s "
                  "batt_log=%d(%u min) touch_test=%d was_playing=%d last=%s(global=%u,pos=%u) shuf_pos=%s",
             (unsigned)s_volume, (unsigned)s_brightness, (unsigned)s_loop_mode,
             (int)s_shuffle, (unsigned)s_sleep_def, (int)s_sleep_def_on,
             (int)s_sleep_awake, (unsigned)s_screen_off,
             (int)s_batt_log, (unsigned)s_batt_log_min,
             (int)s_touch_test,
             (int)s_was_playing,
             s_last_valid ? "yes" : "no",
             (unsigned)s_last_global, (unsigned)s_last_pos,
             s_shuf_pos_valid ? "yes" : "no");
}

uint8_t config_store_get_volume(void)
{
    return s_volume;
}

uint8_t config_store_get_brightness(void)
{
    return s_brightness;
}

uint8_t config_store_get_loop_mode(void)
{
    return s_loop_mode;
}

bool config_store_get_shuffle(void)
{
    return s_shuffle;
}

bool config_store_get_shuffle_pos(uint64_t *pos)
{
    if (!s_shuf_pos_valid) {
        return false;
    }
    if (pos != NULL) {
        *pos = ((uint64_t)s_shuf_pos_hi << 32) | s_shuf_pos_lo;
    }
    return true;
}

uint32_t config_store_get_sleep_default_min(void)
{
    return s_sleep_def;
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

bool config_store_get_battery_log(void)
{
    return s_batt_log;
}

uint8_t config_store_get_battery_log_min(void)
{
    return s_batt_log_min;
}

bool config_store_get_touch_test(void)
{
    return s_touch_test;
}

bool config_store_get_was_playing(void)
{
    return s_was_playing;
}

bool config_store_get_last_song(uint32_t *global, uint32_t *pos_s)
{
    if (!s_last_valid) {
        return false;
    }
    if (global != NULL) {
        *global = s_last_global;
    }
    if (pos_s != NULL) {
        *pos_s = s_last_pos;
    }
    return true;
}

void config_store_set_volume(uint8_t vol)
{
    if (vol > CONFIG_STORE_VOLUME_MAX) {
        vol = CONFIG_STORE_VOLUME_MAX;
    }
    if (vol == s_volume) {
        return;
    }
    s_volume = vol;
    cfg_write_u8(KEY_VOLUME, vol);
    config_notify();
}

void config_store_set_brightness(uint8_t brightness)
{
    /* 亮度下限：不允许存到 CONFIG_STORE_BRIGHTNESS_MIN 以下（防全黑看不清） */
    if (brightness < CONFIG_STORE_BRIGHTNESS_MIN) {
        brightness = CONFIG_STORE_BRIGHTNESS_MIN;
    }
    if (brightness == s_brightness) {
        return;
    }
    s_brightness = brightness;
    cfg_write_u8(KEY_BRIGHTNESS, brightness);
    config_notify();
}

void config_store_set_loop_mode(uint8_t mode)
{
    if (mode == s_loop_mode) {
        return;
    }
    s_loop_mode = mode;
    cfg_write_u8(KEY_LOOP_MODE, mode);
    config_notify();
}

void config_store_set_shuffle(bool on)
{
    if (on == s_shuffle) {
        return;
    }
    s_shuffle = on;
    cfg_write_u8(KEY_SHUFFLE, on ? 1 : 0);
    config_notify();
}

void config_store_set_shuffle_pos(uint64_t pos)
{
    s_shuf_pos_hi = (uint32_t)(pos >> 32);
    s_shuf_pos_lo = (uint32_t)(pos & 0xFFFFFFFFu);
    s_shuf_pos_valid = true;
    /* 内部状态（随机序列现场），UI 不展示，无需通知回调 */
    cfg_write_u32(KEY_SHUFFLE_POS_HI, s_shuf_pos_hi);
    cfg_write_u32(KEY_SHUFFLE_POS_LO, s_shuf_pos_lo);
    cfg_write_u8(KEY_SHUFFLE_POS_VAL, 1);
}

void config_store_set_sleep_default_min(uint32_t minutes)
{
    if (minutes == s_sleep_def) {
        return;
    }
    s_sleep_def = minutes;
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

/* 开发者选项三个 setter：只写内存 + NVS，不通知回调——两个开关与间隔都是
   "开机检查、重启生效"的启动项，界面上无其它实时同步方（与 set_was_playing
   的处理一致；开发选项页自身的状态文字由点击回调就地刷新）。 */

void config_store_set_battery_log(bool on)
{
    if (on == s_batt_log) {
        return;
    }
    s_batt_log = on;
    cfg_write_u8(KEY_BATT_LOG, on ? 1 : 0);
}

void config_store_set_battery_log_min(uint8_t minutes)
{
    /* 收敛到 1~120 分钟（UI 档位 1/2/3/5/10，钳位仅防越界脏数据） */
    if (minutes < 1) {
        minutes = 1;
    } else if (minutes > CONFIG_STORE_BATT_LOG_MIN_MAX) {
        minutes = CONFIG_STORE_BATT_LOG_MIN_MAX;
    }
    if (minutes == s_batt_log_min) {
        return;
    }
    s_batt_log_min = minutes;
    cfg_write_u8(KEY_BATT_LOG_MIN, minutes);
}

void config_store_set_touch_test(bool on)
{
    if (on == s_touch_test) {
        return;
    }
    s_touch_test = on;
    cfg_write_u8(KEY_TOUCH_TEST, on ? 1 : 0);
}

void config_store_set_was_playing(bool playing)
{
    if (playing == s_was_playing) {
        return;
    }
    s_was_playing = playing;
    cfg_write_u8(KEY_WAS_PLAYING, playing ? 1 : 0);
}

void config_store_set_last_song(uint32_t global, uint32_t pos_s)
{
    s_last_global = global;
    s_last_pos = pos_s;
    s_last_valid = true;
    if (s_nvs == 0) {
        return;
    }
    cfg_write_u32(KEY_LAST_GLOBAL, global);
    cfg_write_u32(KEY_LAST_POS, pos_s);
    cfg_write_u8(KEY_LAST_VALID, 1);
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
