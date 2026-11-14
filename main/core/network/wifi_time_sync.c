#include "core/network/wifi_time_sync.h"

#include "core/hardware/rtc.h"
#include "core/hardware/sd_card.h"
#include "core/storage/config_store.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netdb.h"

static const char *TAG = "wifi_time_sync";

/* ------------------------------------------------------------------ */
/* 常量与状态                                                           */
/* ------------------------------------------------------------------ */

/* 事件组位与"最近一次断开原因"：事件回调只做转发，解析留在调用任务 */
#define EVT_WIFI_STARTED      BIT0
#define EVT_WIFI_GOT_IP       BIT1
#define EVT_WIFI_DISCONNECTED BIT2

static EventGroupHandle_t s_evt = NULL;
static volatile uint8_t   s_disconnect_reason = 0;

/* 等待期间的最小检查粒度：决定取消响应速度 */
#define WAIT_SLICE_MS  100

/* ------------------------------------------------------------------ */
/* 进度上报                                                             */
/* ------------------------------------------------------------------ */

static void report(wifi_time_sync_progress_cb_t cb, void *ctx,
                   wifi_time_sync_stage_t stage, int attempt, int max_attempts,
                   int percent, const char *detail)
{
    if (cb != NULL) {
        cb(stage, attempt, max_attempts, percent, detail, ctx);
    }
}

const char *wifi_time_sync_stage_text(wifi_time_sync_stage_t stage)
{
    switch (stage) {
    case WIFI_TIME_SYNC_STAGE_READ_CONFIG: return "正在读取 WiFi 配置…";
    case WIFI_TIME_SYNC_STAGE_WIFI_CONNECT: return "正在连接 WiFi…";
    case WIFI_TIME_SYNC_STAGE_WIFI_IP:      return "正在获取网络地址…";
    case WIFI_TIME_SYNC_STAGE_NTP:          return "正在等待网络时间…";
    case WIFI_TIME_SYNC_STAGE_WRITE_RTC:    return "正在写入时间…";
    case WIFI_TIME_SYNC_STAGE_DONE:         return "对时完成";
    default:                                return "对时中…";
    }
}

const char *wifi_time_sync_result_text(wifi_time_sync_result_t result)
{
    switch (result) {
    case WIFI_TIME_SYNC_OK:
        return "对时成功，时间已更新。";
    case WIFI_TIME_SYNC_CANCELLED:
        return "已取消对时。";
    case WIFI_TIME_SYNC_ERR_NO_SD:
        return "未检测到 SD 卡，无法读取 WiFi 配置。";
    case WIFI_TIME_SYNC_ERR_NO_FILE:
        return "未找到 /sdcard/.ecov/wifi.txt。";
    case WIFI_TIME_SYNC_ERR_BAD_FILE:
        return "wifi.txt 格式不正确（第 1 行必须是 WiFi 名称）。";
    case WIFI_TIME_SYNC_ERR_WIFI_INIT:
        return "WiFi 初始化失败，请重试。";
    case WIFI_TIME_SYNC_ERR_WIFI_NOT_FOUND:
        return "找不到该 WiFi，请检查名称与信号。";
    case WIFI_TIME_SYNC_ERR_WIFI_AUTH:
        return "WiFi 密码错误或认证失败。";
    case WIFI_TIME_SYNC_ERR_WIFI_TIMEOUT:
        return "连接 WiFi 超时，请重试。";
    case WIFI_TIME_SYNC_ERR_NTP_DNS:
        return "无法解析 NTP 服务器域名。";
    case WIFI_TIME_SYNC_ERR_NTP_TIMEOUT:
        return "NTP 服务器无应答，请稍后重试。";
    case WIFI_TIME_SYNC_ERR_RTC:
        return "RTC 写入失败，时间未能保存。";
    default:
        return "对时失败，请重试。";
    }
}

/* ------------------------------------------------------------------ */
/* 配置读取（/sdcard/.ecov/wifi.txt）                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char ssid[33];        /* 802.11 SSID 上限 32 字节 + NUL */
    char password[65];    /* WPA 密码上限 64 字节 + NUL */
    char ntp_server[64];  /* 域名/IP */
} wifi_cfg_t;

