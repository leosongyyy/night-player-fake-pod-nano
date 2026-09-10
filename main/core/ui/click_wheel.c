#include "core/ui/click_wheel.h"

#include <math.h>

#include "core/hardware/display.h"
#include "core/ui/lvgl_port.h"
#include "ui/toast.h"

#include "esp_log.h"

static const char *TAG = "click_wheel";

/* 圆周率：M_PI 在部分 toolchain 的严格 ANSI 模式下不定义，这里自备 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* 外观/行为参数                                                        */
/* ------------------------------------------------------------------ */

/* 显示阈值：超过 10 页（每页 7.2 条 → 72 条）才显示，短列表用手指滑更快 */
#define CW_MIN_ITEMS      73

/* 按钮外观：右下角黄色圆形悬浮按钮 */
#define CW_SIZE           56                  /* 按钮直径（px） */
#define CW_MARGIN         32                  /* 距屏幕右边距（px，原 16 + 16） */
#define CW_MARGIN_BOTTOM  32                  /* 距屏幕下边距（px，原 16 + 16） */
#define CW_COLOR          lv_color_hex(0xFFC000)  /* 黄色（与设置页高亮同色） */
#define CW_COLOR_PRESSED  lv_color_hex(0xE0A800)  /* 按下加深 */
#define CW_ICON_COLOR     lv_color_hex(0x1E1E1E)  /* 深灰图标（同卡片底色） */
#define CW_ICON           LV_SYMBOL_REFRESH  /* 旋转图标（LVGL 内置符号） */
/* 图标字体：用 LVGL 内置 montserrat（系统字体不含 PUA 区 LV_SYMBOL 字形，
   与 virtual_list 的行图标同一处理） */
#define CW_ICON_FONT      (&lv_font_montserrat_20)

/* 一圈（360°）滚动内容总高度的比例：20% → **5 圈滑到底**
   （原为 10%＝10 圈到底，2026-09-10 按用户要求提高灵敏度；灵敏度只由
   这一个常量决定，改它就是改"几圈到底"：圈数 = 1 / CW_TURN_RATIO） */
#define CW_TURN_RATIO     0.2f
/* 每帧最大滚动量（px）：防止触摸异常跳变（如坐标瞬时漂移 180°）时列表乱飞。
   正常转动每帧约 10~40°，460x460 屏 600 首列表下约 140~560px
   ⚠ 该上限必须随 CW_TURN_RATIO 同步放大（否则快速画圈时单帧滚动量被削顶，
   表现成"要转十几圈才到底"，与灵敏度设置矛盾）；180° 的坐标跳变仍会产生
   数千 px，照旧被钳制，跳变保护不受影响 */
#define CW_MAX_STEP_PX    800

/* 画圈指示：屏幕上一圈 12 个灰色小圆点（跟随旋转圆心，即屏幕中心） */
#define CW_RING_DOTS      12     /* 圆点数量 */
#define CW_RING_RADIUS    130    /* 圆环半径（px，以屏幕中心为圆心） */
#define CW_RING_DOT_SIZE  20     /* 圆点直径（px） */
#define CW_RING_DOT_COLOR lv_color_hex(0x808080)  /* 灰色（中性、不抢眼） */
/* 圆点起始角：从屏幕正上方（12 点钟方向）开始均分 12 份，视觉上像一个
   表盘；角度用屏幕坐标系（x 右 y 下），故正上方为 -90° */
#define CW_RING_START_DEG (-90.0f)

/* 说明 Toast 停留时长（ms）：三行文字，给用户足够阅读时间 */
#define CW_HINT_MS        4000

/* 角度调试日志间隔（度）：每累计转动这么多度打一条日志，便于串口验证 */
#define CW_LOG_STEP_DEG   30.0f

