#include "core/storage/bt_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "bt_store";

/* NVS namespace（≤15 字符）与键名 */
#define BT_NVS_NS        "btdev"
#define BT_NVS_KEY_EN    "en"
/* 每个设备一个 key（d0~d4）：单条 272B，**不再需要 1.3KB 的落盘快照缓冲**
   （内部 RAM 紧张；nvs_set_blob 会在自己的锁内拷贝数据，栈上 272B 临时量足够） */
static const char *const BT_SLOT_KEY[BT_STORE_MAX_DEVICES] = {
    "d0", "d1", "d2", "d3", "d4",
};

/* 音量延迟落盘的静默窗口（与 config_store 一致：500ms） */
#define BT_VOL_FLUSH_QUIET_US  (500 * 1000)

/* 内存中的设备表（blob 直存；valid=false 为空槽，删除时后项前移） */
static bt_device_t s_dev[BT_STORE_MAX_DEVICES];

/* 上次的蓝牙开关状态（默认关：新机/升级后开机不启动蓝牙栈） */
static bool s_enabled = false;

/* 延迟落盘（设备音量高频调节） */
static volatile bool    s_dirty = false;
static volatile int64_t s_dirty_us = 0;

/* 内存数组临界区（NVS 写在区外） */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* NVS 句柄：init 失败/尚未 init 时为 0，所有持久化接口做空操作 */
static nvs_handle_t s_nvs = 0;
static bool s_inited = false;

static bool nvs_ready(void)
{
    return s_inited && s_nvs != 0;
}

/* 安全截断设备名到 BT_STORE_NAME_MAX-1 字节，且不切断 UTF-8 多字节序列 */
static void copy_name(char *dst, const char *src)
{
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, BT_STORE_NAME_MAX, "蓝牙设备");
        return;
    }
    size_t n = strlen(src);
    if (n >= BT_STORE_NAME_MAX) {
        n = BT_STORE_NAME_MAX - 1;
        /* 回退到某个字符的首字节：continuation 字节形如 10xxxxxx */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 把内存设备表整体写入 NVS：逐槽写（每槽 272B 栈临时量），空槽删 key。
   逐槽写的目的是**省掉 1.3KB 的静态落盘缓冲**（内部 RAM 紧张）。 */
static void persist_devices(void)
{
    if (!nvs_ready()) {
        return;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        bt_device_t tmp;
        bool valid;
        taskENTER_CRITICAL(&s_mux);
        valid = s_dev[i].valid;
        if (valid) {
            tmp = s_dev[i];
        }
        taskEXIT_CRITICAL(&s_mux);

        esp_err_t e;
        if (valid) {
            e = nvs_set_blob(s_nvs, BT_SLOT_KEY[i], &tmp, sizeof(tmp));
        } else {
            e = nvs_erase_key(s_nvs, BT_SLOT_KEY[i]);
            if (e == ESP_ERR_NVS_NOT_FOUND) {
                e = ESP_OK;
            }
        }
        if (e != ESP_OK) {
            err = e;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist devices failed: %s", esp_err_to_name(err));
    }
}

void bt_store_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    esp_err_t err = nvs_open(BT_NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", BT_NVS_NS, esp_err_to_name(err));
        s_nvs = 0;
        return;
    }

    uint8_t en = 0;
    if (nvs_get_u8(s_nvs, BT_NVS_KEY_EN, &en) == ESP_OK) {
        s_enabled = (en != 0);
    }

    /* 逐槽读取（d0~d4）；长度不符视为旧布局/损坏，删掉该槽 */
    memset(s_dev, 0, sizeof(s_dev));
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        size_t len = sizeof(bt_device_t);
        err = nvs_get_blob(s_nvs, BT_SLOT_KEY[i], &s_dev[i], &len);
        if (err == ESP_OK && len != sizeof(bt_device_t)) {
            ESP_LOGW(TAG, "slot %d size mismatch (%u), erase", i, (unsigned)len);
            memset(&s_dev[i], 0, sizeof(s_dev[i]));
            nvs_erase_key(s_nvs, BT_SLOT_KEY[i]);
        } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "slot %d read failed: %s", i, esp_err_to_name(err));
            memset(&s_dev[i], 0, sizeof(s_dev[i]));
        }
    }

    /* 兜底修正：非法音量/文件名越界（防止旧数据/损坏数据进入硬件路径） */
    int n = 0;
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        if (!s_dev[i].valid) {
            continue;
        }
        n++;
        if (s_dev[i].volume > 30) {
            s_dev[i].volume = 30;
        }
        s_dev[i].name[BT_STORE_NAME_MAX - 1] = '\0';
        s_dev[i].eq_name[EQ_FILE_NAME_MAX - 1] = '\0';
        if (s_dev[i].eq.band_count > EQ_MAX_BANDS) {
            memset(&s_dev[i].eq, 0, sizeof(s_dev[i].eq));
            s_dev[i].eq_valid = false;
            s_dev[i].eq_name[0] = '\0';
        }
    }
    ESP_LOGI(TAG, "loaded %d device(s), bt_enabled=%d", n, (int)s_enabled);
}