/* 去掉行尾 \r（兼容 Windows 换行；\n 在切行时已换成 \0） */
static void strip_cr(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* 去掉两端空白（SSID / 服务器用；密码不裁，避免误伤首尾空格） */
static void trim_ws(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static wifi_time_sync_result_t read_wifi_config(wifi_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted");
        return WIFI_TIME_SYNC_ERR_NO_SD;
    }

    FILE *f = fopen(WIFI_TIME_SYNC_CONFIG_PATH, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s failed", WIFI_TIME_SYNC_CONFIG_PATH);
        return WIFI_TIME_SYNC_ERR_NO_FILE;
    }

    /* 配置文件都是几十字节，512 字节足够；超长视为格式错误 */
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    bool too_long = (n == sizeof(buf) - 1) && (fgetc(f) != EOF);
    fclose(f);
    if (too_long) {
        ESP_LOGW(TAG, "wifi.txt too long");
        return WIFI_TIME_SYNC_ERR_BAD_FILE;
    }
    buf[n] = '\0';

    /* 跳过 UTF-8 BOM */
    char *p = buf;
    if (n >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB
            && (uint8_t)p[2] == 0xBF) {
        p += 3;
    }

    /* 切行：最多取 3 行（第 3 行之后的内容忽略） */
    char *lines[3] = { NULL, NULL, NULL };
    int count = 0;
    while (*p != '\0' && count < 3) {
        lines[count++] = p;
        char *nl = strchr(p, '\n');
        if (nl == NULL) {
            break;
        }
        *nl = '\0';
        p = nl + 1;
    }
    if (count < 1) {
        return WIFI_TIME_SYNC_ERR_BAD_FILE;
    }

    for (int i = 0; i < count; i++) {
        strip_cr(lines[i]);
    }
    trim_ws(lines[0]);

    if (lines[0][0] == '\0') {
        ESP_LOGW(TAG, "wifi.txt: empty SSID");
        return WIFI_TIME_SYNC_ERR_BAD_FILE;
    }
    if (strlen(lines[0]) > 32) {
        ESP_LOGW(TAG, "wifi.txt: SSID too long");
        return WIFI_TIME_SYNC_ERR_BAD_FILE;
    }
    snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", lines[0]);

    if (count >= 2) {
        /* WPA 口令上限 63 字符（64 字节数组要留 NUL）；空 = 开放网络 */
        if (strlen(lines[1]) > 63) {
            ESP_LOGW(TAG, "wifi.txt: password too long");
            return WIFI_TIME_SYNC_ERR_BAD_FILE;
        }
        snprintf(cfg->password, sizeof(cfg->password), "%s", lines[1]);
    }

    if (count >= 3) {
        trim_ws(lines[2]);
        if (lines[2][0] != '\0') {
            if (strlen(lines[2]) >= sizeof(cfg->ntp_server)) {
                ESP_LOGW(TAG, "wifi.txt: ntp server too long");
                return WIFI_TIME_SYNC_ERR_BAD_FILE;
            }
            snprintf(cfg->ntp_server, sizeof(cfg->ntp_server), "%s", lines[2]);
        }
    }
    if (cfg->ntp_server[0] == '\0') {
        snprintf(cfg->ntp_server, sizeof(cfg->ntp_server), "%s",
                 WIFI_TIME_SYNC_DEFAULT_NTP);
    }

    ESP_LOGI(TAG, "wifi config: ssid='%s' (%u bytes) ntp=%s", cfg->ssid,
             (unsigned)strlen(cfg->ssid), cfg->ntp_server);
    return WIFI_TIME_SYNC_OK;
}

/* ------------------------------------------------------------------ */
/* 阻塞等待（带取消检查）                                                */
/* ------------------------------------------------------------------ */

/* 等待事件组 bits（任一命中）或超时；cancel 置位时立即返回 false */
static bool wait_bits(EventBits_t bits, uint32_t timeout_ms,
                      volatile bool *cancel, EventBits_t *out_bits)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (cancel != NULL && *cancel) {
            return false;
        }
        EventBits_t got = xEventGroupWaitBits(s_evt, bits, pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(WAIT_SLICE_MS));
        if (got & bits) {
            if (out_bits != NULL) {
                *out_bits = got & bits;
            }
            return true;
        }
    }
    return false;
}

/* 可取消的延时（重试间隔用） */
static bool delay_cancelable(uint32_t ms, volatile bool *cancel)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (cancel != NULL && *cancel) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(WAIT_SLICE_MS));
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* WiFi 事件                                                            */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    if (s_evt == NULL) {
        return;   /* 已进入 cleanup：卸载回调与事件组之间可能还有在途事件 */
    }
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            xEventGroupSetBits(s_evt, EVT_WIFI_STARTED);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)data;
            s_disconnect_reason = (d != NULL) ? d->reason : 0;
            ESP_LOGW(TAG, "wifi disconnected, reason=%u",
                     (unsigned)s_disconnect_reason);
            xEventGroupSetBits(s_evt, EVT_WIFI_DISCONNECTED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evt, EVT_WIFI_GOT_IP);
    }
}

