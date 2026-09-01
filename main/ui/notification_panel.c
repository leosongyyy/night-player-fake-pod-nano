#include "ui/notification_panel.h"
#include "ui/list_common.h"

#include "core/fonts/system_font.h"
#include "core/fonts/system_smaller_font.h"
#include "core/hardware/audio/audio.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/storage/config_store.h"
#include "core/utils/sleep_timer.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "notification_panel";

#define NOTIF_BAR_HEIGHT   EXAMPLE_LCD_V_RES  /* 全屏下拉面板 */
#define NOTIF_TRIGGER_H    UI_STATUS_BAR_H    /* 顶部触发区域高度（= 状态栏高度） */
#define PANEL_OPEN_Y       0                  /* 面板展开时顶部 y（全屏） */
#define PANEL_CLOSED_Y     (-NOTIF_BAR_HEIGHT) /* 收起时完全隐藏于屏幕上方 */

/* 拖拽跟随与速度决策 */
#define VEL_SAMPLES  6
#define VEL_THRESH   400   /* px/s，超过则按速度方向决定 */

/* ------------------------------------------------------------------ */
/* 控制中心布局参数                                                     */
/* ------------------------------------------------------------------ */

/* 顶部状态信息区高度（与主状态栏等高，左音量右电量） */
#define NOTIF_HEADER_H   UI_STATUS_BAR_H

/* 中间内容区：四周填充与列间距 */
#define CONTENT_PAD      20
#define COL_GAP          12

/* 左列圆形占位按钮 */
#define ROUND_BTN_DIAM   112   /* 直径（竖条宽度与其相同） */
#define ROUND_BTN_GAP    24    /* 上下两按钮间距 */

/* 亮度/音量竖条（iOS 控制中心风格：深灰圆角容器，内嵌贴边浅灰填充条 + 底部彩色图标） */
#define TRACK_W          ROUND_BTN_DIAM   /* 容器宽度（与圆形按钮同宽） */
#define TRACK_H          240   /* 容器高度 */
#define TRACK_RADIUS     28    /* 容器圆角 */
#define TRACK_FILL_RADIUS 4    /* 填充条圆角（小，超出容器圆角的部分被裁剪） */
#define TRACK_ICON_PAD_BOTTOM 20  /* 图标距容器底边（叠在填充条底部上方） */

/* 亮度滑动下限：引用 config_store 的全局下限（CONFIG_STORE_BRIGHTNESS_MIN，
   不允许滑到更低，防止屏幕完全看不清） */
#define BRIGHT_SLIDE_MIN  CONFIG_STORE_BRIGHTNESS_MIN

/* 亮度滑动灵敏度：每滑动 5px 调整 1%（亮度 0~255 = 100%，1% = 2.55 级） */
#define BRIGHT_PX_PER_PCT  5
/* 音量滑动灵敏度：每滑动 20px 调整 1 档 */
#define VOL_PX_PER_LEVEL   20
/* 滑动调音量的上限（24 档）：当前音量 ≥24 时向上滑动不再增加、只有向下
   滑动能降下来；25~30 档仅按键可调（防止滑动误操作把音量推过安全档损伤耳朵） */
#define VOL_SLIDE_MAX_LEVEL  24

/* iOS 控制中心风格竖条配色 */
#define TRACK_BG_COLOR     lv_color_hex(0x3A3A3C)   /* 深灰圆角容器（圆形按钮同色） */
#define TRACK_FILL_COLOR   lv_color_hex(0xCFCFCF)   /* 填充条浅灰 */
#define TRACK_ICON_BRIGHT  lv_color_hex(0xFF9500)   /* 亮度图标橙色 */
#define TRACK_ICON_VOL     lv_color_hex(0x007AFF)   /* 喇叭图标蓝色 */

/* 按下高亮色（与设置页九宫格一致） */
#define NOTIF_PRESS_COLOR  lv_color_hex(0xFFC000)

/* 音量图标固定用"中音量"：界面只看数字档位（与顶部状态栏一致） */
#define NOTIF_VOL_ICON     LV_SYMBOL_VOLUME_MID

