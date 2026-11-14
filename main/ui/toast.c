#include "ui/toast.h"

#include "core/fonts/system_font.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "toast";

/* ------------------------------------------------------------------ */
/* 外观/行为参数                                                        */
/* ------------------------------------------------------------------ */

/* 底色：灰色（需求指定）；文字白色 */
#define TOAST_BG_COLOR       lv_color_hex(0x555555)
/* 倒计时环：背景弧浅灰（作为"剩余总量"底环），指示弧黄色 */
#define TOAST_RING_BG_COLOR  lv_color_hex(0x8A8A8A)
#define TOAST_RING_COLOR     lv_color_hex(0xFFC000) /* 黄，与设置页按压高亮同色 */

#define TOAST_DURATION_MS    2000    /* 默认显示时长：2 秒后自动收回 */
#define TOAST_TICK_MS        100     /* 倒计时环刷新周期（2s = 20 步，每步 5%） */
#define TOAST_SLIDE_PX       120     /* 滑入/滑出位移：底部向下的视觉距离 */
#define TOAST_SLIDE_IN_MS    240     /* 滑入动画时长 */
#define TOAST_SLIDE_OUT_MS   220     /* 滑出动画时长 */
#define TOAST_BOTTOM_MARGIN  40      /* 距屏幕底边距离（px） */

#define TOAST_RING_SIZE      20      /* 倒计时环直径（px），"小小的" */
#define TOAST_RING_WIDTH     3       /* 倒计时环线宽（px） */

#define TOAST_TEXT_MAX       128     /* 文本内部缓冲（UTF-8 字节） */

/* ------------------------------------------------------------------ */
/* 模块状态                                                            */
/* ------------------------------------------------------------------ */

static lv_obj_t    *s_toast   = NULL; /* Toast 容器（灰色圆角条） */
static lv_obj_t    *s_label   = NULL; /* 文本 */
static lv_obj_t    *s_arc     = NULL; /* 右侧环形倒计时 */
static lv_timer_t  *s_timer   = NULL; /* 倒计时驱动（100ms） */
static char         s_text[TOAST_TEXT_MAX]; /* 文本缓冲（lv_label 不复制） */
static int32_t      s_remain_ms = 0;  /* 剩余显示时长（ms）；<=0 = 不在显示期 */
static uint32_t     s_duration_ms = TOAST_DURATION_MS; /* 本次显示总时长（环形倒计时按它折算百分比） */
static bool         s_hidden    = true; /* 完全隐藏（含滑出动画完成态） */

/* ------------------------------------------------------------------ */
/* 事件透传                                                            */
/* ------------------------------------------------------------------ */

/* Toast 纯展示不接收触摸：各对象都 clear 掉 LV_OBJ_FLAG_CLICKABLE
   —— LVGL 9 里这就足够让命中测试直接放行（lv_obj_hit_test 没有该标志即返回
   false），v8 时代额外挂的 LV_EVENT_HIT_TEST 回调已删除：v9 的
   lv_hit_test_info_t 变成了不透明类型（私有头 lv_obj_event_private.h）。 */

/* ------------------------------------------------------------------ */
/* 动画                                                               */
/* ------------------------------------------------------------------ */

/* 滑入/滑出共用执行回调：以 translate_y 相对位移实现（不动 align） */
static void toast_slide_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

/* 滑出完成：隐藏对象并复位，等待下次显示 */
static void toast_hide_ready_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(s_toast, 0, 0);
    s_hidden = true;
}

/* 从底部滑入（仅首次显示时播放；持续显示中更新文字不重播） */
static void toast_slide_in(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_slide_anim_cb);
    lv_anim_set_values(&a, TOAST_SLIDE_PX, 0);
    lv_anim_set_time(&a, TOAST_SLIDE_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 滑出收回（倒计时到点） */
static void toast_slide_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_slide_anim_cb);
    lv_anim_set_ready_cb(&a, toast_hide_ready_cb);
    lv_anim_set_values(&a, 0, TOAST_SLIDE_PX);
    lv_anim_set_time(&a, TOAST_SLIDE_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ */
/* 倒计时                                                             */
/* ------------------------------------------------------------------ */

/* 倒计时到点：开始滑出（滑出期间不再更新环） */
static void toast_start_hide(void)
{
    toast_slide_out();
}

/* 100ms 周期：推进剩余时间 → 更新环形倒计时；到 0 触发滑出 */
static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_remain_ms <= 0) {
        return; /* 未显示 / 正在滑出 */
    }
    s_remain_ms -= TOAST_TICK_MS;
    if (s_remain_ms <= 0) {
        s_remain_ms = 0;
        lv_arc_set_value(s_arc, 0);
        toast_start_hide();
        return;
    }
    /* 环从满（100）递减到空（0）：直观展示"还有多久隐藏"。
       百分比按本次显示总时长折算（ui_toast_show_ms 可指定更长/更短时长） */
    lv_arc_set_value(s_arc, s_remain_ms * 100 / (int32_t)s_duration_ms);
}

