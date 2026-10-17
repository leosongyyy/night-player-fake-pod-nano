#include "core/ui/global_style.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "gstyle";

static bool s_inited;

/* 全局动画模板：必须 static（样式属性里只存指针，生命周期贯穿始终）。
   见头文件"LVGL 9 实测结论"：v9 的 lv_label 在每次重建跑马灯动画时会
   调用 overwrite_anim_property()，把模板里的 act_time（起始延迟）、
   repeat_cnt（轮数）、repeat_delay（轮间停顿）、completed_cb 抄进新动画，
   所以本模块只需要"维护这一个模板 + 在合适的时机重挂/重启"。 */
static lv_anim_t s_anim_template;

/* ------------------------------------------------------------------ */
/* 跑马灯 label 登记表（滚动期统一暂停用）                               */
/*                                                                     */
/* 为什么需要：列表滚动时若可见行还在跑跑马灯，跑马灯动画每帧都会让       */
/* 整块视口重新失效 —— 滚动本来就要重画视口，再叠加跑马灯动画等于把       */
/* "一帧只画一次"变成"一帧画多次"（实验分支实测该项让滚动帧从 20 ms      */
/* 变成 38 ms 量级）。用户已确认"跑马灯帧率低一点不影响手感，滚动才是      */
/* 用户主动触发的，优先保它"，故滚动期间统一暂停。                        */
/*                                                                     */
/* **何时恢复：直接用 LVGL 的 `LV_EVENT_SCROLL_END`，不另设定时器**。      */
/* LVGL 9 在三条路径上都成对发 BEGIN/END（手势拖动 lv_indev_scroll.c、    */
/* 程序化滚动与滚动动画结束 lv_obj_scroll.c），比"位移停 250 ms 才认为     */
/* 停了"的猜测更准确、也更简单。恢复后由动画模板自带的                */
/* `GLOBAL_STYLE_MARQUEE_START_DELAY_MS`（1 s 起始延迟）负责"别一停手就  */
/* 立刻滚动"的观感，所以这里**不需要**任何延迟。                         */
/*                                                                     */
/* 登记发生在 global_style_marquee_attach()；`marquee_off` 记住"暂停前它  */
/* 是否真的在滚"，恢复时只重启原本在滚的那些。                            */
/*                                                                     */
/* ⚠⚠ 两个真机事故留下的结论（务必保留）：                                */
/*  ① **绝对不要用 lv_obj_is_valid() 做存活校验**。它是"从屏幕开始递归遍历   */
/*     整棵对象树"，对每个登记项都是一次 O(对象数) 的全树扫描；更要命的是   */
/*     它**不可靠**：label 被删除后内存可能被 LVGL 复用成别的类，指针仍"在  */
/*     树里"，于是校验通过 → 我们对一个非 label 对象调 lv_label_set_long_   */
/*     mode() → 行为未定义 → LVGL 任务卡死 → **task_wdt 超时**（真机实测：  */
/*     滚动停止后 250 ms 必定触发，backtrace 落在                            */
/*     gs_marquee_apply_all → obj_valid_child）。                           */
/*     正确做法：登记时给 label 挂 LV_EVENT_DELETE 回调，销毁时自动 O(1) 摘除。*/
/*  ② "被上层抑制"（非当前页 / 画布接管绘制）不能被"滚动结束恢复"重新打开 —— */
/*     那是一个真实存在过的 bug：非当前页的 label 被切成 CLIP 后，只要滚动   */
/*     停过一次就会被恢复路径当成"被滚动暂停过"而重开成 CIRCULAR，表现为    */
/*     "离开列表页后其它页的跑马灯自己跑起来"。现在用独立的 `no_marquee`     */
/*     标志区分"被抑制"与"被滚动暂停"。                                    */
/* ------------------------------------------------------------------ */
#define GS_MARQUEE_REG_MAX 48

typedef struct {
    lv_obj_t *label;
    bool      marquee_off;   /* true = 因滚动暂停而临时切到了 CLIP */
    bool      no_marquee;    /* true = 上层主动抑制（非当前页/画布接管），永不自动恢复 */
} gs_marquee_reg_t;