static lv_obj_t *notif_bar = NULL;

/* 顶部状态信息区（左音量 / 右电量，与主状态栏数据同步） */
static lv_obj_t *header_vol_label = NULL;
static lv_obj_t *header_bat_label = NULL;

/* 亮度竖条 */
static lv_obj_t *bright_fill = NULL;

/* 音量竖条 */
static lv_obj_t *vol_fill = NULL;

/* 面板拖拽状态 */
static bool dragging = false;
static int32_t drag_press_y = 0;
static int32_t drag_panel_start_y = 0;
static int32_t v_t[VEL_SAMPLES];
static int32_t v_y[VEL_SAMPLES];
static uint8_t v_idx = 0;
static uint8_t v_cnt = 0;

/* 亮度滑动状态（起点值 + 起点 y + dy 下限）。
   dy 触下限钳制时起点锚点跟随手指漂移（见 bright_slide_event_cb），
   保证滑到下限后往回滑立即响应 */
static bool bright_dragging = false;
static int32_t bright_drag_start_y = 0;   /* 起点锚点（dy 钳制时会漂移） */
static int32_t bright_start_val = 0;      /* 按下时的亮度（整个按压期间不变） */
static int32_t bright_dy_min = 0;         /* dy 下限：亮度降到 BRIGHT_SLIDE_MIN 所需位移 */

/* 音量滑动状态（起点档位 + 起点 y，PRESSING 中按位移换算新档位） */
static bool vol_dragging = false;
static int32_t vol_drag_start_y = 0;
static int32_t vol_start_level = 0;

/* ------------------------------------------------------------------ */
/* 面板拖拽（下拉/上拉展开收起，纯 LVGL 事件驱动）                       */
/* ------------------------------------------------------------------ */

/* 下拉通知栏位置动画回调 */
static void notif_anim_cb(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, value);
}

/* 通知栏动画到指定位置 */
static void notif_animate_to(int32_t target)
{
    int32_t cur = lv_obj_get_y(notif_bar);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, notif_bar);
    lv_anim_set_exec_cb(&a, notif_anim_cb);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 通知栏/状态栏触摸事件回调：由 LVGL 输入设备事件驱动（复用 indev 触摸数据，无额外轮询）。
   - PRESSED：在顶部触发区（状态栏）或底部把手区按下时开始拖拽；
   - PRESSING：面板跟随手指，并记录速度样本；
   - RELEASED：按速度矢量决定展开/收起。
   滑块等可点击子控件会自行消费事件（不冒泡），不会触发面板拖拽。 */
static void notif_panel_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    uint16_t ty = point.y;

    switch (code) {
    case LV_EVENT_PRESSED: {
        bool in_top_grab    = (ty < NOTIF_TRIGGER_H);
        bool in_bottom_grab = (ty >= EXAMPLE_LCD_V_RES - NOTIF_TRIGGER_H);
        if (!dragging && (in_top_grab || in_bottom_grab)) {
            dragging = true;
            drag_press_y = ty;
            drag_panel_start_y = lv_obj_get_y(notif_bar);
            v_idx = 0;
            v_cnt = 0;
            lv_anim_del(notif_bar, notif_anim_cb);  /* 停止进行中的动画，避免抢占 */
        }
        break;
    }
    case LV_EVENT_PRESSING: {
        if (!dragging) break;
        int32_t new_y = drag_panel_start_y + (int32_t)(ty - drag_press_y);
        if (new_y > PANEL_OPEN_Y) new_y = PANEL_OPEN_Y;
        if (new_y < PANEL_CLOSED_Y) new_y = PANEL_CLOSED_Y;
        lv_obj_set_y(notif_bar, new_y);

        v_t[v_idx] = lv_tick_get();
        v_y[v_idx] = ty;
        v_idx = (v_idx + 1) % VEL_SAMPLES;
        if (v_cnt < VEL_SAMPLES) v_cnt++;
        break;
    }
    case LV_EVENT_RELEASED: {
        if (!dragging) break;
        dragging = false;

        int32_t vel = 0;
        if (v_cnt >= 2) {
            uint8_t last = (v_idx - 1 + VEL_SAMPLES) % VEL_SAMPLES;
            uint8_t span = (v_cnt > VEL_SAMPLES) ? VEL_SAMPLES : v_cnt;
            uint8_t old  = (v_idx + VEL_SAMPLES - span) % VEL_SAMPLES;
            int32_t dt   = v_t[last] - v_t[old];
            if (dt > 0) {
                vel = (v_y[last] - v_y[old]) * 1000 / dt;   /* px/s，正值=向下 */
            }
        }

        int32_t cur_y = lv_obj_get_y(notif_bar);
        bool open;
        if (vel > VEL_THRESH)        open = true;                    /* 快速下滑 -> 展开 */
        else if (vel < -VEL_THRESH)  open = false;                   /* 快速上滑 -> 收起 */
        else                         open = (cur_y > PANEL_CLOSED_Y + NOTIF_BAR_HEIGHT / 2); /* 超过一半 -> 展开 */

        notif_animate_to(open ? PANEL_OPEN_Y : PANEL_CLOSED_Y);
        break;
    }
    default:
        break;
    }
}