/* ------------------------------------------------------------------ */
/* 创建                                                               */
/* ------------------------------------------------------------------ */

/* 懒创建：挂到 lv_layer_top()（系统顶层，位于一切普通对象之上）。
   必须在持 LVGL 锁时调用（本模块所有入口都满足）。 */
static void toast_create(void)
{
    lv_obj_t *top = lv_layer_top();

    /* 容器：灰色圆角条，flex 横向排布（文本 + 间距 + 环），居中 */
    s_toast = lv_obj_create(top);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, TOAST_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_all(s_toast, 0, 0);
    lv_obj_set_style_pad_left(s_toast, 20, 0);
    lv_obj_set_style_pad_right(s_toast, 14, 0);
    lv_obj_set_style_pad_top(s_toast, 10, 0);
    lv_obj_set_style_pad_bottom(s_toast, 10, 0);
    lv_obj_set_style_pad_column(s_toast, 12, 0); /* 文本与环之间间距 */
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* 不接收触摸：清除可点击/可滚动，命中测试透传 */
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(s_toast, LV_SCROLLBAR_MODE_OFF);

    /* 文本：白色（需求指定），系统字体（中文等宽适用） */
    s_label = lv_label_create(s_toast);
    lv_label_set_text(s_label, "");
    lv_obj_set_style_text_font(s_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);

    /* 环形倒计时：背景整圆浅灰 + 指示弧黄色，从顶部顺时针收缩；
       无旋钮、不可点击（纯展示）。弧从顶部（0°）顺时针画整圆，
       value = 剩余百分比（100 → 0） */
    s_arc = lv_arc_create(s_toast);
    lv_obj_set_size(s_arc, TOAST_RING_SIZE, TOAST_RING_SIZE);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB); /* 无旋钮 */
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_arc, TOAST_RING_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, TOAST_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, TOAST_RING_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, TOAST_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_arc_set_rotation(s_arc, 270);         /* 0° 转到顶部 */
    lv_arc_set_bg_angles(s_arc, 0, 360);     /* 背景整圆 */
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 100);

    /* 底部居中（留出底部边距） */
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -TOAST_BOTTOM_MARGIN);

    /* 倒计时驱动 timer（LVGL 任务上下文运行，已持锁） */
    s_timer = lv_timer_create(toast_timer_cb, TOAST_TICK_MS, NULL);

    ESP_LOGI(TAG, "toast created on lv_layer_top");
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

void ui_toast_show(const char *text)
{
    ui_toast_show_ms(text, TOAST_DURATION_MS);
}

void ui_toast_show_ms(const char *text, uint32_t duration_ms)
{
    if (text == NULL) {
        return;
    }
    if (s_toast == NULL) {
        toast_create();
    }

    /* 提到最上层：Toast 可能在蓝牙设置页/时间弹窗等全屏覆盖层之前创建，
       不置前的话会被这些后创建的层挡住（2026-11 发现）。 */
    lv_obj_move_foreground(s_toast);

    /* 截断进行中的滑入/滑出动画并复位（正在滑出时新消息到达 → 拉回重显） */
    lv_anim_del(s_toast, toast_slide_anim_cb);
    lv_obj_set_style_translate_y(s_toast, 0, 0);

    /* 复制文本（lv_label_set_text 不复制，调用方的临时缓冲可能失效） */
    snprintf(s_text, sizeof(s_text), "%s", text);
    lv_label_set_text(s_label, s_text);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -TOAST_BOTTOM_MARGIN);

    /* 重置隐藏倒计时与环形进度（时长 0 = 用默认 2 秒） */
    s_duration_ms = (duration_ms > 0) ? duration_ms : TOAST_DURATION_MS;
    s_remain_ms = (int32_t)s_duration_ms;
    lv_arc_set_value(s_arc, 100);

    if (s_hidden) {
        /* 完全隐藏中：从底部滑入 */
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        toast_slide_in();
    }
    s_hidden = false;

    ESP_LOGI(TAG, "toast (%u ms): %s", (unsigned)s_duration_ms, s_text);
}

bool ui_toast_is_shown(void)
{
    return (s_toast != NULL) && !s_hidden;
}
