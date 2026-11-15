#include "core/hardware/bt/bt_service.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/bt_audio.h"
#include "core/hardware/audio/eq.h"
#include "core/storage/bt_store.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "bt_service";

/* 临时调试开关：强制开机启动蓝牙并扫描（排查 BT 栈/内存问题用）。
 * 正常必须为 0：发布行为是"按 NVS 记忆的开关状态决定是否启动；
 * 主机不主动回连，但保持 page scan，已配对耳机可自己开机回连"。 */
#define BT_DEBUG_AUTOSTART_TEST 0

/* 广播名（耳机上显示的名字，用户指定） */
#define BT_LOCAL_DEVICE_NAME  "NightPlayer FPNPro"

/* bt_svc 任务：内部 RAM 栈（会调用协议栈/NVS/音频，不能放 PSRAM）、Core1 */
#define BT_SVC_TASK_STACK   (6 * 1024)
#define BT_SVC_TASK_PRIO    5
#define BT_SVC_TASK_CORE    1

/* 扫描结果上限 */
#define BT_SCAN_MAX  16

/* AVRCP 事务标签 */
#define BT_AVRC_TL_GET_CAPS   0
#define BT_AVRC_TL_ABS_VOL    1
#define BT_AVRC_TL_VOL_NOTIFY 2

/* ------------------------------------------------------------------ */
/* 状态（UI 轮询，无需回调）                                            */
/* ------------------------------------------------------------------ */

static volatile bool          s_enabled = false;   /* 用户开关意图 */
static volatile bt_svc_power_t s_power = BT_SVC_POWER_OFF;
static bool                   s_stack_up = false;

static volatile bool     s_connected = false;
static volatile bool     s_streaming = false;
static volatile int      s_conn_index = -1;
static volatile uint32_t s_out_fs = 0;
static volatile bool     s_codec_unsupported = false;

static esp_bd_addr_t s_peer_bda = {0};
static char          s_peer_name[BT_STORE_NAME_MAX] = "";

static esp_a2d_conn_hdl_t s_conn_hdl = 0;

/* 流请求（audio_pcm_start/stop 驱动） */
static volatile bool s_want_stream = false;
static volatile bool s_stream_pending = false;

/* 设备切换：已连着 A 时用户点了 B → 先断开 A，等 link down 后再连 B。
 * 连接失败不回退 A（用户明确要求）。超时 5s 自动取消，避免 A 长时间
 * 不掉线时残留一个"以后突然连 B"的定时炸弹。 */
static volatile bool s_pending_valid = false;
static uint8_t       s_pending_bda[6] = {0};
static esp_timer_handle_t s_switch_timer = NULL;

/* AVRCP 状态 */
static volatile bool s_avrc_connected = false;
static esp_avrc_rn_evt_cap_mask_t s_rn_cap;

/* 扫描结果 */
typedef struct {
    uint8_t bda[6];
    char    name[BT_STORE_NAME_MAX];
    int     rssi;
} bt_scan_ent_t;

/* 扫描结果：16×~80B=1.3KB，放 PSRAM（内部 RAM 要留给蓝牙控制器），
   init 时分配；指针为空时所有扫描接口视为空表。 */
static bt_scan_ent_t *s_scan = NULL;
static int            s_scan_count = 0;
static portMUX_TYPE   s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool  s_scanning = false;

/* ------------------------------------------------------------------ */
/* 命令队列 / 任务                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_START = 0,
    CMD_STOP,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SCAN_START,
    CMD_SCAN_STOP,
    CMD_PAIR,
    CMD_FORGET,
    CMD_STREAM_START,
    CMD_STREAM_STOP,
    CMD_LINK_UP,
    CMD_LINK_DOWN,
    CMD_SET_OUT_FS,
} bt_cmd_type_t;

typedef struct {
    bt_cmd_type_t type;
    int           idx;
    uint8_t       bda[6];
    uint8_t       vol;
    char          name[BT_STORE_NAME_MAX];
} bt_cmd_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task = NULL;
static esp_timer_handle_t s_stream_timer = NULL;

/* GAP 扫描模式（定义在 GAP 回调一节，连接生命周期里会用到） */
static void bt_scan_mode_idle(void);
static void bt_scan_mode_connected(void);
static void bt_connect_bda(const uint8_t bda[6]);
static esp_err_t bt_stack_start(void);
static void bt_stack_stop(void);

static void bt_post(bt_cmd_type_t type, int idx, const uint8_t *bda,
                    const char *name, uint8_t vol)
{
    if (s_queue == NULL) {
        return;
    }
    bt_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.idx = idx;
    if (bda != NULL) {
        memcpy(cmd.bda, bda, 6);
    }
    if (name != NULL) {
        snprintf(cmd.name, sizeof(cmd.name), "%s", name);
    }
    cmd.vol = vol;
    /* 协议栈回调任务里不能阻塞：队列满就直接丢弃并记日志（状态可由定时器补发） */
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop type %d", (int)type);
    }
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

/* 音量档位 ↔ AVRCP 绝对音量（0~127）
 *
 * ⚠ 蓝牙走**满范围映射**（2026-11 真机反馈后改）：1 档 = 1/127、30 档 = 127/127。
 * 之前复用 DAC 的 35%~85% 曲线，1 档就是 44/127，对高灵敏度耳机（如 ROSE
 * EarFeel i7）仍然偏大。DAC 路径不受影响，仍在 audio.c 里用 35%~85% 保护听力。
 * 反向映射用于耳机侧调音量时同步回本机档位。 */
static uint8_t level_to_absvol(uint8_t level)
{
    if (level == 0) {
        return 0;
    }
    if (level > AUDIO_VOL_LEVEL_MAX) {
        level = AUDIO_VOL_LEVEL_MAX;
    }
    /* 1 -> 1, 30 -> 127（线性铺满） */
    uint32_t v = 1u + (uint32_t)(level - 1) * 126u
                      / (AUDIO_VOL_LEVEL_MAX - 1u);
    return (uint8_t)v;
}