/* 给对象挂载通知栏拖拽事件（状态栏、通知栏共用） */
static void notif_attach_events(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, notif_panel_event_cb, LV_EVENT_RELEASED, NULL);
}

/* ------------------------------------------------------------------ */
/* 亮度 / 音量应用                                                     */
/* ------------------------------------------------------------------ */

/* 应用亮度：写配置（持久化 + 通知其它 UI 同步），再写硬件。
   ⚠ 事件回调运行在 LVGL 任务上下文（已持锁），display_set_brightness 安全。
   亮度钳制到 BRIGHT_SLIDE_MIN 下限（防止屏幕完全看不清），
   未跨过阈值时直接跳过，避免频繁写 NVS / SPI。 */
static void notif_apply_brightness(uint8_t brightness)
{
    if (brightness < BRIGHT_SLIDE_MIN) {
        brightness = BRIGHT_SLIDE_MIN;
    }
    if (brightness == config_store_get_brightness()) {
        return;
    }
    config_store_set_brightness(brightness);
    display_set_brightness(brightness);
}

/* 应用音量：先写硬件（config_store 变更回调会读 audio 值，顺序不能反），
   再持久化 + 通知 UI 同步。睡眠定时音量渐降期间手动调节 → 用户接管，
   静默取消渐降（保持新音量不再自动降低）。
   档位未变时直接跳过（滑动未跨格时避免频繁写 I2C / NVS）。 */
static void notif_apply_volume(uint8_t level)
{
    if (level == audio_get_volume_level()) {
        return;
    }
    sleep_timer_fade_cancel();
    audio_set_volume_level(level);          /* 先写硬件（回调读到的即新值） */
    config_store_set_volume(level);         /* 后持久化 + 通知回调（持锁安全） */
}

/* ------------------------------------------------------------------ */
/* 显示刷新                                                             */
/* ------------------------------------------------------------------ */

/* 刷新音量相关显示：顶部状态区左音量 + 音量条填充条高度（顶部状态区
   左上方已有音量数字，竖条内不再显示数字） */
static void notif_refresh_volume_ui(uint8_t vol)
{
    if (header_vol_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), NOTIF_VOL_ICON " %u", (unsigned)vol);
        lv_label_set_text(header_vol_label, buf);
    }
    if (vol_fill != NULL) {
        int32_t h = TRACK_H * vol / AUDIO_VOL_LEVEL_MAX;
        lv_obj_set_height(vol_fill, h);
    }
}

/* 刷新亮度显示：亮度条填充条高度（亮度无需数字） */
static void notif_refresh_brightness_ui(uint8_t brightness)
{
    if (bright_fill != NULL) {
        int32_t h = TRACK_H * brightness / 255;
        lv_obj_set_height(bright_fill, h);
    }
}

/* 配置变更回调：其它入口（按键调音量、设置页改亮度等）修改配置后，
   同步本面板顶部状态区与亮度/音量竖条显示 */
