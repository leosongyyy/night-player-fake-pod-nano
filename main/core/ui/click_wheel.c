#include "core/ui/click_wheel.h"

#include <math.h>

#include "core/hardware/display.h"
#include "core/ui/lvgl_port.h"
#include "core/ui/virtual_list.h"
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

/* 圈数模型（2026-09 用户数学建模）：滚完**整个列表**所需的圈数 y 由列表
   条目数 x 决定——对数增长，短列表少转几圈、长列表圈数缓慢增加：

       y = log_1.618(x) - 7        // log_1.618(x) = ln(x) / ln(1.618)

   y 钳制在 [CW_TURNS_MIN, CW_TURNS_MAX] = [1, 20]。
   设计意图：条目越多，单圈必须跨越多行才可能在有限圈数内到底；而圈数按
   对数增长，避免"600 首转一圈飞过 120 行、完全看不清"。
   标定值（x → y）：
        73 条 → 1.92 圈     200 条 → 4.01 圈     600 条 → 6.29 圈
      1000 条 → 7.36 圈    5000 条 → 10.70 圈  10000 条 → 12.14 圈
   ⚠ 钳制在现有列表规模（几十~几千条）下不生效：y = 1 需 x ≤ 47 条（低于
   CW_MIN_ITEMS 显示阈值），y = 20 需 x ≈ 44 万条；保留钳制只为公式完整性。 */
#define CW_TURNS_LOG_BASE   1.618f  /* 对数底（黄金比例） */
#define CW_TURNS_LOG_OFFSET 7.0f    /* 对数项的偏移（-7） */
#define CW_TURNS_MIN        1.0f    /* 圈数下限 */
#define CW_TURNS_MAX        20.0f   /* 圈数上限 */

/* 速度档位系数表（下标 = CLICK_WHEEL_SPEED_*）：实际圈数 = 模型圈数 × 系数。
   1.00 / 0.75 / 0.50 —— **0.25 一档均匀变化**（2026-09-11 用户要求：快速由
   早先的"÷1.5"改为 ×0.75，与极速 ×0.5 构成等差档位）。系数越小，圈数越少、
   手感越灵敏。⚠ 只作用于"几圈到底"；行对齐步进、单帧钳制（1/4 圈）、方向
   都不受影响 */
static const float CW_SPEED_FACTOR[CLICK_WHEEL_SPEED_COUNT] = {
    1.00f,   /* 普通：不计算 */
    0.75f,   /* 快速 */
    0.50f,   /* 极速 */
};

/* 档位名（串口日志用；下标同 CLICK_WHEEL_SPEED_*） */
static const char *const CW_SPEED_NAME[CLICK_WHEEL_SPEED_COUNT] = {
    "normal", "fast", "turbo",
};

/* 单帧滚动量上限：取**当前圈数标定出的角度上限**，即单帧最多计入
   CW_MAX_STEP_DEG（90° = 1/4 圈）对应的滚动量。
   ⚠ 为什么不用固定像素上限：同一个固定值（原 800px）对 73 条（每圈约 34 行）
   是半个圈，对 10000 条（每圈约 830 行）却只是几十分之一圈——快速画圈时
   单帧滚动量被削顶，表现成"要转十几圈才到底"，与圈数模型自相矛盾。
   正常转动每帧约 10~40°，远低于 90°；坐标瞬时跳变（如 180°）仍会被钳到
   1/4 圈，跳变保护不丢（详见 ideas_and_exps/CLICK_WHEEL_USAGE.md 2.3）。 */
#define CW_MAX_STEP_DEG     90.0f

/* 行对齐步进（核心手感）：本组件内部用浮点累加器累加"角度换算出的连续
   像素量"，但**只把 item_height 的整数倍**交给列表（见 cw_scroll_by_deg）。
   于是列表位置恒为行高的整数倍 → virtual_list 的行槽永远停在整数行上，
   转动时看到的是"行槽位置不动、里面的内容一格一格换"（阶梯式翻行），
   而不是平滑滑动半个行高。长列表快速画圈时一帧可跨多行（上限 = 1/4 圈对应的
   行数，即"每圈行数 / 4"）。 */

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
/* 滑动方向（NVS 配置 click_wheel_rev，默认正常）：true = 反转（顺时针向上滚）。
   同样由 UI 层通过 click_wheel_set_reverse() 驱动，本模块不依赖 config_store */
static bool      s_reverse    = false;
/* 速度档位（NVS 配置 clkwheel_spd，默认普通）：index 到 CW_SPEED_FACTOR。
   由 click_wheel_set_speed() 驱动，本模块不依赖 config_store */
static uint8_t   s_speed      = CLICK_WHEEL_SPEED_NORMAL;

/* 当前生效的滚动目标（由 s_tab 决定，见 cw_target） */
static lv_obj_t *s_cur_list   = NULL;  /* 正在被滚动的列表 */
static uint32_t  s_cur_count  = 0;     /* 该列表条目总数 */
static lv_coord_t s_cur_item_h = 0;    /* 该列表的行高（整行步进的步长） */
static float     s_cur_turns  = CW_TURNS_MIN; /* 该列表"滚完需几圈"（条目数经
                                                 圈数模型算出，见 cw_turns_for_count） */