static uint8_t absvol_to_level(uint8_t vol127)
{
    if (vol127 == 0) {
        return 0;
    }
    if (vol127 >= 127) {
        return AUDIO_VOL_LEVEL_MAX;
    }
    uint32_t lvl = 1u + (uint32_t)(vol127 - 1)
                        * (AUDIO_VOL_LEVEL_MAX - 1u) / 126u;
    if (lvl > AUDIO_VOL_LEVEL_MAX) {
        lvl = AUDIO_VOL_LEVEL_MAX;
    }
    return (uint8_t)lvl;
}

/* 在能力位图里按偏好顺序选一个（返回位值，0 = 都不支持） */
static uint8_t pick_bit(uint8_t mask, const uint8_t *prefs, int n)
{
    for (int i = 0; i < n; i++) {
        if (mask & prefs[i]) {
            return prefs[i];
        }
    }
    return 0;
}

/* 扫描缓存里按地址找名字；找不到返回空串 */
static void scan_lookup_name(const uint8_t bda[6], char *out, size_t out_size)
{
    out[0] = '\0';
    if (s_scan == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_scan_mux);
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan[i].bda, bda, 6) == 0) {
            snprintf(out, out_size, "%s", s_scan[i].name);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_scan_mux);
}

/* ------------------------------------------------------------------ */
/* 音频联动                                                             */
/* ------------------------------------------------------------------ */

/* 绝对音量发送（audio_set_volume_level → 这里 → AVRCP CT） */
static void bt_absvol_send(uint8_t level)
{
    if (!s_avrc_connected || !s_stack_up) {
        return;
    }
    uint8_t v = level_to_absvol(level);
    esp_avrc_ct_send_set_absolute_volume_cmd(BT_AVRC_TL_ABS_VOL, v);
}

/* 播放开始/暂停 → 请求 A2DP 起流/挂起（bt_audio 注册，任意任务调用） */
static void bt_audio_stream_request_cb(bool start)
{
    s_want_stream = start;
    if (!s_stack_up || !s_connected) {
        return;
    }
    bt_post(start ? CMD_STREAM_START : CMD_STREAM_STOP, -1, NULL, NULL, 0);
}

/* 1s 周期：连接后若还想播放但流没起来，重试（媒体通道 ACK 可能忙） */
static void bt_stream_timer_cb(void *arg)
{
    (void)arg;
    if (s_stack_up && s_connected && s_want_stream && !s_streaming &&
        !s_stream_pending) {
        bt_post(CMD_STREAM_START, -1, NULL, NULL, 0);
    }
}

/* 设备切换超时：5s 内旧设备一直没断开 → 取消这次切换，避免很久以后
 * 关掉 A 时突然去连 B。 */
static void bt_switch_timer_cb(void *arg)
{
    (void)arg;
    if (s_pending_valid) {
        s_pending_valid = false;
        ESP_LOGW(TAG, "switch: pending connect timed out, cancelled");
    }
}

/* 连接成功后的重活（bt_svc 任务上下文）：应用设备音量/EQ、切换输出、起流 */
static void bt_link_up(const uint8_t bda[6])
{
    /* CMD_LINK_UP 可能在队列里排队期间链路又断了/换设备了：过期事件不能
     * 再把输出切到蓝牙（否则会覆盖新链路或留下没有链路的 BT 输出） */
    if (!s_connected || memcmp(bda, s_peer_bda, 6) != 0) {
        ESP_LOGW(TAG, "stale link up ignored");
        return;
    }
    /* 已经连上（可能是目标耳机自己回连，也可能是旧耳机又连回来）：
     * 取消尚未完成的切换，避免稍后再把这条链路拆掉 */
    if (s_pending_valid) {
        s_pending_valid = false;
        if (s_switch_timer != NULL) {
            esp_timer_stop(s_switch_timer);
        }
        ESP_LOGI(TAG, "switch: cancelled, a link is already up");
    }
    int idx = bt_store_find(bda);
    if (idx < 0) {
        char name[BT_STORE_NAME_MAX];
        scan_lookup_name(bda, name, sizeof(name));
        idx = bt_store_add(bda, name[0] ? name : "蓝牙耳机",
                           audio_get_volume_level());
    }
    if (idx < 0) {
        /* 设备表满：不应发生（配对入口已检查），兜底断开避免状态错乱 */
        ESP_LOGW(TAG, "device table full on link up, disconnect");
        esp_a2d_source_disconnect((uint8_t *)bda);
        return;
    }
    s_conn_index = idx;

    bt_device_t dev;
    memset(&dev, 0, sizeof(dev));
    bt_store_get(idx, &dev);
    if (dev.name[0] != '\0') {
        snprintf(s_peer_name, sizeof(s_peer_name), "%s", dev.name);
    }

    if (!s_out_fs) {
        s_out_fs = 48000;   /* 协商结果出来前先按 48k（caps 事件会纠正） */
    }

    /* 设备配置优先：音量/EQ 用该设备记录（连接期间不写 config_store） */
    audio_bt_profile_enter(dev.volume, dev.eq_valid ? &dev.eq : NULL);
    audio_bt_output_enable(s_out_fs);

    ESP_LOGI(TAG, "linked: slot %d \"%s\", volume %u, EQ %s, fs %u",
             idx, s_peer_name, (unsigned)dev.volume, dev.eq_valid ? "on" : "off",
             (unsigned)s_out_fs);

    /* 单链路：已连着耳机时关掉 page scan。否则其它已配对耳机（尤其喜欢
     * 自动回连的）会不断 page 我们，Bluedroid 在"已有 AV SCB"时会报
     * av scb not available / BTA_AV_OPEN_EVT FAILED / Out of ccbs，
     * 既刷屏又可能制造过期的 AVRCP 状态。断开后恢复可连接。 */
    bt_scan_mode_connected();
}

