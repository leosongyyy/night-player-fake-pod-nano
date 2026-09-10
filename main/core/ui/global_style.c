#include "core/ui/global_style.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "gstyle";

/* 监管表容量：目录页行槽 + 歌曲页行槽（各约 10 个）+ 正在播放页 2 个，留足余量 */
#define GS_MARQUEE_MAX_LABELS  48

/* 监管表：需要在守护定时器里复查"跑马灯是否被 LVGL 重建回无限轮"的 label */
static lv_obj_t *s_labels[GS_MARQUEE_MAX_LABELS];
static uint32_t  s_label_cnt;
static bool      s_inited;

/* 全局动画模板：必须 static（样式属性里只存指针，生命周期贯穿始终）。
   只有 act_time（起始延迟）与 repeat_delay（轮间停顿）会被 lv_label 读取，
   见头文件"LVGL 8.4 实测结论" */
static lv_anim_t s_anim_template;

/* "已接管"标记：写进 lv_anim_t::user_data 表示这条动画已被本模块改造过。
   lv_anim_init() 会 memset 整个动画结构，所以 LVGL 重建动画后标记自动失效，
   守护定时器据此判断是否需要补一次接管 */
static const uint8_t s_takeover_mark;

/* ------------------------------------------------------------------ */
/* 监管表：登记 / 摘除                                                  */
/* ------------------------------------------------------------------ */

/* label 被删除时自动摘除（LV_EVENT_DELETE 在对象析构前触发，此时指针仍有效） */
static void gs_label_delete_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_target(e);

    for (uint32_t i = 0; i < s_label_cnt; i++) {
        if (s_labels[i] != label) {
            continue;
        }
        /* 尾元素补位（表内顺序无意义，避免搬移） */
        s_labels[i] = s_labels[s_label_cnt - 1];
        s_label_cnt--;
        return;
    }
}

static void gs_register(lv_obj_t *label)
{
    for (uint32_t i = 0; i < s_label_cnt; i++) {
        if (s_labels[i] == label) {
            return;   /* 已在表内（不让同一个 label 装两次删除回调） */
        }
    }
    if (s_label_cnt >= GS_MARQUEE_MAX_LABELS) {
        ESP_LOGW(TAG, "marquee label table full (%d), label ignored",
                 GS_MARQUEE_MAX_LABELS);
        return;
    }
    s_labels[s_label_cnt++] = label;
    lv_obj_add_event_cb(label, gs_label_delete_cb, LV_EVENT_DELETE, NULL);
}

/* ------------------------------------------------------------------ */
/* 接管：把 LVGL 建好的无限轮滚动动画就地改成有限轮                      */
/* ------------------------------------------------------------------ */

/* 为什么不另建一条自己的动画，而要就地改 LVGL 的：
   同一 var 上只保留一条动画，避免两条动画抢写 label->offset.x；而且动画的
   exec_cb 仍是 LVGL 自己的，LVGL 的"切换长文本模式即停滚动（lv_anim_del 按
   exec_cb 删）"等既有逻辑不受影响 */
static void gs_marquee_takeover(lv_obj_t *label)
{
#if GLOBAL_STYLE_MARQUEE_ROUNDS > 0
    /* exec_cb 传 NULL：匹配该对象名下的任意动画。跑马灯 label 上只会有 lv_label
       自己建的那一条横向滚动动画（本模块不在这一类 label 上建别的动画） */
    lv_anim_t *a = lv_anim_get(label, NULL);
    if (a == NULL) {
        return;   /* 文本没溢出（或已经滚完）→ 没有动画可接管 */
    }
    if (a->user_data == (void *)&s_takeover_mark) {
        return;   /* 已接管 */
    }

    /* LVGL 语义：repeat_cnt = 总轮数（含第一轮），LV_ANIM_REPEAT_INFINITE(0xFFFF) 才是无限 */
    a->repeat_cnt   = (uint16_t)GLOBAL_STYLE_MARQUEE_ROUNDS;
    a->repeat_delay = GLOBAL_STYLE_MARQUEE_PAUSE_MS;   /* 模板已给，这里兜底 */
    a->user_data    = (void *)&s_takeover_mark;
#else
    /* 轮数为 0 = 无限循环：不改动，直接用 LVGL 原生行为 */
    LV_UNUSED(label);
#endif
}