/* 行对齐步进状态：只保存"不足一行的滚动量余量"（浮点像素）。
   ⚠ 步进是**相对列表当前位置**的（每帧按行高整数倍 by 出去），刻意不做
   "对齐到整行网格"的吸附——列表停在什么位置，就按那个样子一格格跳，
   行槽（含小数相位）保持不动、只有里面的条目在换。 */
static float     s_scroll_acc = 0.0f;

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
   变化量被半径比例缩小，转一圈的滚动量由圈数模型（条目数 → 几圈到底）统一
   标定，见文件头常量注释。 */
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

/* 圈数模型：由条目数 x 算"滚完整个列表需要几圈" y = log_1.618(x) - 7，
   钳制到 [CW_TURNS_MIN, CW_TURNS_MAX]（2026-09 用户建模，标定值见文件头
   常量注释）。只在**锁定滚动目标时**算一次（按下/换列表时），此处两次
   logf 的开销可以忽略；转圈过程中条目数不会变。 */
static float cw_turns_for_count(uint32_t count)
{
    if (count < 2) {
        return CW_TURNS_MIN;   /* log 定义域保护（实际列表要么为空、要么 ≥ 1 条） */
    }
    float y = logf((float)count) / logf(CW_TURNS_LOG_BASE) - CW_TURNS_LOG_OFFSET;
    if (y < CW_TURNS_MIN) {
        y = CW_TURNS_MIN;
    } else if (y > CW_TURNS_MAX) {
        y = CW_TURNS_MAX;
    }
    return y;
}

/* 有效圈数 = 圈数模型输出 × 速度档位系数（普通 1.00 / 快速 0.75 / 极速 0.50）。
   目标锁定（cw_target）与列表登记日志都用它，保证串口读数与当前档位一致 */
static float cw_effective_turns(uint32_t count)
{
    return cw_turns_for_count(count) * CW_SPEED_FACTOR[s_speed];
}

/* 取当前页面生效的滚动目标（含行高：按整行步进要知道步长；含圈数：滚动量
   由条目数按圈数模型标定，再乘当前速度档位系数） */
static void cw_target(void)
{
    if (s_tab < CLICK_WHEEL_TAB_COUNT && s_lists[s_tab] != NULL) {
        s_cur_list   = s_lists[s_tab];
        s_cur_count  = s_counts[s_tab];
        s_cur_item_h = virtual_list_get_item_height(s_cur_list);
        s_cur_turns  = cw_effective_turns(s_cur_count);
    } else {
        s_cur_list   = NULL;
        s_cur_count  = 0;
        s_cur_item_h = 0;
        s_cur_turns  = CW_TURNS_MIN;
    }
}

/* 按角度差滚动列表：顺时针（角度增大）向下滚，逆时针向上滚。
   ⚠ 本函数**只把行高的整数倍**交给列表（行对齐步进）：
   内部把角度换算出的连续像素量累加进浮点累加器 s_scroll_acc，每帧只取出
   其中"已满整行"的部分（朝 0 截断）交给列表，不足一行的余量留在累加器里
   下次一起算——所以慢速转动不会像以前那样丢掉不足 1px 的量。
   因为输出恒为行高整数倍，行槽在屏幕上的位置（含当前的小数相位）保持不动，
   转动时呈现"行位置不变、内容一格一格换"的效果。
   ⚠ 附带收益（意外发现，见 CLICK_WHEEL_USAGE.md 2.3.2）：位移由角度差算出的
   "格数"决定、与帧数无关，所以**掉帧不再累积成位移损失**；而且帧变慢时平滑滚动
   会表现为一次突进（被眼睛读作卡顿），按格滚动只表现为"这一下多跳了几格"，
   原本快速滚动时的卡顿感在体感上消失。注意这只改观感，不省绘制开销
   （每帧仍是一次列表区整块重绘），底层帧率问题别拿它当遮羞布。
   边界：**不做任何钳制**，交给 lv_obj_scroll_by_bounded 自己 clamp——
   滚到最底下时多余的滚动量被列表丢弃，列表就正好停在"最后一项贴底"
   （顶部同理停在 0，即第一项贴顶）。 */