/* 断开原因归类 */
static bool reason_is_not_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND
        || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
        || reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        || reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static bool reason_is_auth(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE
        || reason == WIFI_REASON_AUTH_FAIL
        || reason == WIFI_REASON_ASSOC_FAIL
        || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* WiFi 连接（最多 WIFI_TIME_SYNC_WIFI_ATTEMPTS 次）                    */
/* ------------------------------------------------------------------ */

/* NTP 阶段在本函数内、WiFi 仍在线时执行（见下方定义） */
static wifi_time_sync_result_t ntp_sync(const char *server,
                                        wifi_time_sync_progress_cb_t cb,
                                        void *ctx, volatile bool *cancel,
                                        time_t *utc_out);

/* 连上 WiFi 后紧接着做 NTP；成功时 *utc_out = UTC 秒。
   ⚠ WiFi/netif 资源必须保持到 NTP 完成，所以两步放在同一个函数里，
     统一在 cleanup 释放。 */
static wifi_time_sync_result_t wifi_sync_online(const wifi_cfg_t *cfg,
                                                wifi_time_sync_progress_cb_t cb,
                                                void *ctx,
                                                volatile bool *cancel,
                                                time_t *utc_out)
{
    esp_netif_t *netif = NULL;
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    bool wifi_inited = false;
    bool wifi_started = false;
    bool handlers_registered = false;
    wifi_time_sync_result_t result = WIFI_TIME_SYNC_ERR_WIFI_INIT;

    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return WIFI_TIME_SYNC_ERR_WIFI_INIT;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "create default event loop: %s", esp_err_to_name(err));
        goto cleanup;
    }
    netif = esp_netif_create_default_wifi_sta();
    if (netif == NULL) {
        ESP_LOGE(TAG, "create default wifi sta failed");
        goto cleanup;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        goto cleanup;
    }
    wifi_inited = true;
    /* 配置只放 RAM：不做 WiFi 配置的 NVS 持久化，退出后不残留 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL,
                                              &wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register wifi event: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL,
                                              &ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register ip event: %s", esp_err_to_name(err));
        goto cleanup;
    }
    handlers_registered = true;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        goto cleanup;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    /* SSID 允许正好 32 字节（不要求 NUL），不能用 snprintf 截成 31 字节 */
    size_t ssid_len = strlen(cfg->ssid);
    if (ssid_len > sizeof(wc.sta.ssid)) {
        ssid_len = sizeof(wc.sta.ssid);
    }
    memcpy(wc.sta.ssid, cfg->ssid, ssid_len);
    size_t pass_len = strlen(cfg->password);
    if (pass_len > sizeof(wc.sta.password) - 1) {
        pass_len = sizeof(wc.sta.password) - 1;
    }
    memcpy(wc.sta.password, cfg->password, pass_len);
    /* 阈值 OPEN = 不限制加密方式（开放/WPA2/WPA3 都允许）；具体由 AP 决定 */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        goto cleanup;
    }
    wifi_started = true;

    if (!wait_bits(EVT_WIFI_STARTED, 5000, cancel, NULL)) {
        if (cancel != NULL && *cancel) {
            result = WIFI_TIME_SYNC_CANCELLED;
        } else {
            ESP_LOGE(TAG, "wifi start timeout");
        }
        goto cleanup;
    }
    /* NTP 只要几十字节，关掉省电模式换响应速度 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 逐次尝试连接；认证类失败立即终止（重试无意义） */
    uint8_t last_reason = 0;
    for (int attempt = 1; attempt <= WIFI_TIME_SYNC_WIFI_ATTEMPTS; attempt++) {
        if (cancel != NULL && *cancel) {
            result = WIFI_TIME_SYNC_CANCELLED;
            goto cleanup;
        }
        report(cb, ctx, WIFI_TIME_SYNC_STAGE_WIFI_CONNECT, attempt,
               WIFI_TIME_SYNC_WIFI_ATTEMPTS,
               10 + (attempt - 1) * 12, cfg->ssid);

        xEventGroupClearBits(s_evt,
                             EVT_WIFI_GOT_IP | EVT_WIFI_DISCONNECTED);
        s_disconnect_reason = 0;
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect (attempt %d): %s", attempt,
                     esp_err_to_name(err));
        }

        EventBits_t bits = 0;
        bool got = wait_bits(EVT_WIFI_GOT_IP | EVT_WIFI_DISCONNECTED,
                             WIFI_TIME_SYNC_CONNECT_TIMEOUT_MS, cancel, &bits);
        if (cancel != NULL && *cancel) {
            result = WIFI_TIME_SYNC_CANCELLED;
            goto cleanup;
        }
        if (got && (bits & EVT_WIFI_GOT_IP)) {
            ESP_LOGI(TAG, "wifi connected on attempt %d", attempt);
            report(cb, ctx, WIFI_TIME_SYNC_STAGE_WIFI_IP, attempt,
                   WIFI_TIME_SYNC_WIFI_ATTEMPTS, 50, cfg->ssid);
            /* WiFi 仍在线：直接进入 NTP，成功后才走 cleanup 释放 */
            result = ntp_sync(cfg->ntp_server, cb, ctx, cancel, utc_out);
            goto cleanup;
        }
        last_reason = s_disconnect_reason;
        ESP_LOGW(TAG, "wifi attempt %d/%d failed (reason=%u, timeout=%d)",
                 attempt, WIFI_TIME_SYNC_WIFI_ATTEMPTS,
                 (unsigned)last_reason, got ? 0 : 1);

        /* 密码错误 / 认证失败：换一次也不会成功，直接报错 */
        if (got && reason_is_auth(last_reason)) {
            result = WIFI_TIME_SYNC_ERR_WIFI_AUTH;
            goto cleanup;
        }
        if (reason_is_not_found(last_reason)) {
            result = WIFI_TIME_SYNC_ERR_WIFI_NOT_FOUND;
        } else {
            result = WIFI_TIME_SYNC_ERR_WIFI_TIMEOUT;
        }

        if (attempt < WIFI_TIME_SYNC_WIFI_ATTEMPTS) {
            if (!delay_cancelable(1000, cancel)) {
                result = WIFI_TIME_SYNC_CANCELLED;
                goto cleanup;
            }
        }
    }
    ESP_LOGE(TAG, "wifi connect failed after %d attempts (last reason=%u)",
             WIFI_TIME_SYNC_WIFI_ATTEMPTS, (unsigned)last_reason);