/* 断开后的收尾：切回 DAC、恢复配置槽位音量/EQ */
static void bt_link_down(void)
{
    audio_bt_output_disable();
    audio_bt_profile_leave();
    bt_audio_notify_streaming(false);
    s_conn_index = -1;
    s_peer_name[0] = '\0';
    s_out_fs = 0;
    s_streaming = false;
    s_stream_pending = false;
    bt_store_flush_pending(true);
    ESP_LOGI(TAG, "link down, output restored to DAC");
    /* 断开后重新允许已配对耳机主动回连 */
    bt_scan_mode_idle();

    /* 用户点了另一台设备：旧链路已收尾，稍等协议栈释放 AV SCB 后连目标。
     * ⚠ 不重启协议栈：实测重启后 Bluedroid 的 SDP/AVDTP 状态会残留，
     * 反而导致 host 主动连接全部失效（耳机自己回连仍可用）。 */
    if (s_pending_valid) {
        uint8_t bda[6];
        memcpy(bda, s_pending_bda, 6);
        s_pending_valid = false;
        if (s_switch_timer != NULL) {
            esp_timer_stop(s_switch_timer);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "switch: link down, connecting pending device");
        bt_connect_bda(bda);
    }
}

/* ------------------------------------------------------------------ */
/* GAP 回调                                                             */
/* ------------------------------------------------------------------ */

/* 空闲态扫描模式：可连接、不可发现。
 *
 * 这是"耳机主动回连"的关键：BR/EDR 的 page scan 关闭时（NON_CONNECTABLE），
 * 已配对耳机开机后根本 page 不到我们；开着 page scan 且不开 inquiry scan，
 * 耳机能按自己记的地址直连，同时又不会出现在附近设备的搜索列表里。
 * 发现（配对）期间才临时改成 CONNECTABLE + GENERAL_DISCOVERABLE。 */
static void bt_scan_mode_idle(void)
{
    if (s_stack_up && s_power == BT_SVC_POWER_ON) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
}

/* 已连接态：不 page scan、不可发现。单链路设计下避免其它耳机反复
 * page（Bluedroid 已有 AV SCB 时只会报错），也省一点待机功耗。 */