static void cw_scroll_by_deg(float delta_deg)
{
    if (s_cur_list == NULL || s_cur_count == 0 || delta_deg == 0.0f) {
        return;
    }

    /* 可滚动行程 = 内容总高度 - 视口高度 = scroll_top + scroll_bottom
       （两者之和与当前滚动位置无关，是常量）：这正是"滚完整个列表"要走过
       的距离，圈数模型 y 标定的就是这段行程。从滚动范围反推，避免依赖
       虚拟列表内部结构 */
    lv_coord_t range = lv_obj_get_scroll_bottom(s_cur_list) +
                       lv_obj_get_scroll_top(s_cur_list);
    if (range <= 0) {
        return;   /* 内容不足一屏：无需滚动 */
    }

    /* 每度滚动量 = 全部行程 / (圈数 × 360°)：转满 s_cur_turns
       （= 圈数模型输出，快速模式已减半）圈正好从顶部滚到底部。
       ⚠ 方向：屏幕坐标系里顺时针 = 角度增大；需求是"顺时针向下滚"，
       而 lv_obj_scroll_by 的 dy 为负才是内容上移（视口向下），故取负号
       （顺时针 → dy 为负 → 列表向下翻行）。
       滑动方向设为"反转"时整体取反：顺时针 → 向上滚，逆时针 → 向下滚 */
    float px_per_deg = (float)range / (s_cur_turns * 360.0f);
    float dir_sign   = s_reverse ? 1.0f : -1.0f;
    float px = dir_sign * delta_deg * px_per_deg;
    /* 单帧上限 = 1/4 圈对应的量（随行程/圈数一起标定，见 CW_MAX_STEP_DEG）：
       正常转动（10~40°/帧）不受影响，坐标跳变仍被钳住 */
    float max_px = px_per_deg * CW_MAX_STEP_DEG;
    if (px > max_px) {
        px = max_px;
    } else if (px < -max_px) {
        px = -max_px;
    }

    /* 1) 内部累加连续像素量（不足一行的余量保留，不丢弃） */
    s_scroll_acc += px;

    /* 行高未知（非 virtual_list 或取值失败）：退化为按像素滚动 */
    if (s_cur_item_h <= 0) {
        s_scroll_acc = 0.0f;
        lv_obj_scroll_by_bounded(s_cur_list, 0, (lv_coord_t)px, LV_ANIM_OFF);
        return;
    }

    /* 2) 只取出"整行"的部分：朝 0 截断，即转够一行才走一行。
       （余量上限 = 一行，反向转动最多先"还"掉一行就会立刻响应） */
    long rows = (long)(s_scroll_acc / (float)s_cur_item_h);
    if (rows == 0) {
        return;   /* 不足一行：余量留在累加器里等下次 */
    }
    s_scroll_acc -= (float)(rows * s_cur_item_h);   /* 扣掉已交给列表的量 */

    /* 3) 交给列表：行高的整数倍（dy 为负 = 向下翻行），越界由列表自己 clamp */
    lv_obj_scroll_by_bounded(s_cur_list, 0,
                             (lv_coord_t)(rows * s_cur_item_h), LV_ANIM_OFF);

    /* 累计转动角度：每超过 CW_LOG_STEP_DEG 打一条日志（串口验证用） */
    s_acc_deg += delta_deg;
    if (fabsf(s_acc_deg) >= CW_LOG_STEP_DEG) {
        ESP_LOGI(TAG, "rotate %.0f deg -> %ld row(s) (%d px, scroll_y=%d)",
                 (double)s_acc_deg, rows,
                 (int)(rows * s_cur_item_h),
                 (int)lv_obj_get_scroll_y(s_cur_list));
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
    /* 每次按住重新开始累加余量（不碰列表位置：步进是相对的，
       按下时列表停在哪儿，就从哪儿开始一格格跳） */
    s_scroll_acc = 0.0f;
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
    ESP_LOGI(TAG, "tab %u list set: items=%u (%.2f turns to end, %s)",
             (unsigned)tab, (unsigned)item_count,
             (double)cw_effective_turns(item_count),
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

void click_wheel_set_reverse(bool reverse)
{
    if (reverse == s_reverse) {
        return;   /* 幂等 */
    }
    s_reverse = reverse;
    /* 方向在每帧换算时取用，无需重算滚动目标；只打日志便于串口核对 */
    ESP_LOGI(TAG, "click wheel direction: %s", reverse ? "reverse" : "normal");
}

void click_wheel_set_speed(uint8_t speed)
{
    if (speed >= CLICK_WHEEL_SPEED_COUNT) {
        speed = CLICK_WHEEL_SPEED_NORMAL;   /* 越界钳到"普通"（脏配置防护） */
    }
    if (speed == s_speed) {
        return;   /* 幂等 */
    }
    s_speed = speed;
    /* 系数参与每帧换算，重新锁定一次滚动目标把新系数应用到当前列表 */
    cw_target();
    if (s_cur_list != NULL) {
        ESP_LOGI(TAG, "click wheel speed: %s (x%.2f -> %.2f turns for %u items)",
                 CW_SPEED_NAME[s_speed], (double)CW_SPEED_FACTOR[s_speed],
                 (double)s_cur_turns, (unsigned)s_cur_count);
    } else {
        /* 当前页面没有绑定的列表（如停在非列表页）：只报档位，
           避免打出"0 items / 1.00 turns"这种误导性读数 */
        ESP_LOGI(TAG, "click wheel speed: %s (x%.2f, no list bound)",
                 CW_SPEED_NAME[s_speed], (double)CW_SPEED_FACTOR[s_speed]);
    }
}