/* ------------------------------------------------------------------ */
/* 模块状态（全局唯一实例：按钮挂在 lv_scr_act 屏幕层，由当前页面驱动）    */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_btn        = NULL;  /* 按钮对象（lv_scr_act() 的子对象） */
static lv_obj_t *s_lists[CLICK_WHEEL_TAB_COUNT] = { NULL, NULL }; /* 各页面的列表 */
static uint32_t  s_counts[CLICK_WHEEL_TAB_COUNT] = { 0, 0 };      /* 各页面的条目数 */
static uint8_t   s_tab        = 0;     /* 当前主界面页面下标 */
static bool      s_overlay    = false; /* 全屏覆盖层（设置菜单）是否显示中 */
/* 功能开关（NVS 配置 click_wheel，默认开）：关闭后按钮永不显示。
   由 UI 层在创建后按 config_store_get_click_wheel() 设置，并在设置菜单
   切换时调用 click_wheel_set_enabled() 立即生效（本模块不依赖 config_store，
   保持 core/ui 与应用配置解耦） */
static bool      s_enabled    = true;

/* 当前生效的滚动目标（由 s_tab 决定，见 cw_target） */
static lv_obj_t *s_cur_list   = NULL;  /* 正在被滚动的列表 */
static uint32_t  s_cur_count  = 0;     /* 该列表条目总数 */

/* 画圈状态 */
static bool      s_active     = false; /* 是否处于"按住画圈"状态 */
static bool      s_rotated    = false; /* 本次按住期间是否真的转过（决定点击是否弹说明） */
static float     s_last_deg   = 0.0f;  /* 上一次采样角度（度） */
static float     s_acc_deg    = 0.0f;  /* 本次按住累计转动角度（仅用于调试日志） */

/* 画圈指示圆点（12 个灰色小圆，围成以屏幕中心为圆心的圈）：
   创建一次常驻隐藏，画圈期间显示，松手立即隐藏 */
static lv_obj_t *s_dots[CW_RING_DOTS] = { NULL };
static bool      s_dots_visible = false;

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */

/* 前向声明：圆点创建要用到命中穿透回调（定义在后面的事件回调区） */
static void cw_icon_pass_through_cb(lv_event_t *e);

/* 创建 12 个灰色圆点（以屏幕中心为圆心、CW_RING_RADIUS 为半径均分）。
   只建一次；平时 HIDDEN，画圈时显示。圆点不接收触摸（纯视觉指示）。 */
static void cw_ring_dots_create(void)
{
    lv_obj_t *parent = lv_scr_act();   /* 与按钮同层（屏幕层），位于主界面之上 */
    for (uint8_t i = 0; i < CW_RING_DOTS; i++) {
        float deg = CW_RING_START_DEG +
                    (360.0f / (float)CW_RING_DOTS) * (float)i;
        float rad = deg * (float)M_PI / 180.0f;
        /* 屏幕坐标系：x 右 y 下，故 x=cos、y=sin；圆心 = 屏幕中心 */
        lv_coord_t cx = (lv_coord_t)((float)EXAMPLE_LCD_H_RES / 2.0f +
                                     cosf(rad) * (float)CW_RING_RADIUS);
        lv_coord_t cy = (lv_coord_t)((float)EXAMPLE_LCD_V_RES / 2.0f +
                                     sinf(rad) * (float)CW_RING_RADIUS);

        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, CW_RING_DOT_SIZE, CW_RING_DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, CW_RING_DOT_COLOR, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(dot, cw_icon_pass_through_cb, LV_EVENT_HIT_TEST,
                            NULL);
        lv_obj_set_pos(dot, cx - CW_RING_DOT_SIZE / 2,
                       cy - CW_RING_DOT_SIZE / 2);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_dots[i] = dot;
    }
}