static void bt_scan_mode_connected(void)
{
    if (s_stack_up && s_power == BT_SVC_POWER_ON && !s_scanning) {
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
}

static void scan_add(const uint8_t *bda, const char *name, int rssi)
{
    if (name == NULL || name[0] == '\0' || s_scan == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_scan_mux);
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan[i].bda, bda, 6) == 0) {
            /* 已存在：更新名字/RSSI */
            snprintf(s_scan[i].name, sizeof(s_scan[i].name), "%s", name);
            s_scan[i].rssi = rssi;
            taskEXIT_CRITICAL(&s_scan_mux);
            return;
        }
    }
    if (s_scan_count < BT_SCAN_MAX) {
        memcpy(s_scan[s_scan_count].bda, bda, 6);
        snprintf(s_scan[s_scan_count].name, sizeof(s_scan[s_scan_count].name),
                 "%s", name);
        s_scan[s_scan_count].rssi = rssi;
        s_scan_count++;
    }
    taskEXIT_CRITICAL(&s_scan_mux);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        const uint8_t *bda = param->disc_res.bda;
        uint8_t *eir = NULL;
        int rssi = -128;
        char name[BT_STORE_NAME_MAX] = "";
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (uint8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                if (p->len > 0) {
                    size_t n = p->len < sizeof(name) - 1 ? p->len : sizeof(name) - 1;
                    memcpy(name, p->val, n);
                    name[n] = '\0';
                }
                break;
            default:
                break;
            }
        }
        if (eir != NULL) {
            uint8_t len = 0;
            uint8_t *nm = esp_bt_gap_resolve_eir_data(
                eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (nm == NULL) {
                nm = esp_bt_gap_resolve_eir_data(
                    eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            }
            if (nm != NULL && len > 0) {
                size_t n = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
                memcpy(name, nm, n);
                name[n] = '\0';
            }
        }
        scan_add(bda, name, rssi);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_scanning = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            /* 扫描结束回到"可连接但不可发现"：已配对的耳机开机后能自己
             * 主动回连（page scan），但不会出现在别人的搜索结果里。
             * 若此时已连着耳机，则保持不可连接（单链路）。 */
            if (s_connected) {
                bt_scan_mode_connected();
            } else {
                bt_scan_mode_idle();
            }
            ESP_LOGI(TAG, "discovery finished (%d device(s))", bt_service_scan_count());
        }
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        /* 仅记录日志；切换改为重启协议栈，不再依赖 ACL 事件时序 */
        ESP_LOGD(TAG, "ACL disconnected (reason %d)",
                 (int)param->acl_disconn_cmpl_stat.reason);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "auth complete: stat=%d name=%s", (int)param->auth_cmpl.stat,
                 (char *)param->auth_cmpl.device_name);
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            char nm[BT_STORE_NAME_MAX];
            scan_lookup_name(param->auth_cmpl.bda, nm, sizeof(nm));
            int idx = bt_store_find(param->auth_cmpl.bda);
            if (idx >= 0 && nm[0] != '\0') {
                bt_store_update_name(idx, nm);
            }
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin;
        memset(pin, 0, sizeof(pin));
        uint8_t pin_len = 4;
        if (param->pin_req.min_16_digit) {
            pin_len = 16;
        } else {
            pin[0] = '0';
            pin[1] = '0';
            pin[2] = '0';
            pin[3] = '0';
        }
        esp_bt_gap_pin_reply(param->pin_req.bda, true, pin_len, pin);
        ESP_LOGI(TAG, "legacy PIN requested, replied %u digits", (unsigned)pin_len);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm requested (%06u), accept", (unsigned)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06u", (unsigned)param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey requested");
        break;
    case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
        ESP_LOGI(TAG, "local device name: %s", (char *)param->get_dev_name_cmpl.name);
        break;
    default:
        ESP_LOGD(TAG, "gap event %d", (int)event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* A2DP 回调                                                            */
/* ------------------------------------------------------------------ */

/* 协商 SBC 参数：优先 48kHz，其次 44.1kHz；都不支持则标记不支持 */
static void a2d_choose_pref(esp_a2d_conn_hdl_t hdl, const esp_a2d_mcc_t *caps)
{
    if (caps == NULL || caps->type != ESP_A2D_MCT_SBC) {
        ESP_LOGE(TAG, "sink codec is not SBC (type=%d)", caps ? (int)caps->type : -1);
        s_codec_unsupported = true;
        return;
    }
    const esp_a2d_cie_sbc_t *s = &caps->cie.sbc_info;

    static const uint8_t sf_pref[] = {
        ESP_A2D_SBC_CIE_SF_48K, ESP_A2D_SBC_CIE_SF_44K,
    };
    uint8_t sf = pick_bit(s->samp_freq, sf_pref, 2);
    if (sf == 0) {
        ESP_LOGE(TAG, "sink SBC does not support 48k/44.1k (mask=0x%x)",
                 (unsigned)s->samp_freq);
        s_codec_unsupported = true;
        return;
    }
    s_codec_unsupported = false;

    static const uint8_t ch_pref[] = {
        ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO,
        ESP_A2D_SBC_CIE_CH_MODE_STEREO,
        ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL,
        ESP_A2D_SBC_CIE_CH_MODE_MONO,
    };
    static const uint8_t bl_pref[] = {
        ESP_A2D_SBC_CIE_BLOCK_LEN_16, ESP_A2D_SBC_CIE_BLOCK_LEN_12,
        ESP_A2D_SBC_CIE_BLOCK_LEN_8, ESP_A2D_SBC_CIE_BLOCK_LEN_4,
    };
    static const uint8_t sb_pref[] = {
        ESP_A2D_SBC_CIE_NUM_SUBBANDS_8, ESP_A2D_SBC_CIE_NUM_SUBBANDS_4,
    };
    static const uint8_t al_pref[] = {
        ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS, ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR,
    };

    esp_a2d_mcc_t pref;
    memset(&pref, 0, sizeof(pref));
    pref.type = ESP_A2D_MCT_SBC;
    pref.cie.sbc_info.samp_freq    = sf;
    pref.cie.sbc_info.ch_mode      = pick_bit(s->ch_mode, ch_pref, 4);
    pref.cie.sbc_info.block_len    = pick_bit(s->block_len, bl_pref, 4);
    pref.cie.sbc_info.num_subbands = pick_bit(s->num_subbands, sb_pref, 2);
    pref.cie.sbc_info.alloc_mthd   = pick_bit(s->alloc_mthd, al_pref, 2);
    if (pref.cie.sbc_info.ch_mode == 0 || pref.cie.sbc_info.block_len == 0 ||
        pref.cie.sbc_info.num_subbands == 0 || pref.cie.sbc_info.alloc_mthd == 0) {
        ESP_LOGE(TAG, "sink SBC caps incomplete, use defaults");
        pref.cie.sbc_info.ch_mode      = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
        pref.cie.sbc_info.block_len    = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
        pref.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
        pref.cie.sbc_info.alloc_mthd   = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    }
    /* bitpool：在耳机能力范围内取尽量高（53 是 A2DP 高质量档，约 328kbps@48k） */
    uint8_t min_bp = s->min_bitpool < 2 ? 2 : s->min_bitpool;
    uint8_t max_bp = s->max_bitpool > 53 ? 53 : s->max_bitpool;
    if (max_bp < min_bp) {
        max_bp = min_bp;
    }
    pref.cie.sbc_info.min_bitpool = min_bp;
    pref.cie.sbc_info.max_bitpool = max_bp;

    esp_err_t err = esp_a2d_source_set_pref_mcc(hdl, &pref);
    uint32_t fs = (sf == ESP_A2D_SBC_CIE_SF_48K) ? 48000 : 44100;
    ESP_LOGI(TAG, "pref SBC: %u Hz ch=0x%x bl=0x%x sb=0x%x alloc=0x%x bp=%u..%u (%s)",
             (unsigned)fs, (unsigned)pref.cie.sbc_info.ch_mode,
             (unsigned)pref.cie.sbc_info.block_len,
             (unsigned)pref.cie.sbc_info.num_subbands,
             (unsigned)pref.cie.sbc_info.alloc_mthd, (unsigned)min_bp,
             (unsigned)max_bp, esp_err_to_name(err));
    (void)fs;
    /* ⚠ 这里**只表达偏好**，不写 s_out_fs：实际生效的采样率以协议栈的
       ESP_A2D_AUDIO_CFG_EVT 为准（真机实测过：首连时栈可能先用 44.1k 默认值，
       此时若按 48k 喂 PCM，播放速度会快约 8.8%）。 */
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            /* 单链路设计：已连着 A 时又收到 B 的 CONNECTED（理论上栈会拒绝，
             * 这里兜底）——保持当前 A，断开 B，避免 s_peer_bda/设备槽位被覆盖 */
            if (s_connected && memcmp(param->conn_stat.remote_bda, s_peer_bda, 6) != 0) {
                ESP_LOGW(TAG, "A2DP connect for another device while \"%s\" active, reject",
                         s_peer_name);
                esp_a2d_source_disconnect((uint8_t *)param->conn_stat.remote_bda);
                break;
            }
            s_connected = true;
            s_conn_hdl = param->conn_stat.conn_hdl;
            memcpy(s_peer_bda, param->conn_stat.remote_bda, 6);
            ESP_LOGI(TAG, "A2DP connected %02X:%02X:%02X:%02X:%02X:%02X, conn_hdl=%u, mtu=%u",
                     param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                     param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                     param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5],
                     (unsigned)s_conn_hdl, (unsigned)param->conn_stat.audio_mtu);
            bt_post(CMD_LINK_UP, -1, s_peer_bda, NULL, 0);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            /* ⚠ 第二副耳机在 A 已连接时尝试回连，Bluedroid 会先报
             * BTA_AV_OPEN_EVT::FAILED，再给一个"从未连接成功"的
             * DISCONNECTED 事件；如果无条件走 link_down，会把正在放的 A
             * 一起拆掉。这里按 bda/当前状态过滤：
             *   - 从未连上（s_connected=false）→ 没有可收尾的链路，忽略；
             *   - 断开的不是当前对端 → 过期/别人的事件，忽略。 */
            if (!s_connected) {
                ESP_LOGI(TAG, "A2DP disconnect without active link (%02X:%02X:%02X:%02X:%02X:%02X, reason %d), ignore",
                         param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                         param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                         param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5],
                         (int)param->conn_stat.disc_rsn);
                break;
            }
            if (memcmp(param->conn_stat.remote_bda, s_peer_bda, 6) != 0) {
                ESP_LOGW(TAG, "ignore A2DP disconnect for a non-active device");
                break;
            }
            ESP_LOGI(TAG, "A2DP disconnected (reason %d)",
                     (int)param->conn_stat.disc_rsn);
            s_connected = false;
            s_streaming = false;
            s_want_stream = false;
            s_stream_pending = false;
            s_conn_hdl = 0;
            bt_audio_notify_streaming(false);
            bt_post(CMD_LINK_DOWN, -1, NULL, NULL, 0);
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_streaming = true;
            s_stream_pending = false;
            bt_audio_notify_streaming(true);
        } else {
            s_streaming = false;
            bt_audio_notify_streaming(false);
        }
        break;
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT:
        a2d_choose_pref(param->a2d_report_snk_codec_caps_stat.conn_hdl,
                        &param->a2d_report_snk_codec_caps_stat.mcc);
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t cmd = param->media_ctrl_stat.cmd;
        bool ok = (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS);
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (ok) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else {
                s_stream_pending = false;
                ESP_LOGW(TAG, "check src ready failed");
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            s_stream_pending = false;
            ESP_LOGI(TAG, "media start ack: %s", ok ? "ok" : "fail");
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            ESP_LOGI(TAG, "media suspend ack: %s", ok ? "ok" : "fail");
        }
        break;
    }
    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(TAG, "A2DP profile state: %d",
                 (int)param->a2d_prof_stat.init_state);
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        /* 协议栈最终采用的 codec 配置（A2DP source 也会上报）：采样率以这里为准 */
        const esp_a2d_mcc_t *m = &param->audio_cfg.mcc;
        if (m->type == ESP_A2D_MCT_SBC) {
            const esp_a2d_cie_sbc_t *s = &m->cie.sbc_info;
            uint32_t fs = 0;
            if (s->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                fs = 48000;
            } else if (s->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                fs = 44100;
            } else if (s->samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                fs = 32000;
            }
            ESP_LOGI(TAG, "A2DP audio cfg: SBC sf=0x%x ch=0x%x bl=0x%x sb=0x%x "
                          "alloc=0x%x bp=%u..%u -> %u Hz",
                     (unsigned)s->samp_freq, (unsigned)s->ch_mode,
                     (unsigned)s->block_len, (unsigned)s->num_subbands,
                     (unsigned)s->alloc_mthd, (unsigned)s->min_bitpool,
                     (unsigned)s->max_bitpool, (unsigned)fs);
            if (fs != 0 && fs != s_out_fs) {
                s_out_fs = fs;
                if (s_connected) {
                    bt_post(CMD_SET_OUT_FS, -1, NULL, NULL, 0);
                }
            }
        } else {
            ESP_LOGW(TAG, "A2DP audio cfg: non-SBC codec type %d",
                     (int)m->type);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "a2d event %d", (int)event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* AVRCP CT 回调                                                        */
/* ------------------------------------------------------------------ */

static void avrc_register_volume_notify(void)
{
    if (s_avrc_connected) {
        esp_avrc_ct_send_register_notification_cmd(BT_AVRC_TL_VOL_NOTIFY,
                                                   ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        /* 与 A2DP 一样按 bda 过滤：多副耳机轮流回连时，失败/过期的 AVRCP
         * 连接状态不能影响当前对端（否则会把当前设备的绝对音量支持清掉） */
        if (param->conn_stat.connected) {
            if (s_connected && memcmp(param->conn_stat.remote_bda, s_peer_bda, 6) != 0) {
                ESP_LOGW(TAG, "ignore AVRCP connect for a non-active device");
                break;
            }
            s_avrc_connected = true;
            ESP_LOGI(TAG, "AVRCP connected, query RN capabilities");
            esp_avrc_ct_send_get_rn_capabilities_cmd(BT_AVRC_TL_GET_CAPS);
        } else {
            if (s_connected && memcmp(param->conn_stat.remote_bda, s_peer_bda, 6) != 0) {
                ESP_LOGW(TAG, "ignore stale AVRCP disconnect");
                break;
            }
            s_avrc_connected = false;
            ESP_LOGI(TAG, "AVRCP disconnected");
            bt_audio_set_absvol_supported(false);
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        s_rn_cap.bits = param->get_rn_caps_rsp.evt_set.bits;
        bool vol_cap = esp_avrc_rn_evt_bit_mask_operation(
            ESP_AVRC_BIT_MASK_OP_TEST, &s_rn_cap, ESP_AVRC_RN_VOLUME_CHANGE);
        ESP_LOGI(TAG, "peer RN caps=0x%x, volume-change=%d",
                 (unsigned)s_rn_cap.bits, (int)vol_cap);
        bt_audio_set_absvol_supported(vol_cap);
        if (vol_cap) {
            avrc_register_volume_notify();
            audio_bt_refresh_volume();
        } else {
            /* 能力表里没有音量变化通知：仍乐观地发一次 SetAbsoluteVolume，
               收到响应就认为支持绝对音量（不少耳机不报这个能力位） */
            esp_avrc_ct_send_set_absolute_volume_cmd(
                BT_AVRC_TL_ABS_VOL, level_to_absvol(audio_get_volume_level()));
        }
        break;
    }
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        ESP_LOGI(TAG, "SetAbsoluteVolume response: %u/127",
                 (unsigned)param->set_volume_rsp.volume);
        if (!bt_audio_absvol_supported()) {
            ESP_LOGI(TAG, "peer supports absolute volume (detected by response)");
            bt_audio_set_absvol_supported(true);
            avrc_register_volume_notify();
            audio_bt_refresh_volume();
        }
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            uint8_t vol = param->change_ntf.event_parameter.volume;
            uint8_t level = absvol_to_level(vol);
            ESP_LOGI(TAG, "remote volume change: %u/127 -> level %u",
                     (unsigned)vol, (unsigned)level);
            /* 耳机侧改了音量：同步本机档位并写回该设备记忆（不回发命令） */
            audio_volume_from_remote(level);
            if (s_conn_index >= 0) {
                bt_store_set_volume(s_conn_index, level);
            }
            avrc_register_volume_notify();
        }
        break;
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "AVRCP remote features mask=0x%x tg_flag=0x%x",
                 (unsigned)param->rmt_feats.feat_mask,
                 (unsigned)param->rmt_feats.tg_feat_flag);
        break;
    case ESP_AVRC_CT_PROF_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP CT state: %d", (int)param->avrc_ct_init_stat.state);
        break;
    default:
        ESP_LOGD(TAG, "avrc event %d", (int)event);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 协议栈启动/关闭                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t bt_stack_start(void)
{
    if (s_stack_up) {
        s_power = BT_SVC_POWER_ON;
        return ESP_OK;
    }

    s_power = BT_SVC_POWER_STARTING;

    /* 只用 Classic：释放 BLE 控制器内存（已编译 BLE 时才有意义） */
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        goto fail;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        goto fail;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        goto fail;
    }

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name(BT_LOCAL_DEVICE_NAME);

    /* 无屏幕键盘的从设备角色：Just Works（SSP），legacy PIN 回 0000 */
    esp_bt_sp_param_t sp_param = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(sp_param, &iocap, sizeof(iocap));
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    memset(pin_code, 0, sizeof(pin_code));
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    /* AVRCP CT 先于 A2DP 初始化（A2DP 连接时两者一起建立） */
    err = esp_avrc_ct_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "avrc ct init failed: %s", esp_err_to_name(err));
    }
    esp_avrc_ct_register_callback(avrc_ct_cb);

    err = esp_a2d_register_callback(a2d_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2d register callback failed: %s", esp_err_to_name(err));
    }
    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2d source init failed: %s", esp_err_to_name(err));
    }
    err = esp_a2d_source_register_data_callback(bt_audio_data_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2d data callback register failed: %s", esp_err_to_name(err));
    }

    /* 空闲态保持可连接（page scan 开、不可发现），让已配对耳机开机后能
     * 主动回连；需要配对时再临时加 inquiry scan。 */
    s_stack_up = true;
    s_power = BT_SVC_POWER_ON;
    bt_scan_mode_idle();

    ESP_LOGI(TAG, "stack up (device name \"%s\")", BT_LOCAL_DEVICE_NAME);
    /* 给 A2DP/AVRCP profile 一点初始化时间（异步事件） */
    vTaskDelay(pdMS_TO_TICKS(300));
