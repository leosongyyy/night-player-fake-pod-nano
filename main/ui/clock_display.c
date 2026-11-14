#include "ui/clock_display.h"

#include "ui/clock_text.h"

#include <stddef.h>
#include <string.h>

#include "lvgl.h"

/* 最多同时挂多少个 label：状态栏 + 下拉通知栏，留两个余量 */
#define CLOCK_DISPLAY_MAX_LABELS  4

/* 临时状态默认持续秒数（用户要求"大概 3 秒"） */
#define CLOCK_DISPLAY_TEMP_SEC    3

/* 临时状态文本缓冲（"🔊 30" 之类很短，32 字节足够） */
#define CLOCK_DISPLAY_TEMP_MAX    32

/* 时间刷新周期（ms）：只显示时/分，1 秒轮询足以在分钟跳变时及时更新 */
#define CLOCK_DISPLAY_TICK_MS     1000

static lv_obj_t *s_labels[CLOCK_DISPLAY_MAX_LABELS];  /* 已挂 label（NULL=空槽） */
static lv_timer_t *s_tick_timer = NULL;               /* 1 秒刷新定时器（常驻） */
static uint8_t s_temp_hold_sec = 0;                   /* >0 = 正在显示临时状态（剩余秒数） */
static char s_temp_text[CLOCK_DISPLAY_TEMP_MAX];      /* 当前临时状态文本 */

/* 把文本写给所有已挂 label（内容相同不重复设，避免多余重绘） */
static void labels_set_text(const char *text)
{
    for (size_t i = 0; i < CLOCK_DISPLAY_MAX_LABELS; i++) {
        lv_obj_t *label = s_labels[i];
        if (label == NULL) {
            continue;
        }
        const char *cur = lv_label_get_text(label);
        if (cur == NULL || strcmp(cur, text) != 0) {
            lv_label_set_text(label, text);
        }
    }
}

/* label 被删除时自动摘除登记（本项目里两者都常驻，这里只是防御悬垂指针） */
static void label_deleted_cb(lv_event_t *e)
{
    lv_obj_t *label = (lv_obj_t *)lv_event_get_target(e);
    for (size_t i = 0; i < CLOCK_DISPLAY_MAX_LABELS; i++) {
        if (s_labels[i] == label) {
            s_labels[i] = NULL;
        }
    }
}

/* 按当前时制把时间写给所有 label（RTC 不可用时是 --:--） */
static void render_time(void)
{
    char buf[8];
    ui_clock_text(buf, sizeof(buf));
    labels_set_text(buf);
}

/* 1 秒定时器：先递减临时状态的秒计数器，归零那一拍写回时间。
   临时状态未结束时直接返回——此时 label 归临时文本所有，不能覆盖。 */
static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_temp_hold_sec > 0) {
        s_temp_hold_sec--;
        if (s_temp_hold_sec > 0) {
            return;
        }
        s_temp_text[0] = '\0';
    }
    render_time();
}

/* 惰性创建 1 秒定时器（第一个 label 挂上时） */
static void ensure_timer(void)
{
    if (s_tick_timer != NULL) {
        return;
    }
    s_tick_timer = lv_timer_create(tick_cb, CLOCK_DISPLAY_TICK_MS, NULL);
}

void clock_display_attach(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }
    for (size_t i = 0; i < CLOCK_DISPLAY_MAX_LABELS; i++) {
        if (s_labels[i] == label) {
            return;   /* 已挂过 */
        }
    }
    for (size_t i = 0; i < CLOCK_DISPLAY_MAX_LABELS; i++) {
        if (s_labels[i] == NULL) {
            s_labels[i] = label;
            lv_obj_add_event_cb(label, label_deleted_cb, LV_EVENT_DELETE, NULL);
            break;
        }
    }
    ensure_timer();
    /* 立即填充一次，避免 label 停在创建时的占位文本 */
    if (s_temp_hold_sec > 0) {
        labels_set_text(s_temp_text);
    } else {
        render_time();
    }
}

void clock_display_show_temp_for(const char *text, uint8_t seconds)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    size_t n = strlen(text);
    if (n >= sizeof(s_temp_text)) {
        n = sizeof(s_temp_text) - 1;
    }
    memcpy(s_temp_text, text, n);
    s_temp_text[n] = '\0';

    s_temp_hold_sec = (seconds == 0) ? 1 : seconds;
    ensure_timer();
    labels_set_text(s_temp_text);
}

void clock_display_show_temp(const char *text)
{
    clock_display_show_temp_for(text, CLOCK_DISPLAY_TEMP_SEC);
}

void clock_display_refresh(void)
{
    if (s_temp_hold_sec > 0) {
        return;   /* 临时状态期间不改写，等它自然结束 */
    }
    render_time();
}