/* 显示/隐藏画圈指示圆点（幂等；状态未变时直接返回，避免重复遍历） */
static void cw_ring_dots_set_visible(bool visible)
{
    if (s_dots_visible == visible) {
        return;
    }
    for (uint8_t i = 0; i < CW_RING_DOTS; i++) {
        if (s_dots[i] == NULL) {
            continue;
        }
        if (visible) {
            lv_obj_clear_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_dots_visible = visible;
}

/* 计算手指相对**屏幕中心**的角度（度，屏幕坐标系：x 右 y 下，顺时针为正）。
   圆心固定取屏幕中点（需求原意）：手指绕按钮小圈转时，相对屏幕中心的角度
   变化量被半径比例缩小，转一圈的滚动量由 CW_TURN_RATIO 统一标定。 */
static float cw_angle_deg(uint16_t x, uint16_t y)
{
    float dx = (float)x - (float)EXAMPLE_LCD_H_RES / 2.0f;
    float dy = (float)y - (float)EXAMPLE_LCD_V_RES / 2.0f;
    /* 手指正好落在屏幕中心（几乎不可能）：atan2(0,0) 无意义，沿用上次角度 */
    if (dx == 0.0f && dy == 0.0f) {
        return s_last_deg;
    }
    return atan2f(dy, dx) * 180.0f / (float)M_PI;
}

/* 同步按钮显隐：仅在"功能开关开启 + 当前页面是列表页 + 该页列表条目超过
   阈值 + 无全屏覆盖层"四者同时满足时显示 */
static void cw_sync_visible(void)
{
    if (s_btn == NULL) {
        return;
    }
    bool show = s_enabled &&
                (!s_overlay) &&
                (s_tab < CLICK_WHEEL_TAB_COUNT) &&
                (s_lists[s_tab] != NULL) &&
                (s_counts[s_tab] >= CW_MIN_ITEMS);
    if (show) {
        lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
        s_active = false;   /* 隐藏即终止进行中的画圈 */
        cw_ring_dots_set_visible(false);   /* 一并收起指示圆点 */
    }
}

/* 取当前页面生效的滚动目标 */
static void cw_target(void)
{
    if (s_tab < CLICK_WHEEL_TAB_COUNT && s_lists[s_tab] != NULL) {
        s_cur_list  = s_lists[s_tab];
        s_cur_count = s_counts[s_tab];
    } else {
        s_cur_list  = NULL;
        s_cur_count = 0;
    }
}

/* 按角度差滚动列表：顺时针（角度增大）向下滚，逆时针向上滚。
   lv_obj_scroll_by_bounded 自带边界钳制，滚到顶/底不会出现过冲。 */
static void cw_scroll_by_deg(float delta_deg)
{
    if (s_cur_list == NULL || s_cur_count == 0 || delta_deg == 0.0f) {
        return;
    }

    /* 内容总高度 = 视口高度 + 可滚动范围（从滚动范围反推，避免依赖
       虚拟列表内部结构） */
    lv_coord_t view_h    = lv_obj_get_height(s_cur_list);
    lv_coord_t range     = lv_obj_get_scroll_bottom(s_cur_list) +
                           lv_obj_get_scroll_top(s_cur_list);
    lv_coord_t content_h = view_h + range;
    if (content_h <= view_h) {
        return;   /* 内容不足一屏：无需滚动 */
    }

    /* 一圈滚动 20% 内容高度（5 圈到底）→ 每度滚动量。
       ⚠ 方向：屏幕坐标系里顺时针 = 角度增大；需求是"顺时针向下滚"，
       而 lv_obj_scroll_by 的 dy 为正表示内容上移（视口向下），故取负号
       （顺时针 → dy 为正 → 列表向下翻）。 */
    float px_per_deg = (float)content_h * CW_TURN_RATIO / 360.0f;
    float px = -delta_deg * px_per_deg;
    if (px > CW_MAX_STEP_PX) {
        px = CW_MAX_STEP_PX;
    } else if (px < -CW_MAX_STEP_PX) {
        px = -CW_MAX_STEP_PX;
    }
    if (px > -1.0f && px < 1.0f) {
        return;   /* 不足 1px 不滚动，省掉无意义的重绑 */
    }

    lv_obj_scroll_by_bounded(s_cur_list, 0, (lv_coord_t)px, LV_ANIM_OFF);

    /* 累计转动角度：每超过 CW_LOG_STEP_DEG 打一条日志（串口验证用） */
    s_acc_deg += delta_deg;
    if (fabsf(s_acc_deg) >= CW_LOG_STEP_DEG) {
        ESP_LOGI(TAG, "rotate %.0f deg -> scroll %d px (scroll_y=%d)",
                 (double)s_acc_deg, (int)px, (int)lv_obj_get_scroll_y(s_cur_list));
        s_acc_deg = 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* 触摸回调（由 lvgl_port 的输入设备轮询驱动，运行在 LVGL 任务、已持锁）   */
/* ------------------------------------------------------------------ */

static void cw_touch_cb(bool pressed, uint16_t x, uint16_t y, void *user_ctx)
{
    (void)user_ctx;

    if (!s_active || !pressed) {
        return;   /* 未在画圈（抬起由按钮事件处理） */
    }
    /* ⚠ 这里**不再**用 cw_touch_on_button 过滤：画圈时手指必然会移出按钮
       范围（按钮只有 56px），一过滤就只剩按钮内那点角度变化，表现为
       "拖不动"。起手是否按在按钮上由 LV_EVENT_PRESSED 保证（只有按在按钮
       上才会进入 s_active），之后整根手指的移动都照常采样。 */

    float deg   = cw_angle_deg(x, y);
    float delta = deg - s_last_deg;
    /* 归一化到 (-180, 180]：处理跨 0°/360° 边界的跳变 */
    while (delta > 180.0f) {
        delta -= 360.0f;
    }
    while (delta < -180.0f) {
        delta += 360.0f;
    }
    s_last_deg = deg;

    if (delta != 0.0f) {
        s_rotated = true;
        cw_scroll_by_deg(delta);
    }
}

/* ------------------------------------------------------------------ */
/* 按钮事件                                                            */
/* ------------------------------------------------------------------ */

/* 按下：进入画圈状态，记录起始角度（松手后据此判断是"点击"还是"画圈"） */
static void cw_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p = { 0, 0 };
    if (indev != NULL) {
        lv_indev_get_point(indev, &p);
    }
    cw_target();   /* 按下瞬间锁定滚动目标（防止画圈途中切页/换列表） */
    s_last_deg = cw_angle_deg((uint16_t)p.x, (uint16_t)p.y);
    s_acc_deg  = 0.0f;
    s_rotated  = false;
    s_active   = true;
    cw_ring_dots_set_visible(true);   /* 显示画圈指示圆点 */
}

/* 松开：按住期间没转过 → 视为直接点击，弹出使用说明 */
static void cw_released_cb(lv_event_t *e)
{
    (void)e;
    s_active = false;
    cw_ring_dots_set_visible(false);   /* 隐藏画圈指示圆点 */
    if (!s_rotated) {
        ui_toast_show_ms("你点击的是 ClickWheel\n"
                         "按住该按钮并在屏幕上画圈\n可以快速翻页",
                         CW_HINT_MS);
    }
}

/* 图标命中穿透：图标只是装饰，点击/拖动都归属按钮本体 */
static void cw_icon_pass_through_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    if (info != NULL) {
        info->res = false;
    }
}

/* 按下丢失（极端情况：按钮被隐藏/对象被删等）：退出画圈状态，
   避免后续触摸继续按旧角度滚列表 */
static void cw_press_lost_cb(lv_event_t *e)
{
    (void)e;
    s_active = false;
    cw_ring_dots_set_visible(false);   /* 隐藏画圈指示圆点 */
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

lv_obj_t *click_wheel_create(void)
{
    if (s_btn != NULL) {
        return s_btn;   /* 已创建：幂等 */
    }

    /* 父对象 = 当前屏幕 lv_scr_act()：主界面 tabview 的**兄弟**且比 tabview
       晚创建 → 天然位于整个主界面之上。
       ⚠ 放在 tab 页容器内时，从按钮起手拖动会被 LVGL 的滚动链
       （SCROLL_CHAIN）找到可横向滚动的 tabview，判定为翻页并给按钮发
       PRESS_LOST，画圈直接失效；放到屏幕层（tabview 之外）+ 清除滚动链后
       事件不再外泄。
       ⚠ **不要**改挂 lv_layer_top()：那样它会浮在下拉控制中心之上（曾出现
       的 Z 序 Bug），而"把面板也挂顶层"的修法会破坏 LVGL 的覆盖判定——
       lv_refr_get_top_obj() 只在当前屏幕的子对象树里寻找覆盖绘制块的不透明
       对象，面板一旦离开屏幕树，被它遮住的页面内容（全屏封面大图等）每帧
       都会被重绘一遍，导致下拉控制中心严重掉帧（见 DONE_PLANS.md 第 38 点）。
       面板与本按钮同在屏幕层，Z 序由创建顺序（main_ui_create →
       notification_panel_create）保证面板在上。 */
    lv_obj_t *parent = lv_scr_act();

    s_btn = lv_obj_create(parent);
    lv_obj_set_size(s_btn, CW_SIZE, CW_SIZE);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn, CW_COLOR, 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, CW_COLOR_PRESSED, LV_STATE_PRESSED);
    /* 不可滚动 + 断开滚动链：拖动按钮时 LVGL 不会去父级（顶层/屏幕）
       寻找可滚动对象，tabview 的左右翻页手势不会被劫走 */
    lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(s_btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_RIGHT, -CW_MARGIN, -CW_MARGIN_BOTTOM);
    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);   /* 初始隐藏，由页面驱动显示 */

    /* 图标：深灰旋转箭头，居中、不拦截点击（同样断开滚动链） */
    lv_obj_t *icon = lv_label_create(s_btn);
    lv_label_set_text(icon, CW_ICON);
    lv_obj_set_style_text_font(icon, CW_ICON_FONT, 0);
    lv_obj_set_style_text_color(icon, CW_ICON_COLOR, 0);
    lv_obj_center(icon);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(icon, cw_icon_pass_through_cb, LV_EVENT_HIT_TEST, NULL);

    /* 事件：按下开始画圈 / 松开判断点击 / 按下丢失兜底 */
    lv_obj_add_event_cb(s_btn, cw_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, cw_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_btn, cw_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

    /* 触摸订阅：画圈期间需要持续读取手指绝对坐标（按钮事件不带坐标） */
    lvgl_port_register_touch_cb(cw_touch_cb, NULL);

    /* 画圈指示：12 个灰色圆点围成一圈（常驻隐藏，画圈时显示） */
    cw_ring_dots_create();

    ESP_LOGI(TAG, "click wheel created on screen layer (%dx%d px, margin %d/%d, "
                  "ring %d dots r=%d)",
             CW_SIZE, CW_SIZE, CW_MARGIN, CW_MARGIN_BOTTOM,
             CW_RING_DOTS, CW_RING_RADIUS);
    return s_btn;
}

void click_wheel_set_list(uint8_t tab, lv_obj_t *list, uint32_t item_count)
{
    if (tab >= CLICK_WHEEL_TAB_COUNT) {
        return;
    }
    s_lists[tab]  = list;
    s_counts[tab] = item_count;
    cw_target();
    cw_sync_visible();
    ESP_LOGI(TAG, "tab %u list set: items=%u (%s)", (unsigned)tab,
             (unsigned)item_count,
             item_count >= CW_MIN_ITEMS ? "wheel eligible" : "wheel hidden");
}

void click_wheel_set_tab(uint8_t tab)
{
    s_tab = tab;
    cw_target();
    cw_sync_visible();
}

void click_wheel_set_overlay(bool visible)
{
    if (visible == s_overlay) {
        return;   /* 幂等：状态未变不重算显隐 */
    }
    s_overlay = visible;
    cw_sync_visible();
}

void click_wheel_set_enabled(bool on)
{
    if (on == s_enabled) {
        return;   /* 幂等：状态未变不重算显隐 */
    }
    s_enabled = on;
    cw_sync_visible();   /* 关闭时立即隐藏按钮并终止画圈 */
    ESP_LOGI(TAG, "click wheel %s", on ? "enabled" : "disabled");
}