#if BT_DEBUG_AUTOSTART_TEST
    ESP_LOGW(TAG, "DEBUG: auto scan after stack up");
    bt_post(CMD_SCAN_START, -1, NULL, NULL, 0);
#endif
    return ESP_OK;

fail:
    s_power = BT_SVC_POWER_OFF;
    return err;
}

static void bt_stack_stop(void)
{
    if (!s_stack_up) {
        s_power = BT_SVC_POWER_OFF;
        return;
    }
    s_power = BT_SVC_POWER_STOPPING;

    s_want_stream = false;
    if (s_connected) {
        esp_a2d_source_disconnect(s_peer_bda);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* ⚠ 关 host 之前必须**先反初始化两个 profile**：
       IDF 的 esp_bluedroid_deinit() 不会清 BTC 里 profile 的"已初始化"标志
       （真正的清理只走 profile 自己的 deinit），不先反初始化的话，下次开蓝牙
       时 esp_avrc_ct_init()/esp_a2d_source_init() 会返回 ESP_ERR_INVALID_STATE
       （日志 `btc_avrc_ct_init already initialized` / `a2d source init failed`），
       A2DP source 停在半初始化态 —— 之后耳机 ACL 能连上但起不了流，
       用户看到的就是"关一次蓝牙就再也连不上"。
       ① 顺序必须是 AVRC CT **先于** A2DP（IDF 的 btc_avrc_ct_deinit 会检查
          g_a2dp_on_deinit，反过来会告警并留下脏状态）；
       ② 两个 deinit 都是异步投递到 BTC 任务的，必须留出执行时间再关 host
          （btc_a2d_src_deinit 还会等 A2DP 状态回到 IDLE 才真正 cleanup）。 */
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_avrc_ct_deinit();
        esp_a2d_source_deinit();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_stack_up = false;
    s_connected = false;
    s_streaming = false;
    s_scanning = false;
    s_power = BT_SVC_POWER_OFF;
    bt_audio_set_absvol_supported(false);
    bt_audio_notify_streaming(false);
    ESP_LOGI(TAG, "stack down");
}

/* ------------------------------------------------------------------ */
/* 命令处理                                                             */
/* ------------------------------------------------------------------ */

static void bt_connect_bda(const uint8_t bda[6])
{
    if (!s_stack_up) {
        return;
    }
    if (s_connected) {
        if (memcmp(bda, s_peer_bda, 6) == 0) {
            return;   /* 已连接同一设备 */
        }
        ESP_LOGW(TAG, "already connected to another device, disconnect first");
        return;
    }
    s_codec_unsupported = false;
    esp_err_t err = esp_a2d_source_connect((uint8_t *)bda);
    ESP_LOGI(TAG, "connect request %02X:%02X:%02X:%02X:%02X:%02X: %s",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
             esp_err_to_name(err));
}

/* 用户点了某台设备：已连着别的设备时**先断开当前设备**，等 link down
 * 收尾后再连目标；目标连接失败就保持断开，不回退旧设备（用户要求）。
 * 只能在 bt_svc 任务上下文调用（bt_link_down 里会消费 pending）。 */
static void bt_request_connect_bda(const uint8_t bda[6])
{
    if (!s_stack_up) {
        return;
    }
    if (s_connected) {
        if (memcmp(bda, s_peer_bda, 6) == 0) {
            return;   /* 点的就是当前设备 */
        }
        memcpy(s_pending_bda, bda, 6);
        s_pending_valid = true;
        if (s_switch_timer != NULL) {
            esp_timer_stop(s_switch_timer);
            esp_timer_start_once(s_switch_timer, 5 * 1000 * 1000);
        }
        ESP_LOGI(TAG, "switch: disconnect current device first");
        esp_a2d_source_disconnect(s_peer_bda);
        return;
    }

    /* 没有活动连接：清掉可能残留的切换请求，直接连 */
    s_pending_valid = false;
    if (s_switch_timer != NULL) {
        esp_timer_stop(s_switch_timer);
    }
    bt_connect_bda(bda);
}

/* 取消尚未完成的设备切换（用户主动断开/关蓝牙/删除目标设备时） */
static void bt_pending_cancel(void)
{
    s_pending_valid = false;
    if (s_switch_timer != NULL) {
        esp_timer_stop(s_switch_timer);
    }
}

static void bt_handle_cmd(const bt_cmd_t *cmd)
{
    switch (cmd->type) {
    case CMD_START:
        if (bt_stack_start() == ESP_OK) {
            s_enabled = true;
        } else {
            s_enabled = false;
            bt_store_set_enabled(false);
        }
        break;

    case CMD_STOP:
        bt_pending_cancel();
        if (s_connected) {
            bt_link_down();
        }
        bt_stack_stop();
        break;

    case CMD_CONNECT:
        if (cmd->idx >= 0) {
            bt_device_t d;
            if (bt_store_get(cmd->idx, &d)) {
                bt_request_connect_bda(d.bda);
            }
        }
        break;

    case CMD_DISCONNECT:
        bt_pending_cancel();
        if (s_connected) {
            esp_a2d_source_disconnect(s_peer_bda);
        }
        break;

    case CMD_SCAN_START:
        if (!s_stack_up) {
            break;
        }
        {
            taskENTER_CRITICAL(&s_scan_mux);
            s_scan_count = 0;
            taskEXIT_CRITICAL(&s_scan_mux);
            s_scanning = true;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            esp_err_t err = esp_bt_gap_start_discovery(
                ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            ESP_LOGI(TAG, "start discovery: %s", esp_err_to_name(err));
        }
        break;

    case CMD_SCAN_STOP:
        if (s_stack_up) {
            esp_bt_gap_cancel_discovery();
        }
        break;

    case CMD_PAIR: {
        if (!s_stack_up) {
            break;
        }
        int idx = bt_store_find(cmd->bda);
        if (idx < 0) {
            if (bt_store_count() >= BT_STORE_MAX_DEVICES) {
                ESP_LOGW(TAG, "device table full (%d), refuse pairing",
                         BT_STORE_MAX_DEVICES);
                break;
            }
            idx = bt_store_add(cmd->bda, cmd->name[0] ? cmd->name : "蓝牙耳机",
                               audio_get_volume_level());
        } else if (cmd->name[0] != '\0') {
            bt_store_update_name(idx, cmd->name);
        }
        if (idx >= 0) {
            bt_request_connect_bda(cmd->bda);
        }
        break;
    }

    case CMD_FORGET: {
        bt_device_t d;
        if (!bt_store_get(cmd->idx, &d)) {
            break;
        }
        /* 正在切换到这个设备时又把它删了 → 取消切换 */
        if (s_pending_valid && memcmp(d.bda, s_pending_bda, 6) == 0) {
            bt_pending_cancel();
        }
        if (s_connected && memcmp(d.bda, s_peer_bda, 6) == 0) {
            esp_a2d_source_disconnect(s_peer_bda);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (s_stack_up) {
            esp_bt_gap_remove_bond_device(d.bda);
        }
        bt_store_remove(cmd->idx);
        break;
    }

    case CMD_STREAM_START:
        if (!s_connected || s_streaming || s_stream_pending) {
            break;
        }
        s_stream_pending = true;
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        break;

    case CMD_STREAM_STOP:
        s_stream_pending = false;
        if (s_streaming) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        }
        break;

    case CMD_LINK_UP:
        bt_link_up(cmd->bda);
        break;

    case CMD_LINK_DOWN:
        /* 断开事件排队期间已经连上了新设备：这是旧链路的收尾，忽略，
         * 不能把新链路的输出/设备状态清掉 */
        if (s_connected) {
            ESP_LOGW(TAG, "stale link down ignored (new link active)");
            break;
        }
        bt_link_down();
        break;

    case CMD_SET_OUT_FS:
        if (s_connected && s_out_fs != 0) {
            audio_bt_set_out_fs(s_out_fs);
        }
        break;

    default:
        break;
    }
}

static void bt_svc_task(void *arg)
{
    (void)arg;
    bt_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            bt_handle_cmd(&cmd);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

void bt_service_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;

    bt_store_init();

    /* 音频侧的联动回调 */
    bt_audio_set_stream_handler(bt_audio_stream_request_cb);
    bt_audio_set_absvol_handler(bt_absvol_send);

    s_queue = xQueueCreateWithCaps(8, sizeof(bt_cmd_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue == NULL) {
        s_queue = xQueueCreate(8, sizeof(bt_cmd_t));
    }
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "create cmd queue failed");
        return;
    }

    s_scan = heap_caps_calloc(BT_SCAN_MAX, sizeof(bt_scan_ent_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_scan == NULL) {
        s_scan = heap_caps_calloc(BT_SCAN_MAX, sizeof(bt_scan_ent_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_scan == NULL) {
        ESP_LOGW(TAG, "alloc scan table failed, scan results disabled");
    }
    BaseType_t ok = xTaskCreatePinnedToCore(bt_svc_task, "bt_svc",
                                            BT_SVC_TASK_STACK, NULL,
                                            BT_SVC_TASK_PRIO, &s_task,
                                            BT_SVC_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create bt_svc task failed");
        return;
    }

    esp_timer_create_args_t targs = {
        .callback = bt_stream_timer_cb,
        .arg = NULL,
        .name = "bt_stream",
    };
    esp_timer_create(&targs, &s_stream_timer);
    if (s_stream_timer != NULL) {
        esp_timer_start_periodic(s_stream_timer, 1000 * 1000);
    }

    /* 设备切换超时（一次性；需要时 start_once） */
    esp_timer_create_args_t targs2 = {
        .callback = bt_switch_timer_cb,
        .arg = NULL,
        .name = "bt_switch",
    };
    esp_timer_create(&targs2, &s_switch_timer);

    /* 按上次开关记忆决定是否启动（用户要求：记忆状态、不自动回连） */
#if BT_DEBUG_AUTOSTART_TEST
    s_enabled = true;
#else
    s_enabled = bt_store_get_enabled();
#endif
    if (s_enabled) {
        bt_post(CMD_START, -1, NULL, NULL, 0);
    } else {
        s_power = BT_SVC_POWER_OFF;
    }
    ESP_LOGI(TAG, "service ready (remembered bt_enabled=%d)", (int)s_enabled);
}

bool bt_service_is_enabled(void)
{
    return s_enabled;
}

void bt_service_set_enabled(bool on)
{
    if (s_enabled == on) {
        return;
    }
    s_enabled = on;
    bt_store_set_enabled(on);
    bt_post(on ? CMD_START : CMD_STOP, -1, NULL, NULL, 0);
    ESP_LOGI(TAG, "bt switch -> %s", on ? "on" : "off");
}

bt_svc_power_t bt_service_power_state(void)
{
    return s_power;
}

bool bt_service_is_connected(void)
{
    return s_connected;
}

bool bt_service_is_streaming(void)
{
    return s_streaming;
}

int bt_service_connected_index(void)
{
    return s_connected ? s_conn_index : -1;
}

const char *bt_service_connected_name(void)
{
    return s_connected ? s_peer_name : "";
}

uint32_t bt_service_output_fs(void)
{
    return s_connected ? s_out_fs : 0;
}

bool bt_service_codec_unsupported(void)
{
    return s_codec_unsupported;
}

void bt_service_connect_index(int idx)
{
    bt_post(CMD_CONNECT, idx, NULL, NULL, 0);
}

void bt_service_disconnect(void)
{
    bt_post(CMD_DISCONNECT, -1, NULL, NULL, 0);
}

void bt_service_forget(int idx)
{
    bt_post(CMD_FORGET, idx, NULL, NULL, 0);
}

void bt_service_scan_start(void)
{
    bt_post(CMD_SCAN_START, -1, NULL, NULL, 0);
}

void bt_service_scan_stop(void)
{
    bt_post(CMD_SCAN_STOP, -1, NULL, NULL, 0);
}

bool bt_service_scan_active(void)
{
    return s_scanning;
}

int bt_service_scan_count(void)
{
    int n;
    if (s_scan == NULL) {
        return 0;
    }
    taskENTER_CRITICAL(&s_scan_mux);
    n = s_scan_count;
    taskEXIT_CRITICAL(&s_scan_mux);
    return n;
}

bool bt_service_scan_get(int i, bt_scan_result_t *out)
{
    if (i < 0 || out == NULL || s_scan == NULL) {
        return false;
    }
    bool ok = false;
    taskENTER_CRITICAL(&s_scan_mux);
    if (i < s_scan_count) {
        memcpy(out->bda, s_scan[i].bda, 6);
        snprintf(out->name, sizeof(out->name), "%s", s_scan[i].name);
        out->rssi = s_scan[i].rssi;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_scan_mux);
    return ok;
}

void bt_service_scan_connect(int i)
{
    bt_scan_result_t r;
    if (!bt_service_scan_get(i, &r)) {
        return;
    }
    bt_post(CMD_PAIR, -1, r.bda, r.name, 0);
}

void bt_service_note_volume(uint8_t level)
{
    if (s_connected && s_conn_index >= 0) {
        bt_store_set_volume(s_conn_index, level);
    }
}

void bt_service_apply_device_eq(int idx)
{
    if (!s_connected || idx != s_conn_index) {
        return;
    }
    bt_device_t d;
    if (bt_store_get(idx, &d)) {
        eq_apply(d.eq_valid ? &d.eq : NULL);
    }
}

bool bt_service_connected_quiet_mode(void)
{
    if (!s_connected || s_conn_index < 0) {
        return false;
    }
    bt_device_t d;
    return bt_store_get(s_conn_index, &d) && d.quiet_mode;
}