bool bt_store_get_enabled(void)
{
    return s_enabled;
}

void bt_store_set_enabled(bool on)
{
    s_enabled = on;
    if (!nvs_ready()) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_nvs, BT_NVS_KEY_EN, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save bt_enabled failed: %s", esp_err_to_name(err));
    }
}

int bt_store_count(void)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        if (s_dev[i].valid) {
            n++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

bool bt_store_get(int idx, bt_device_t *out)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES) {
        return false;
    }
    bool ok;
    taskENTER_CRITICAL(&s_mux);
    ok = s_dev[idx].valid;
    if (ok && out != NULL) {
        *out = s_dev[idx];
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

int bt_store_find(const uint8_t bda[6])
{
    if (bda == NULL) {
        return -1;
    }
    int found = -1;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        if (s_dev[i].valid && memcmp(s_dev[i].bda, bda, 6) == 0) {
            found = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

int bt_store_add(const uint8_t bda[6], const char *name, uint8_t volume)
{
    if (bda == NULL) {
        return -1;
    }
    if (volume > 30) {
        volume = 30;
    }

    /* 已存在：只更新名字（连接后可能拿到更完整的名字） */
    int idx = bt_store_find(bda);
    if (idx >= 0) {
        bt_store_update_name(idx, name);
        return idx;
    }

    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BT_STORE_MAX_DEVICES; i++) {
        if (!s_dev[i].valid) {
            memset(&s_dev[i], 0, sizeof(s_dev[i]));
            s_dev[i].valid = true;
            memcpy(s_dev[i].bda, bda, 6);
            s_dev[i].volume = volume;
            s_dev[i].quiet_mode = false;   /* 每个设备默认不开启安静模式 */
            copy_name(s_dev[i].name, name);
            idx = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);

    if (idx < 0) {
        ESP_LOGW(TAG, "device table full (%d), refuse new device", BT_STORE_MAX_DEVICES);
        return -1;
    }
    persist_devices();
    ESP_LOGI(TAG, "device added: slot %d, name \"%s\"", idx, name ? name : "");
    return idx;
}

void bt_store_update_name(int idx, const char *name)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES || name == NULL) {
        return;
    }
    bool changed = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_dev[idx].valid && strcmp(s_dev[idx].name, name) != 0) {
        copy_name(s_dev[idx].name, name);
        changed = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (changed) {
        persist_devices();
    }
}

void bt_store_set_volume(int idx, uint8_t vol)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES) {
        return;
    }
    if (vol > 30) {
        vol = 30;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_dev[idx].valid && s_dev[idx].volume != vol) {
        s_dev[idx].volume = vol;
        s_dirty = true;
        s_dirty_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_mux);
}

void bt_store_set_eq(int idx, const char *name, const eq_preset_t *preset)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_dev[idx].valid) {
        if (name != NULL && preset != NULL && preset->band_count > 0) {
            s_dev[idx].eq_valid = true;
            copy_name(s_dev[idx].eq_name, name);
            s_dev[idx].eq = *preset;
        } else {
            s_dev[idx].eq_valid = false;
            s_dev[idx].eq_name[0] = '\0';
            memset(&s_dev[idx].eq, 0, sizeof(s_dev[idx].eq));
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    persist_devices();
}

void bt_store_set_quiet(int idx, bool on)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_dev[idx].valid && s_dev[idx].quiet_mode != on) {
        s_dev[idx].quiet_mode = on;
    }
    taskEXIT_CRITICAL(&s_mux);
    persist_devices();
}

void bt_store_remove(int idx)
{
    if (idx < 0 || idx >= BT_STORE_MAX_DEVICES) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    for (int i = idx; i < BT_STORE_MAX_DEVICES - 1; i++) {
        s_dev[i] = s_dev[i + 1];
    }
    memset(&s_dev[BT_STORE_MAX_DEVICES - 1], 0, sizeof(s_dev[0]));
    taskEXIT_CRITICAL(&s_mux);
    persist_devices();
    ESP_LOGI(TAG, "device slot %d removed", idx);
}

void bt_store_flush_pending(bool force)
{
    if (!s_dirty) {
        return;
    }
    if (!force &&
        esp_timer_get_time() - s_dirty_us < (int64_t)BT_VOL_FLUSH_QUIET_US) {
        return;
    }
    s_dirty = false;
    persist_devices();
}
