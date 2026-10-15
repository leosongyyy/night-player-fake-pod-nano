#include "core/ui/touch_test.h"
#include "core/ui/lvgl_port.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "touch_test";

#define TOUCH_TEST_DOT_SIZE  20    /* 圆点直径（px） */
#define TOUCH_TEST_FADE_MS   1000  /* 抬起后渐变消失时长 */

static lv_obj_t *s_dot = NULL;     /* 白色圆点 */
static bool s_prev_pressed = false;

/* 淡出动画执行回调：设置透明度 */
static void dot_fade_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, value, 0);
}

/* 淡出动画完成：隐藏圆点 */
static void dot_fade_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

/* 抬起后 1s 内渐变消失 */
static void dot_fade_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_dot);
    lv_anim_set_exec_cb(&a, dot_fade_anim_cb);
    lv_anim_set_ready_cb(&a, dot_fade_ready_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, TOUCH_TEST_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/* 触摸事件回调：由 lvgl_port 的 LVGL 输入设备轮询驱动（复用触摸数据，无额外定时器）。
   按下时圆点跟随手指，抬起后渐变消失。 */
static void touch_test_touch_cb(bool pressed, uint16_t tx, uint16_t ty, void *user_ctx)
{
    if (s_dot == NULL) return;

    if (pressed) {
        /* 按下：取消进行中的淡出，恢复不透明，移到最上层（可能在 UI 之前创建），跟随手指 */
        lv_anim_del(s_dot, dot_fade_anim_cb);
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_dot, LV_OPA_COVER, 0);
        lv_obj_move_foreground(s_dot);
        lv_obj_set_pos(s_dot, tx - TOUCH_TEST_DOT_SIZE / 2, ty - TOUCH_TEST_DOT_SIZE / 2);
    } else if (s_prev_pressed) {
        /* 刚抬起：开始渐变消失 */
        dot_fade_out();
    }
    s_prev_pressed = pressed;
}

void touch_test_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 白色圆点（默认隐藏，不拦截触摸） */
    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, TOUCH_TEST_DOT_SIZE, TOUCH_TEST_DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

    /* 事件穿透：圆点只是视觉指示，绝不参与命中测试、不拦截下层控件。
       v8 时代额外挂了一个 LV_EVENT_HIT_TEST 回调把结果置 false；LVGL 9 里
       `lv_hit_test_info_t` 变成了不透明类型（定义在私有头 lv_obj_event_private.h），
       而且命中测试的第一道判据就是 LV_OBJ_FLAG_CLICKABLE（见 lv_obj_pos.c 的
       lv_obj_hit_test：无该标志直接返回 false，连 HIT_TEST 事件都不会发），
       所以上面这条 clear_flag 已经足够，回调就此删除。 */

    /* 注册触摸事件回调：复用 lvgl_port 的触摸轮询，不再单独创建定时器 */
    lvgl_port_register_touch_cb(touch_test_touch_cb, NULL);

    ESP_LOGI(TAG, "Touch test created");
}