static gs_marquee_reg_t s_reg[GS_MARQUEE_REG_MAX];
static uint8_t  s_reg_cnt;
static bool     s_scroll_active;            /* 滚动进行中（BEGIN..END 之间）：跑马灯统一暂停 */

/* 从登记表里按指针摘除一项（用尾巴填补，保持紧凑） */
static void gs_reg_remove(lv_obj_t *label)
{
    for (uint8_t i = 0; i < s_reg_cnt; i++) {
        if (s_reg[i].label == label) {
            s_reg[i] = s_reg[s_reg_cnt - 1];
            s_reg_cnt--;
            return;
        }
    }
}

/* label 被销毁：O(1) 摘除登记项（见文件上方说明 ①） */
static void gs_label_delete_cb(lv_event_t *e)
{
    gs_reg_remove((lv_obj_t *)lv_event_get_target(e));
}

/* 该 label 是否已在登记表中；返回登记项（未登记返回 NULL） */
static gs_marquee_reg_t *gs_reg_find(lv_obj_t *label, bool create_if_missing)
{
    for (uint8_t i = 0; i < s_reg_cnt; i++) {
        if (s_reg[i].label == label) {
            return &s_reg[i];
        }
    }
    if (!create_if_missing) {
        return NULL;
    }
    if (s_reg_cnt >= GS_MARQUEE_REG_MAX) {
        ESP_LOGW(TAG, "marquee registry full (%d)", (int)GS_MARQUEE_REG_MAX);
        return NULL;
    }
    s_reg[s_reg_cnt].label = label;
    s_reg[s_reg_cnt].marquee_off = false;
    s_reg[s_reg_cnt].no_marquee = false;
    /* 生命周期由 LVGL 通知我们，绝不用 lv_obj_is_valid 轮询（见上方 ①） */
    lv_obj_add_event_cb(label, gs_label_delete_cb, LV_EVENT_DELETE, NULL);
    return &s_reg[s_reg_cnt++];
}

/* 把某个 label 切换到 CLIP（暂停）或 SCROLL_CIRCULAR（恢复并重跑） */
static void gs_marquee_apply(lv_obj_t *label, bool want_scroll)
{
    const gs_marquee_reg_t *r = gs_reg_find(label, false);
    if (r != NULL && r->no_marquee) {
        return;   /* 被主动抑制：任何自动路径都不许碰它 */
    }
    lv_label_long_mode_t mode = want_scroll
        ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
    if (lv_label_get_long_mode(label) == mode) {
        return;
    }
    /* 只调 lv_label_set_long_mode：它内部会 `lv_anim_delete(set_ofs_x_anim /
       set_ofs_y_anim)` + 复位 offset + refr_text，等于把滚动动画彻底停掉。
       ⚠ 不要试图自己 lv_anim_delete(label, set_ofs_x_anim)：那是 lv_label.c 的
       static 函数，外部拿不到符号 */
    lv_label_set_long_mode(label, mode);
    if (want_scroll) {
        global_style_marquee_attach(label);   /* 按全局节奏从头重跑 */
    }
}