static void notif_config_changed_cb(void *ctx)
{
    (void)ctx;
    notif_refresh_volume_ui(config_store_get_volume());
    notif_refresh_brightness_ui(config_store_get_brightness());
}

/* 电量图标按百分比分级（对应 LV_SYMBOL_BATTERY_* 五档，与主状态栏一致） */
static const char *header_battery_icon(float pct)
{
    if (pct >= 90.0f) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60.0f) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40.0f) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15.0f) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* 周期刷新顶部状态区电量（约 1s，与主状态栏同一电压源） */
static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (header_bat_label == NULL) return;

    float vbat_mv = battery_read_voltage_mv();
    if (vbat_mv < 0) {
        lv_label_set_text(header_bat_label, LV_SYMBOL_BATTERY_EMPTY " --.-- V");
        return;
    }
    float pct = (vbat_mv - BAT_EMPTY_MV) / (float)(BAT_FULL_MV - BAT_EMPTY_MV) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.2f V",
             header_battery_icon(pct), vbat_mv / 1000.0f);
    lv_label_set_text(header_bat_label, buf);
}

/* ------------------------------------------------------------------ */
/* 竖条滑动交互（亮度 / 音量，iOS 控制中心风格）                        */
/* ------------------------------------------------------------------ */

/* 亮度竖条滑动：上滑变亮、下滑变暗，每滑动 5px 调整 1%（0~255 无上限限制，
   可直接滑到最大；下限钳制到 BRIGHT_SLIDE_MIN，防止屏幕完全看不清）。
   ⚠ 逻辑说明：
     - 按下时按当前亮度预算 dy 的下限 dy_min（降到 BRIGHT_SLIDE_MIN 所需
       位移，负值）。⚠ 计算必须全用 (int32_t) 强转：CONFIG_STORE_BRIGHTNESS_MIN
       定义为 16u（无符号），与有符号 start_val 混算会被整型提升转成无符号，
       产生巨大正数导致 dy_min 错乱、滑动时 val 溢出（曾出现"按下即拉最低 /
       上滑时 16↔255 闪烁"）；强转后为有符号运算，dy_min 正确。
     - 下滑时 dy 触及 dy_min → 钳住 dy，并把起点锚点 bright_drag_start_y
       跟随手指下移（保持 dy 恰在下限），亮度停在下限；此时往回（上）滑
       dy 立即脱离钳制、亮度马上响应，不会出现"要滑回一截才有效果"的粘滞感。
     - 上滑方向不做钳制：dy 最大 = 按下 y ≤ 屏幕高（460px），dy*255/500
       ≤ 234，val = start_val + dy*255/500 在 int32 内不会溢出，由
       val > 255 → 255 兜底。 */
static void bright_slide_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    switch (code) {
    case LV_EVENT_PRESSED:
        bright_dragging = true;
        bright_drag_start_y = point.y;
        bright_start_val = (int32_t)config_store_get_brightness();
        /* 按当前亮度预算 dy 下限（降到 BRIGHT_SLIDE_MIN 所需位移，负值）。
           ⚠ 全用 (int32_t) 强转：CONFIG_STORE_BRIGHTNESS_MIN 为 16u（无符号），
           与有符号 start_val 混算会被整型提升为无符号，dy_min 错乱曾引发
           val 溢出/闪烁（见函数头注释） */
        bright_dy_min = (int32_t)(((int32_t)BRIGHT_SLIDE_MIN - (int32_t)bright_start_val) * (int32_t)5 * (int32_t)100 / (int32_t)255);

        break;
    case LV_EVENT_PRESSING:
        if (!bright_dragging) break;
        {
            /* 上滑（y 减小）dy > 0 → 变亮 */
            int32_t dy = bright_drag_start_y - (int32_t)point.y;
            if (dy < bright_dy_min) {
                /* 滑到亮度下限：钳住 dy，锚点跟随手指下移，亮度停在下限 */
                dy = bright_dy_min;
                bright_drag_start_y = (int32_t)point.y + bright_dy_min;
            } 

            int32_t val = bright_start_val + dy * 255 / (BRIGHT_PX_PER_PCT * 100);
            if (val < BRIGHT_SLIDE_MIN) val = BRIGHT_SLIDE_MIN;
            if (val > 255) val = 255;
            notif_apply_brightness((uint8_t)val);
        }
        break;
    case LV_EVENT_RELEASED:
        bright_dragging = false;
        break;
    default:
        break;
    }
}