/* 守护定时器：LVGL 在文本/尺寸/样式变化时会重建滚动动画（重建后轮数又变回
   无限），这里低频复查，发现"没被接管过"的动画就补一次接管。
   已接管的动画会在 gs_marquee_takeover 里立刻返回，开销可忽略 */
static void gs_marquee_poll_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    for (uint32_t i = 0; i < s_label_cnt; i++) {
        lv_obj_t *label = s_labels[i];
        /* 只有 App 明确要求跑马灯（CIRCULAR）的 label 才管；CLIP 等模式保持静止 */
        if (lv_label_get_long_mode(label) != LV_LABEL_LONG_SCROLL_CIRCULAR) {
            continue;
        }
        gs_marquee_takeover(label);
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

/* 按当前参数从头重跑一次跑马灯：重设同一长文本模式 = 删掉旧动画 + 偏移复位到 0
   + 让 lv_label 按全局模板重建动画（文本没变化也能重跑），随后接管轮数 */
static void gs_marquee_restart(lv_obj_t *label)
{
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_SCROLL_CIRCULAR) {
        return;   /* 该 label 当前不要跑马灯（如非当前页被切成 CLIP）：不动它 */
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    gs_marquee_takeover(label);
}

/* 首次调用时初始化模板与守护定时器（懒初始化：调用方都在 LVGL 上下文，
   不必让 main.c 关心初始化顺序） */
static void gs_init_once(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    lv_anim_init(&s_anim_template);
    lv_anim_set_delay(&s_anim_template, GLOBAL_STYLE_MARQUEE_START_DELAY_MS);
    lv_anim_set_repeat_delay(&s_anim_template, GLOBAL_STYLE_MARQUEE_PAUSE_MS);

    lv_timer_create(gs_marquee_poll_cb, GLOBAL_STYLE_MARQUEE_POLL_MS, NULL);

    ESP_LOGI(TAG, "marquee style: rounds=%d, pause=%dms, start delay=%dms, speed=%dpx/s",
             GLOBAL_STYLE_MARQUEE_ROUNDS, GLOBAL_STYLE_MARQUEE_PAUSE_MS,
             GLOBAL_STYLE_MARQUEE_START_DELAY_MS, GLOBAL_STYLE_MARQUEE_SPEED);
}

void global_style_marquee_attach(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }

    gs_init_once();
    gs_register(label);

    /* ① 全局动画模板：起始延迟 + 轮间停顿，交给 LVGL 原生处理
          （LVGL 每次重建滚动动画都会读它，所以改了文本/尺寸也能正确重来） */
    lv_obj_set_style_anim(label, &s_anim_template, 0);

#if GLOBAL_STYLE_MARQUEE_SPEED > 0
    /* 滚动速度：须在动画创建之前设好（lv_label 建动画时才读 anim_speed） */
    lv_obj_set_style_anim_speed(label, GLOBAL_STYLE_MARQUEE_SPEED, 0);
#endif

    /* ② 立刻按新参数重跑，并接管轮数（不等守护定时器） */
    gs_marquee_restart(label);
}

void global_style_marquee_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    /* 内容没变：一个字都不动。
       lv_label_set_text 即便内容相同也会重建滚动动画（见头文件说明），
       曲目/目录变化时上层会重绑全部可见行，不拦这一下就会"整屏跑马灯一起重置" */
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;
    }

    /* 内容真的变了：改写并按全局节奏从头重跑（停顿 START_DELAY → 滚 ROUNDS 轮） */
    lv_label_set_text(label, text);
    global_style_marquee_attach(label);
}