cleanup:
    if (handlers_registered) {
        if (wifi_handler != NULL) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  wifi_handler);
        }
        if (ip_handler != NULL) {
            esp_event_handler_instance_unregister(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  ip_handler);
        }
    }
    if (wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    if (wifi_inited) {
        esp_wifi_deinit();
    }
    if (netif != NULL) {
        esp_netif_destroy_default_wifi(netif);
    }
    if (s_evt != NULL) {
        vEventGroupDelete(s_evt);
        s_evt = NULL;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* NTP 对时                                                            */
/* ------------------------------------------------------------------ */

/* SNTP 同步完成通知（lwip tcpip 线程调用）：只置标志。
   ⚠ 不能只用"time() 是否大于 2023"判定成功：上一次对时成功后系统时间
   已经正确，第二次对时即使服务器无应答也会立刻被误判为成功。 */
static volatile bool s_ntp_synced = false;

static void ntp_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_ntp_synced = true;
}

static wifi_time_sync_result_t ntp_sync(const char *server,
                                        wifi_time_sync_progress_cb_t cb,
                                        void *ctx, volatile bool *cancel,
                                        time_t *utc_out)
{
    /* 先做一次 DNS 解析：能把"域名写错/没有 DNS"和"NTP 无应答"区分开 */
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int dns = getaddrinfo(server, "123", &hints, &res);
    if (res != NULL) {
        freeaddrinfo(res);
    }
    if (dns != 0) {
        ESP_LOGE(TAG, "resolve %s failed: %d", server, dns);
        return WIFI_TIME_SYNC_ERR_NTP_DNS;
    }

    for (int attempt = 1; attempt <= WIFI_TIME_SYNC_NTP_ATTEMPTS; attempt++) {
        if (cancel != NULL && *cancel) {
            return WIFI_TIME_SYNC_CANCELLED;
        }
        report(cb, ctx, WIFI_TIME_SYNC_STAGE_NTP, attempt,
               WIFI_TIME_SYNC_NTP_ATTEMPTS,
               55 + (attempt - 1) * 8, server);

        esp_sntp_stop();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, server);
        esp_sntp_set_time_sync_notification_cb(ntp_sync_notification_cb);
        s_ntp_synced = false;
        esp_sntp_init();

        /* 成功判据 = 本次收到过 SNTP 完成通知（而不是"系统时间看起来合理"）；
           settimeofday 由 SNTP 内部完成 */
        const time_t sane_epoch = 1700000000;  /* 2023-11-14 */
        int64_t deadline = esp_timer_get_time()
                         + (int64_t)WIFI_TIME_SYNC_NTP_TIMEOUT_MS * 1000;
        while (esp_timer_get_time() < deadline) {
            if (cancel != NULL && *cancel) {
                esp_sntp_stop();
                return WIFI_TIME_SYNC_CANCELLED;
            }
            if (s_ntp_synced) {
                time_t now = time(NULL);
                if (now < sane_epoch) {
                    /* 收到通知但时间明显不合理：按失败处理，避免写入 1970 */
                    ESP_LOGE(TAG, "ntp notified but time %lld is insane",
                             (long long)now);
                    break;
                }
                esp_sntp_stop();
                *utc_out = now;
                ESP_LOGI(TAG, "ntp synced on attempt %d: %lld (UTC)", attempt,
                         (long long)now);
                return WIFI_TIME_SYNC_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(WAIT_SLICE_MS));
        }
        ESP_LOGW(TAG, "ntp attempt %d/%d timeout", attempt,
                 WIFI_TIME_SYNC_NTP_ATTEMPTS);
        esp_sntp_stop();
        if (attempt < WIFI_TIME_SYNC_NTP_ATTEMPTS) {
            if (!delay_cancelable(1000, cancel)) {
                return WIFI_TIME_SYNC_CANCELLED;
            }
        }
    }
    return WIFI_TIME_SYNC_ERR_NTP_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                             */
/* ------------------------------------------------------------------ */

wifi_time_sync_result_t wifi_time_sync_run(wifi_time_sync_progress_cb_t cb,
                                           void *ctx,
                                           volatile bool *cancel)
{
    wifi_time_sync_result_t result = WIFI_TIME_SYNC_OK;

    ESP_LOGI(TAG, "=== wifi time sync start ===");
    int64_t t0 = esp_timer_get_time();

    report(cb, ctx, WIFI_TIME_SYNC_STAGE_READ_CONFIG, 0, 0, 3, NULL);
    wifi_cfg_t cfg;
    result = read_wifi_config(&cfg);
    if (result != WIFI_TIME_SYNC_OK) {
        return result;
    }

    if (!rtc_is_ready()) {
        ESP_LOGE(TAG, "RTC not ready");
        return WIFI_TIME_SYNC_ERR_RTC;
    }

    time_t utc = 0;
    result = wifi_sync_online(&cfg, cb, ctx, cancel, &utc);
    if (result != WIFI_TIME_SYNC_OK) {
        ESP_LOGW(TAG, "wifi/ntp stage failed: %d", (int)result);
        return result;
    }

    if (cancel != NULL && *cancel) {
        return WIFI_TIME_SYNC_CANCELLED;
    }

    /* UTC → 本地墙钟（RTC 不存时区，存用户看到的本地时间） */
    report(cb, ctx, WIFI_TIME_SYNC_STAGE_WRITE_RTC, 0, 0, 95, NULL);
    int tz_min = config_store_get_tz_offset_min();
    time_t local = utc + (time_t)tz_min * 60;
    struct tm tm_local;
    if (gmtime_r(&local, &tm_local) == NULL) {
        return WIFI_TIME_SYNC_ERR_RTC;
    }

    rtc_time_t rt = {
        .year    = (uint16_t)(tm_local.tm_year + 1900),
        .month   = (uint8_t)(tm_local.tm_mon + 1),
        .day     = (uint8_t)tm_local.tm_mday,
        .hour    = (uint8_t)tm_local.tm_hour,
        .minute  = (uint8_t)tm_local.tm_min,
        .second  = (uint8_t)tm_local.tm_sec,
        .weekday = rtc_weekday_from_date((uint16_t)(tm_local.tm_year + 1900),
                                         (uint8_t)(tm_local.tm_mon + 1),
                                         (uint8_t)tm_local.tm_mday),
    };
    esp_err_t err = rtc_set_time(&rt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtc_set_time: %s", esp_err_to_name(err));
        return WIFI_TIME_SYNC_ERR_RTC;
    }

    report(cb, ctx, WIFI_TIME_SYNC_STAGE_DONE, 0, 0, 100, NULL);
    ESP_LOGI(TAG, "=== wifi time sync done in %d ms: %04u-%02u-%02u "
                  "%02u:%02u:%02u (UTC%+d:%02d) ===",
             (int)((esp_timer_get_time() - t0) / 1000),
             rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second,
             tz_min / 60, (tz_min < 0 ? -tz_min : tz_min) % 60);
    return WIFI_TIME_SYNC_OK;
}
