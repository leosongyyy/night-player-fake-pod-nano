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

void global_style_marquee_attach(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }

    gs_init_once();

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
