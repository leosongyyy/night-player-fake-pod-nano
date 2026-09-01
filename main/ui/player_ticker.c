#include "ui/player_ticker.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "player_ticker";

/* 轮询周期（ms）：与播放页刷新周期一致，兼顾标题/进度刷新与开销 */
#define PLAYER_TICKER_MS   500

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