/* 音量竖条滑动：上滑增音量、下滑减音量，每滑动 20px 调整 1 档。
   ⚠ 上滑（增大方向）限制在 VOL_SLIDE_MAX_LEVEL：起点档位已 ≥ 上限时
   上滑不再增加、只有下滑能降下来（防止误操作损伤耳朵）；25~30 档仅按键可调。 */
static void vol_slide_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    switch (code) {
    case LV_EVENT_PRESSED:
        vol_dragging = true;
        vol_drag_start_y = point.y;
        vol_start_level = (int32_t)audio_get_volume_level();
        break;
    case LV_EVENT_PRESSING:
        if (!vol_dragging) break;
        {
            /* 上滑（y 减小）dy > 0 → 档位增加 */
            int32_t dy = vol_drag_start_y - (int32_t)point.y;
            int32_t level = vol_start_level + dy / VOL_PX_PER_LEVEL;
            if (level > vol_start_level) {
                /* 增大方向钳制：起点已 ≥ 上限则保持起点（不再增加），
                   否则最多到上限 */
                int32_t cap = (vol_start_level >= VOL_SLIDE_MAX_LEVEL)
                                  ? vol_start_level
                                  : VOL_SLIDE_MAX_LEVEL;
                if (level > cap) level = cap;
            }
            if (level < 0) level = 0;
            if (level > AUDIO_VOL_LEVEL_MAX) level = AUDIO_VOL_LEVEL_MAX;
            notif_apply_volume((uint8_t)level);
        }
        break;
    case LV_EVENT_RELEASED:
        vol_dragging = false;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 控件构建                                                             */
/* ------------------------------------------------------------------ */

/* 事件穿透：该对象永远不参与命中测试，触摸事件穿透到下层控件 */
static void pass_through_hit_test_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    if (info != NULL) {
        info->res = false;
    }
}

/* 创建 iOS 控制中心风格的竖条：深灰圆角容器，内部为浅灰填充条（左/右/底
   三边紧贴容器边，从底部向上生长）与底部彩色图标（叠在填充条上方）。
   容器开启圆角裁剪（clip_corner）：填充条的小圆角超出容器圆角边界时被裁掉，
   呈现"填充条贴合轨道圆角"的 iOS 效果。
   @param parent     父容器
   @param[out] fill_out  返回填充条句柄（高度=比例，由调用方刷新）
   @param icon       底部图标文字（LV_SYMBOL_*）
   @param icon_color 底部图标颜色
   @return 深灰容器句柄（其上挂滑动事件） */
static lv_obj_t *vertical_bar_create(lv_obj_t *parent, lv_obj_t **fill_out,
                                     const char *icon, lv_color_t icon_color)
{
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_set_size(track, TRACK_W, TRACK_H);
    lv_obj_set_style_bg_color(track, TRACK_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, TRACK_RADIUS, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(track, LV_SCROLLBAR_MODE_OFF);
    /* 子对象超出容器圆角边界部分被裁剪（填充条上下边缘贴合轨道圆角） */
    lv_obj_set_style_clip_corner(track, true, 0);

    /* 浅灰填充条：先创建（在下层），左/右/底三边紧贴容器，高度由调用方设置，
       小圆角，超出容器圆角的部分被 clip_corner 裁掉 */
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_set_width(fill, TRACK_W);          /* 左右贴容器边 */
    lv_obj_set_height(fill, 0);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);  /* 底边贴容器边 */
    lv_obj_set_style_bg_color(fill, TRACK_FILL_COLOR, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, TRACK_FILL_RADIUS, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(fill, LV_SCROLLBAR_MODE_OFF);

    /* 底部彩色图标（加大号，后创建叠加在填充条上方，容器内底部居中） */
    lv_obj_t *icon_lbl = lv_label_create(track);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_lbl, icon_color, 0);
    lv_obj_align(icon_lbl, LV_ALIGN_BOTTOM_MID, 0, -TRACK_ICON_PAD_BOTTOM);

    if (fill_out != NULL) {
        *fill_out = fill;
    }
    return track;
}

