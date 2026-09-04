#include "ui/player_ticker.h"

#include "ui/toast.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "player_ticker";

/* 轮询周期（ms）：与播放页刷新周期一致，兼顾标题/进度刷新与开销 */
#define PLAYER_TICKER_MS   500

/* 坏曲事件"新鲜窗口"（us）：前三种单曲坏曲事件仅在发生后 15 秒内被拉取
   才弹 toast（锁屏期间 LVGL timer 冻结、解锁后补拉时用于过滤过时消息——
   锁屏时单首坏曲已被自动跳过且不影响播放，无需解锁后再打扰用户）；
   PLAYER_EVENT_TOO_MANY_BAD（连续坏曲过多已停止续播）无条件提示。 */
#define BAD_EVENT_FRESH_US (15 * 1000 * 1000LL)

/* 订阅者上限：当前正在播放页 + 播放列表页，预留目录列表等后续接入 */
#define PLAYER_TICKER_MAX  4

/* 一条订阅记录 */
typedef struct {
    player_ticker_cb_t cb;   /* 回调（NULL 表示空槽位） */
    void *ctx;               /* 回调上下文 */
} player_ticker_sub_t;

static lv_timer_t           *s_timer = NULL;   /* 共享轮询 timer */
static player_ticker_sub_t   s_subs[PLAYER_TICKER_MAX];
static uint32_t              s_sub_count = 0;  /* 已注册订阅数 */

/* 共享 timer 回调：拉一次状态快照，广播给所有订阅者 */
static void ticker_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 拉取坏曲事件（单槽，拉取即清空，后写覆盖前写）：把最近一次
       "文件不存在/数据损坏/格式不支持"或"连续坏曲过多"转成 toast。
       展示规则（见 BAD_EVENT_FRESH_US）：前三种仅 15 秒内发生才提示；
       "连续坏曲过多导致停止"无条件提示。在 LVGL timer 上下文执行
       （天然持 LVGL 锁），可直接操作 UI。 */
    player_event_t ev;
    if (player_controller_take_event(&ev)) {
        const int64_t age_us = esp_timer_get_time() - ev.time_us;
        const char *msg = NULL;
        switch (ev.type) {
        case PLAYER_EVENT_FILE_MISSING:
            if (age_us <= BAD_EVENT_FRESH_US) msg = "歌曲文件不存在";
            break;
        case PLAYER_EVENT_DATA_CORRUPT:
            if (age_us <= BAD_EVENT_FRESH_US) msg = "歌曲数据损坏";
            break;
        case PLAYER_EVENT_FORMAT_UNSUPPORTED:
            if (age_us <= BAD_EVENT_FRESH_US) msg = "歌曲格式不支持";
            break;
        case PLAYER_EVENT_TOO_MANY_BAD:
            msg = "请检查SD卡";   /* 无条件：自动续播已停止，必须告知用户 */
            break;
        default:
            break;
        }
        if (msg != NULL) {
            ui_toast_show(msg);
        } else {
            ESP_LOGI(TAG, "drop stale bad event (%d, age %lld us)",
                     (int)ev.type, (long long)age_us);
        }
    }

    player_controller_status_t st;
    player_controller_get_status(&st);

    for (uint32_t i = 0; i < s_sub_count; i++) {
        s_subs[i].cb(&st, s_subs[i].ctx);
    }
}

void player_ticker_subscribe(player_ticker_cb_t cb, void *ctx)
{
    if (cb == NULL) {
        return;
    }

    /* 同一 cb+ctx 重复订阅：忽略，避免重复回调 */
    for (uint32_t i = 0; i < s_sub_count; i++) {
        if (s_subs[i].cb == cb && s_subs[i].ctx == ctx) {
            return;
        }
    }

    if (s_sub_count >= PLAYER_TICKER_MAX) {
        ESP_LOGE(TAG, "subscribe overflow: %u/%u",
                 (unsigned)s_sub_count, (unsigned)PLAYER_TICKER_MAX);
        return;
    }

    s_subs[s_sub_count].cb = cb;
    s_subs[s_sub_count].ctx = ctx;
    s_sub_count++;

    /* 懒创建共享 timer：所有订阅者共用这同一个周期刷新 */
    if (s_timer == NULL) {
        s_timer = lv_timer_create(ticker_timer_cb, PLAYER_TICKER_MS, NULL);
    }
}

void player_ticker_unsubscribe(player_ticker_cb_t cb, void *ctx)
{
    for (uint32_t i = 0; i < s_sub_count; i++) {
        if (s_subs[i].cb == cb && s_subs[i].ctx == ctx) {
            /* 末尾元素补位，保持数组紧凑 */
            s_subs[i] = s_subs[s_sub_count - 1];
            s_sub_count--;
            return;
        }
    }
}
