#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file wifi_time_sync.h
 * @brief WiFi + SNTP 网络对时（读取 SD 卡 WiFi 配置 → 连 AP → NTP → 写 RTC）
 *
 * 配置来源（UTF-8 文本，位于 SD 卡）：
 *   `/sdcard/.ecov/wifi.txt`
 *     第 1 行：WiFi SSID
 *     第 2 行：WiFi 密码（开放网络可留空）
 *     第 3 行：可选，NTP 服务器域名/IP；缺省用 WIFI_TIME_SYNC_DEFAULT_NTP
 *   容忍 UTF-8 BOM、CRLF；SSID/服务器两端的空白会被裁掉，密码原样保留
 *   （只去掉行尾 \r），避免误伤以空格开头/结尾的密码。
 *
 * 流程与限制：
 *   - WiFi 最多尝试 3 次（含首次）；认证类失败（密码错误等）立即终止，
 *     不再浪费剩余次数；
 *   - NTP 最多尝试 5 次；每次重新初始化 SNTP 客户端；
 *   - 成功后把 **UTC + config_store 时区偏移** 写入 PCF85063A（RTC 存本地
 *     墙钟时间），并保持系统时间（UTC）与 SNTP 同步；
 *   - 任何阶段都可取消（cancel 标志），100ms 粒度响应。
 *
 * ⚠ 本函数是**阻塞**的（最长可达一分多钟），必须在独立任务里调用。
 * ⚠ 调用任务的栈必须在**内部 RAM**：esp_wifi_init/start 会触发 PHY 校准
 *   数据写 NVS（关缓存段），PSRAM 栈任务直接调用会命中 IDF 的关缓存断言。
 * ⚠ 调用方负责在流程期间抑制自动息屏（power_service_set_hold_awake）。
 * ⚠ 不要与其它 WiFi 使用者并发；本模块每次 run 都完整 init/start/stop/deinit，
 *   退出后不保留 WiFi 资源。
 */

/** wifi.txt 路径（SD 卡挂载点 + .ecov 隐藏目录，与 .npd 侧车文件同目录） */
#define WIFI_TIME_SYNC_CONFIG_PATH   "/sdcard/.ecov/wifi.txt"

/** wifi.txt 没有第三行时使用的默认 NTP 服务器（国内解析稳定） */
#define WIFI_TIME_SYNC_DEFAULT_NTP   "ntp.aliyun.com"

/** 最多尝试次数（含首次） */
#define WIFI_TIME_SYNC_WIFI_ATTEMPTS  3
#define WIFI_TIME_SYNC_NTP_ATTEMPTS   5

/** 单次超时（ms） */
#define WIFI_TIME_SYNC_CONNECT_TIMEOUT_MS  15000   /* 每次 WiFi 连接 */
#define WIFI_TIME_SYNC_NTP_TIMEOUT_MS      8000    /* 每次 NTP 等待 */

/** 进度阶段（UI 据此显示文字） */
typedef enum {
    WIFI_TIME_SYNC_STAGE_READ_CONFIG = 0,  /* 读取并解析 wifi.txt */
    WIFI_TIME_SYNC_STAGE_WIFI_CONNECT,     /* 连接 WiFi（第 attempt 次） */
    WIFI_TIME_SYNC_STAGE_WIFI_IP,          /* 已连上，等待 DHCP 分配 IP */
    WIFI_TIME_SYNC_STAGE_NTP,              /* 等待 NTP 应答（第 attempt 次） */
    WIFI_TIME_SYNC_STAGE_WRITE_RTC,        /* 写 RTC */
    WIFI_TIME_SYNC_STAGE_DONE,             /* 完成 */
} wifi_time_sync_stage_t;

/** 结果码（UI 据此显示错误信息） */
typedef enum {
    WIFI_TIME_SYNC_OK = 0,
    WIFI_TIME_SYNC_CANCELLED,          /* 用户取消 */
    WIFI_TIME_SYNC_ERR_NO_SD,          /* SD 卡未挂载 */
    WIFI_TIME_SYNC_ERR_NO_FILE,        /* 找不到 wifi.txt */
    WIFI_TIME_SYNC_ERR_BAD_FILE,       /* 文件为空/SSID 为空/行长超限 */
    WIFI_TIME_SYNC_ERR_WIFI_INIT,      /* WiFi 初始化/启动失败 */
    WIFI_TIME_SYNC_ERR_WIFI_NOT_FOUND, /* 扫不到该 AP（含安全模式不匹配） */
    WIFI_TIME_SYNC_ERR_WIFI_AUTH,      /* 密码错误/认证失败 */
    WIFI_TIME_SYNC_ERR_WIFI_TIMEOUT,   /* 连接超时 */
    WIFI_TIME_SYNC_ERR_NTP_DNS,        /* NTP 域名解析失败 */
    WIFI_TIME_SYNC_ERR_NTP_TIMEOUT,    /* NTP 多次无应答 */
    WIFI_TIME_SYNC_ERR_RTC,            /* RTC 写入失败/未就绪 */
} wifi_time_sync_result_t;

/**
 * @brief 进度回调（在调用 wifi_time_sync_run 的任务上下文执行，**不是** LVGL 任务）
 *
 * @param stage        当前阶段
 * @param attempt      当前第几次尝试（从 1 开始；非重试阶段为 0）
 * @param max_attempts 该阶段的最多尝试次数（非重试阶段为 0）
 * @param percent      进度百分比 0~100
 * @param detail       补充信息（SSID / NTP 服务器等，可为 NULL；仅本次回调内有效）
 *
 * ⚠ 回调里若要更新 LVGL 对象，必须自行 lvgl_port_lock()，且不能阻塞太久。
 */
typedef void (*wifi_time_sync_progress_cb_t)(wifi_time_sync_stage_t stage,
                                             int attempt, int max_attempts,
                                             int percent, const char *detail,
                                             void *ctx);

/**
 * @brief 执行一次完整的网络对时（阻塞）
 *
 * @param cb     进度回调（可为 NULL）
 * @param ctx    回调上下文
 * @param cancel 取消标志（可为 NULL；置 true 后 100ms 内响应并返回
 *               WIFI_TIME_SYNC_CANCELLED）
 * @return 结果码；WIFI_TIME_SYNC_OK 表示 RTC 已写入
 */
wifi_time_sync_result_t wifi_time_sync_run(wifi_time_sync_progress_cb_t cb,
                                           void *ctx,
                                           volatile bool *cancel);

/** @brief 阶段名（中文，UI 直接用） */
const char *wifi_time_sync_stage_text(wifi_time_sync_stage_t stage);

/** @brief 结果对应的用户可读错误信息（成功/取消也各有一句） */
const char *wifi_time_sync_result_text(wifi_time_sync_result_t result);

#ifdef __cplusplus
}
#endif