/* 圆形占位按钮：默认深灰底（与竖条容器同色），按下时黄色高亮。
   功能暂未定义（仅外观占位，图标后续按功能替换）。 */
static void round_btn_create(lv_obj_t *parent, const char *icon)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, ROUND_BTN_DIAM, ROUND_BTN_DIAM);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, TRACK_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, NOTIF_PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_obj_center(icon_lbl);
}

/* 创建三列之一的容器：flex 纵向排列、水平/垂直居中。
   容器本身不拦截事件（clear CLICKABLE + 穿透），由内部按钮/竖条作为交互目标，
   空白处触摸穿透到 notif_bar 仍可拖拽面板。 */
static lv_obj_t *content_col_create(lv_obj_t *content)
{
    lv_obj_t *col = lv_obj_create(content);
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(col, pass_through_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
    return col;
}

/* 顶部状态信息区：46px 高，左=音量（图标+档位）、右=电量（图标+电压），
   与主状态栏（app.c）数据同步（音量走 config_store 回调、电量走周期刷新）。
   不含主状态栏中央的当前页面名称。整个区域事件穿透，不干扰面板拖拽。 */
static void header_create(void)
{
    lv_obj_t *header = lv_obj_create(notif_bar);
    lv_obj_set_size(header, EXAMPLE_LCD_H_RES, NOTIF_HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(header, pass_through_hit_test_cb, LV_EVENT_HIT_TEST, NULL);

    /* 左：音量（固定图标 + 档位数字） */
    header_vol_label = lv_label_create(header);
    lv_label_set_text(header_vol_label, NOTIF_VOL_ICON " --");
    lv_obj_set_style_text_font(header_vol_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header_vol_label, lv_color_white(), 0);
    lv_obj_align(header_vol_label, LV_ALIGN_LEFT_MID, 12, 0);

    /* 右：电量（电池图标 + 电压） */
    header_bat_label = lv_label_create(header);
    lv_label_set_text(header_bat_label, LV_SYMBOL_BATTERY_FULL " --.-- V");
    lv_obj_set_style_text_font(header_bat_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header_bat_label, lv_color_white(), 0);
    lv_obj_align(header_bat_label, LV_ALIGN_RIGHT_MID, -12, 0);
}

/* 中间内容区：左中右三列（flex row，等宽均分）。
   - 左列：两个圆形占位按钮（深灰底，功能待定）；
   - 中列：亮度竖条（深灰容器 + 贴边浅灰填充条 + 橙色图标叠于条上，
     0~255 无上限，每滑 5px 调 1%）；
   - 右列：音量竖条（同样式，蓝色喇叭图标，每滑 20px 调 1 档，滑动上限 24 档）。 */
static void content_create(void)
{
    lv_obj_t *content = lv_obj_create(notif_bar);
    lv_obj_set_size(content, EXAMPLE_LCD_H_RES,
                    EXAMPLE_LCD_V_RES - NOTIF_HEADER_H - NOTIF_TRIGGER_H);
    lv_obj_set_pos(content, 0, NOTIF_HEADER_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, CONTENT_PAD, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(content, pass_through_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(content, COL_GAP, 0);

    /* ---- 左列：两个圆形占位按钮（上下各一，功能暂时保持不动） ---- */
    lv_obj_t *left_col = content_col_create(content);
    lv_obj_set_style_pad_row(left_col, ROUND_BTN_GAP, 0);
    round_btn_create(left_col, LV_SYMBOL_WIFI);
    round_btn_create(left_col, LV_SYMBOL_BLUETOOTH);

    /* ---- 中列：亮度竖条（贴边浅灰填充条 + 橙色图标叠于条上） ---- */
    lv_obj_t *mid_col = content_col_create(content);
    lv_obj_t *bright_track = vertical_bar_create(mid_col, &bright_fill,
                                                 LV_SYMBOL_SETTINGS,
                                                 TRACK_ICON_BRIGHT);
    lv_obj_add_event_cb(bright_track, bright_slide_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bright_track, bright_slide_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bright_track, bright_slide_event_cb, LV_EVENT_RELEASED, NULL);

    /* ---- 右列：音量竖条（贴边浅灰填充条 + 蓝色喇叭图标叠于条上） ---- */
    lv_obj_t *right_col = content_col_create(content);
    lv_obj_t *vol_track = vertical_bar_create(right_col, &vol_fill,
                                              NOTIF_VOL_ICON,
                                              TRACK_ICON_VOL);
    lv_obj_add_event_cb(vol_track, vol_slide_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(vol_track, vol_slide_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(vol_track, vol_slide_event_cb, LV_EVENT_RELEASED, NULL);
}

void notification_panel_create(lv_obj_t *status_bar)
{
    lv_obj_t *scr = lv_scr_act();

    /* 下拉通知栏（全屏，最后创建=绘制在状态栏之上，默认隐藏在屏幕上方） */
    notif_bar = lv_obj_create(scr);
    lv_obj_set_size(notif_bar, EXAMPLE_LCD_H_RES, NOTIF_BAR_HEIGHT);
    lv_obj_set_pos(notif_bar, 0, PANEL_CLOSED_Y);
    lv_obj_set_style_bg_color(notif_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(notif_bar, 0, 0);
    lv_obj_set_style_radius(notif_bar, 0, 0);
    /* 内容区铺满全屏：清除默认 padding，禁止滚动与滚动条，避免子对象宽度超出出现左右滚动条 */
    lv_obj_set_style_pad_all(notif_bar, 0, 0);
    lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(notif_bar, LV_SCROLLBAR_MODE_OFF);

    /* 顶部状态信息区（46px：左音量 / 右电量） */
    header_create();

    /* 中间控制中心内容（左中右三列） */
    content_create();

    /* 底部上拉把手：全宽、与顶部触发区同高，接收上拉收起手势。
       作为 notif_bar 的子对象，随面板一起移动/隐藏。
       宽度自适应父容器内容区，不参与命中测试，只作为手势区与视觉提示。 */
    lv_obj_t *grab_bar = lv_obj_create(notif_bar);
    lv_obj_set_width(grab_bar, lv_pct(100));
    lv_obj_set_height(grab_bar, NOTIF_TRIGGER_H);
    lv_obj_align(grab_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(grab_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(grab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(grab_bar, 1, 0);
    lv_obj_set_style_border_side(grab_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(grab_bar, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(grab_bar, 0, 0);
    lv_obj_clear_flag(grab_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grab_bar, pass_through_hit_test_cb, LV_EVENT_HIT_TEST, NULL);

    lv_obj_t *grab_label = lv_label_create(grab_bar);
    lv_label_set_text(grab_label, LV_SYMBOL_UP " Swipe up to close");
    lv_obj_set_style_text_font(grab_label, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(grab_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_center(grab_label);

    /* 初始填充：顶部状态区（音量/电量）与亮度/音量竖条 */
    notif_refresh_volume_ui(config_store_get_volume());
    notif_refresh_brightness_ui(config_store_get_brightness());
    battery_timer_cb(NULL);

    /* 订阅配置变更：按键调音量、设置页改亮度等修改后同步本面板显示 */
    config_store_register_cb(notif_config_changed_cb, NULL);

    /* 电池周期刷新（约 1s） */
    lv_timer_create(battery_timer_cb, 1000, NULL);

    /* 触摸事件：状态栏（面板收起时下拉）与通知栏（展开时上拉/下拉）共用拖拽事件。
       亮度/音量竖条、圆形按钮等可点击子控件会消费自身事件（不冒泡），
       不会触发面板拖拽。 */
    notif_attach_events(status_bar);
    notif_attach_events(notif_bar);

    ESP_LOGI(TAG, "Notification panel created (control center: header + 3 columns)");
}