/* 按"是否滚动中"重新应用全表（只处理"本该在滚"的那些） */
static void gs_marquee_apply_all(void)
{
    for (uint8_t i = 0; i < s_reg_cnt; i++) {
        gs_marquee_reg_t *r = &s_reg[i];
        if (r->no_marquee) {
            continue;   /* 被主动抑制（非当前页 / 画布接管绘制） */
        }
        if (s_scroll_active) {
            /* 暂停：只处理当前真在滚的，并记住状态 */
            if (lv_label_get_long_mode(r->label) == LV_LABEL_LONG_SCROLL_CIRCULAR) {
                r->marquee_off = true;
                gs_marquee_apply(r->label, false);
            }
        } else if (r->marquee_off) {
            /* 恢复：只重启原本在滚的 */
            r->marquee_off = false;
            gs_marquee_apply(r->label, true);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

/* 按当前参数从头重跑一次跑马灯。
   v9 的 lv_label_set_long_mode() 会先删掉旧的横向滚动动画、把 offset 复位到
   0，再调 lv_label_refr_text() 按样式模板重建动画——正好等于"按全局节奏
   从头重跑"，因此这里重复写一次同样的模式即可（v8 时代它同样会重建，
   这点两个版本一致）。 */
static void gs_marquee_restart(lv_obj_t *label)
{
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_SCROLL_CIRCULAR) {
        return;   /* 该 label 当前不要跑马灯（如非当前页被切成 CLIP）：不动它 */
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

/* 首次调用时初始化全局动画模板（懒初始化：调用方都在 LVGL 上下文，
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
#if GLOBAL_STYLE_MARQUEE_ROUNDS > 0
    /* 总轮数（含第一轮）：v9 会把它抄进 label 重建出来的滚动动画 */
    lv_anim_set_repeat_count(&s_anim_template, GLOBAL_STYLE_MARQUEE_ROUNDS);
#else
    /* 0 = 无限循环：显式用 LVGL 的"无限"常量（v9 为 0xFFFFFFFF，v8 是 0xFFFF，
       直接写字面量会踩坑，故用宏） */
    lv_anim_set_repeat_count(&s_anim_template, LV_ANIM_REPEAT_INFINITE);
#endif

    ESP_LOGI(TAG, "marquee style: rounds=%d, pause=%dms, start delay=%dms, duration=%dms",
             GLOBAL_STYLE_MARQUEE_ROUNDS, GLOBAL_STYLE_MARQUEE_PAUSE_MS,
             GLOBAL_STYLE_MARQUEE_START_DELAY_MS, GLOBAL_STYLE_MARQUEE_DURATION_MS);
}

void global_style_marquee_set_scroll_active(bool active)
{
    if (active == s_scroll_active) {
        return;   /* 状态没变：幂等返回（滚动事件每帧都会来） */
    }
    s_scroll_active = active;
    gs_marquee_apply_all();
}

void global_style_marquee_set_suppressed(lv_obj_t *label, bool suppressed)
{
    if (label == NULL) {
        return;
    }
    gs_marquee_reg_t *r = gs_reg_find(label, true);
    if (r == NULL) {
        return;
    }
    r->no_marquee = suppressed;
    r->marquee_off = false;   /* 抑制与"滚动暂停"是两件事，别让它被恢复路径捡走 */
}

void global_style_marquee_attach(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }

    gs_init_once();

    /* 被主动抑制的 label（非当前页 / 画布接管绘制）：
       连样式都不用碰，直接返回。见文件上方说明 ② */
    const gs_marquee_reg_t *r = gs_reg_find(label, true);
    if (r != NULL && r->no_marquee) {
        return;
    }

    /* 全局动画模板：起始延迟 + 轮数 + 轮间停顿，全部交给 LVGL 原生处理
       （LVGL 每次重建滚动动画都会读它，所以改了文本/尺寸也能正确重来） */
    lv_obj_set_style_anim(label, &s_anim_template, 0);

#if GLOBAL_STYLE_MARQUEE_DURATION_MS > 0
    /* 单轮滚动时长（ms）：style 属性，必须在动画创建之前设好。
       ⚠ v8 这里是 anim_speed（px/s），v9 已删除该属性，只保留时长 */
    lv_obj_set_style_anim_duration(label, GLOBAL_STYLE_MARQUEE_DURATION_MS, 0);
#endif

    /* 立刻按新参数重跑（不等别的地方触发刷新） */
    gs_marquee_restart(label);
}

void global_style_marquee_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    /* 内容没变：一个字都不动。
       lv_label_set_text 即便内容相同也会走一遍重建动画（见头文件说明），
       曲目/目录变化时上层会重绑全部可见行，不拦这一下就会白白让
       所有可见行的跑马灯重新开始计时 */
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;
    }

    /* 内容真的变了：改写并按全局节奏从头重跑（停顿 START_DELAY → 滚 ROUNDS 轮） */
    lv_label_set_text(label, text);
    global_style_marquee_attach(label);
}
