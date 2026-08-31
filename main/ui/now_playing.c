#include "ui/now_playing.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/fonts/system_smaller_font.h"
#include "core/hardware/display.h"
#include "core/image/default_cover.h"
#include "core/player/player_controller.h"
#include "ui/app.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/playlist_page.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "now_playing";

/* 标题栏/控制栏高度（自拟） */
#define NP_TITLE_BAR_H    99   /* 标题栏：两行文字 + 内边距（副标题到底部留 16px） */
#define NP_CONTROL_BAR_H  144  /* 控制栏：按钮行 + 进度条行 */

/* 控制栏内部布局 */
#define NP_BTN_W          81   /* 上一曲/播放/下一曲按钮宽（108 的 75%） */
#define NP_BTN_H          81   /* 按钮高（正方形，与宽一致） */
#define NP_BTN_GAP        8    /* 按钮间距 */
#define NP_SLIDER_Y       109  /* 进度条顶部 y（按钮底 89 + 20px 间距） */
#define NP_SLIDER_H       32   /* 进度条控件高（触控响应区；视觉条更细） */
#define NP_SIDE_PAD       20   /* 时长文字/进度条左右边距 */

/* 第一行内容中心相对控制栏中心的 y 偏移（控制栏高 144 → 中心 72，按钮行中心 48） */
#define NP_ROW1_OFFSET    -24

/* 控制按钮图标字体 */
#define NP_ICON_FONT      (&lv_font_montserrat_28)
/* 播放按钮图标字体（稍大一号，突出播放键） */
#define NP_PLAY_ICON_FONT (&lv_font_montserrat_32)

/* 页面数据刷新周期（ms） */
#define NP_REFRESH_MS     500

/* 状态 */
static lv_obj_t *s_page         = NULL;  /* tab 页容器 */
static lv_obj_t *s_title_bar    = NULL;  /* 顶部标题栏 */
static lv_obj_t *s_control_bar  = NULL;  /* 底部控制栏 */
static lv_obj_t *s_title_label  = NULL;  /* 歌曲标题（大字体，滚动） */
static lv_obj_t *s_sub_label    = NULL;  /* 专辑/艺术家（小字体，滚动） */
static lv_obj_t *s_cur_time     = NULL;  /* 当前播放时长 */
static lv_obj_t *s_total_time   = NULL;  /* 总时长 */
static lv_obj_t *s_play_icon    = NULL;  /* 播放/暂停按钮图标 */
static lv_obj_t *s_slider       = NULL;  /* 进度条 */

static uint32_t s_last_global   = 0xFFFFFFFFu; /* 上次显示的歌曲全局下标 */
static bool     s_ui_visible    = true;   /* 标题栏/控制栏可见性 */
static bool     s_slider_dragging = false; /* 进度条是否正在拖动 */

/* ------------------------------------------------------------------ */
/* 内部工具                                                             */
/* ------------------------------------------------------------------ */

/* 创建可滚动的跑马灯 label（超长文本循环滚动；非当前页时由外部关闭） */
static lv_obj_t *np_make_marquee_label(lv_obj_t *parent, const char *text,
                                       const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

/* 创建控制按钮（图标按钮）：无阴影、无背景色，仅按下时显示颜色反馈。
   label_out 可选：返回图标 label 句柄（用于播放/暂停图标切换），不需要传 NULL */
static lv_obj_t *np_make_icon_btn(lv_obj_t *parent, const char *icon,
                                  lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);          /* 去掉默认样式（阴影/背景/边框） */
    lv_obj_set_size(btn, NP_BTN_W, NP_BTN_H);
    /* 默认透明；按下时灰色半透明反馈 */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREY), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_font(lbl, NP_ICON_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    if (label_out != NULL) {
        *label_out = lbl;
    }
    return btn;
}

/* 设置标题栏/控制栏可见性（点击封面切换）；顶部状态栏同步透明/恢复
   （状态栏保留触摸事件，通知栏下拉不受影响） */
static void np_set_ui_visible(bool visible)
{
    s_ui_visible = visible;
    if (visible) {
        lv_obj_clear_flag(s_title_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_control_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_title_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_control_bar, LV_OBJ_FLAG_HIDDEN);
    }
    app_status_set_ui_visible(visible);
}

/* 强制恢复标题栏/控制栏显示（离开正在播放页时由 main_ui 调用；
   与 np_set_ui_visible 的区别：不联动状态栏，状态栏由调用方单独恢复） */
void now_playing_force_show_ui(void)
{
    s_ui_visible = true;
    if (s_title_bar != NULL) {
        lv_obj_clear_flag(s_title_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_control_bar != NULL) {
        lv_obj_clear_flag(s_control_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ */
/* 交互回调                                                             */
/* ------------------------------------------------------------------ */

/* 点击封面：切换标题栏/控制栏显示与隐藏 */
static void cover_click_cb(lv_event_t *e)
{
    (void)e;
    np_set_ui_visible(!s_ui_visible);
}

/* 点击标题栏：播放列表更新为正在播放的目录，并跳转到播放列表页 */
static void title_bar_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "title bar clicked, go to playlist");
    playlist_page_show_current();
    main_ui_switch_tab(1);
}

/* 控制按钮 */
static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_prev();
}

static void play_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_toggle();
}

static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_next();
}

/* 进度条：按下标记拖动中（暂停自动跟随），松手时 seek 到拖动位置 */
static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_slider_dragging = true;
    } else if (code == LV_EVENT_RELEASED) {
        s_slider_dragging = false;
        player_controller_seek((uint32_t)lv_slider_get_value(slider));
        ESP_LOGI(TAG, "seek to %u s", (unsigned)lv_slider_get_value(slider));
    }
}

/* ------------------------------------------------------------------ */
/* 周期刷新                                                             */
/* ------------------------------------------------------------------ */

/* 把秒数格式化为 m:ss */
static void np_format_time(char *buf, size_t size, uint32_t sec)
{
    snprintf(buf, size, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

/* 周期刷新：歌曲/时长/进度/播放状态 */
static void np_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    player_controller_status_t st;
    player_controller_get_status(&st);

    /* 歌曲变化时更新标题/副标题（跑马灯随 set_text 重新开始） */
    if (st.cur_global != s_last_global) {
        s_last_global = st.cur_global;
        lv_label_set_text(s_title_label, st.song_name);
        lv_label_set_text(s_sub_label, st.dir_name);
        ESP_LOGI(TAG, "now playing: %s [%s]", st.song_name, st.dir_name);
    }

    /* 时长 */
    char buf[16];
    np_format_time(buf, sizeof(buf), st.position_s);
    lv_label_set_text(s_cur_time, buf);
    np_format_time(buf, sizeof(buf), st.duration_s);
    lv_label_set_text(s_total_time, buf);

    /* 进度条（非拖动时跟随播放位置） */
    if (!s_slider_dragging) {
        if (st.duration_s > 0) {
            lv_slider_set_range(s_slider, 0, (int32_t)st.duration_s);
            lv_slider_set_value(s_slider, (int32_t)st.position_s, LV_ANIM_OFF);
        } else {
            lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
        }
    }

    /* 播放/暂停图标 */
    const char *icon = (st.playing == MP3_STATE_PLAYING)
                       ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    if (strcmp(lv_label_get_text(s_play_icon), icon) != 0) {
        lv_label_set_text(s_play_icon, icon);
    }
}

/* ------------------------------------------------------------------ */
/* 页面创建                                                             */
/* ------------------------------------------------------------------ */

lv_obj_t *now_playing_create(lv_obj_t *tv)
{
    s_page = lv_tabview_add_tab(tv, "");
    ui_page_style(s_page);

    /* 封面图：全屏 460x460，点击切换标题栏/控制栏显示 */
    lv_obj_t *cover = lv_img_create(s_page);
    lv_img_set_src(cover, &default_cover);
    lv_obj_set_size(cover, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover, cover_click_cb, LV_EVENT_CLICKED, NULL);

    /* 顶部标题栏：50% 半透明黑，位于状态栏下方；点击跳转到播放列表 */
    s_title_bar = lv_obj_create(s_page);
    lv_obj_set_size(s_title_bar, EXAMPLE_LCD_H_RES, NP_TITLE_BAR_H);
    lv_obj_set_pos(s_title_bar, 0, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_title_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_title_bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_title_bar, 0, 0);
    lv_obj_set_style_radius(s_title_bar, 0, 0);   /* 无圆角 */
    lv_obj_set_style_pad_left(s_title_bar, 16, 0);
    lv_obj_set_style_pad_right(s_title_bar, 16, 0);
    lv_obj_set_style_pad_top(s_title_bar, 10, 0);
    lv_obj_set_style_pad_bottom(s_title_bar, 0, 0);
    lv_obj_clear_flag(s_title_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_title_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_title_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_title_bar, title_bar_click_cb, LV_EVENT_CLICKED, NULL);

    /* 歌曲标题：大字体（系统字体，含中文），滚动 */
    player_controller_status_t st;
    player_controller_get_status(&st);
    s_title_label = np_make_marquee_label(s_title_bar, st.song_name,
                                          UI_SYSTEM_FONT, lv_color_white());
    lv_obj_set_pos(s_title_label, 0, 0);

    /* 专辑/艺术家：普通大小字体 + 灰色（小字体 system_smaller_font 字不全，
       用户数据可能含生僻字/日文，改用与标题同族的 UI_SYSTEM_FONT），滚动 */
    s_sub_label = np_make_marquee_label(s_title_bar, st.dir_name,
                                        UI_SYSTEM_FONT,
                                        lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_pos(s_sub_label, 0, 40);

    /* 底部控制栏：50% 半透明黑 */
    s_control_bar = lv_obj_create(s_page);
    lv_obj_set_size(s_control_bar, EXAMPLE_LCD_H_RES, NP_CONTROL_BAR_H);
    lv_obj_set_pos(s_control_bar, 0,
                   EXAMPLE_LCD_V_RES - NP_CONTROL_BAR_H);
    lv_obj_set_style_bg_color(s_control_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_control_bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_control_bar, 0, 0);
    lv_obj_set_style_radius(s_control_bar, 0, 0);   /* 无圆角 */
    lv_obj_set_style_pad_all(s_control_bar, 0, 0);
    lv_obj_clear_flag(s_control_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_control_bar, LV_SCROLLBAR_MODE_OFF);

    /* 第一行：当前时长（左）+ 上一曲/播放/下一曲（居中）+ 总时长（右）。
       时长用 system_smaller_font（MiSans-Bold 20px，比内置 14px 更大更协调） */
    s_cur_time = lv_label_create(s_control_bar);
    lv_label_set_text(s_cur_time, "0:00");
    lv_obj_set_style_text_font(s_cur_time, &system_smaller_font, 0);
    lv_obj_set_style_text_color(s_cur_time, lv_color_white(), 0);
    lv_obj_align(s_cur_time, LV_ALIGN_LEFT_MID, NP_SIDE_PAD, NP_ROW1_OFFSET);

    s_total_time = lv_label_create(s_control_bar);
    lv_label_set_text(s_total_time, "0:00");
    lv_obj_set_style_text_font(s_total_time, &system_smaller_font, 0);
    lv_obj_set_style_text_color(s_total_time, lv_color_white(), 0);
    lv_obj_align(s_total_time, LV_ALIGN_RIGHT_MID, -NP_SIDE_PAD, NP_ROW1_OFFSET);

    /* 三个控制按钮：居中排列（相对控制栏水平中心）。
       播放按钮的图标 label 由 s_play_icon 持有，用于播放/暂停图标切换 */
    lv_obj_t *prev_btn = np_make_icon_btn(s_control_bar, LV_SYMBOL_PREV,
                                          prev_btn_cb, NULL);
    lv_obj_align(prev_btn, LV_ALIGN_CENTER, -NP_BTN_W - NP_BTN_GAP, NP_ROW1_OFFSET);

    lv_obj_t *play_btn = np_make_icon_btn(s_control_bar, LV_SYMBOL_PLAY,
                                          play_btn_cb, &s_play_icon);
    lv_obj_align(play_btn, LV_ALIGN_CENTER, 0, NP_ROW1_OFFSET);
    /* 播放按钮图标稍大一号（montserrat_32），突出播放键 */
    lv_obj_set_style_text_font(s_play_icon, NP_PLAY_ICON_FONT, 0);

    lv_obj_t *next_btn = np_make_icon_btn(s_control_bar, LV_SYMBOL_NEXT,
                                          next_btn_cb, NULL);
    lv_obj_align(next_btn, LV_ALIGN_CENTER, NP_BTN_W + NP_BTN_GAP, NP_ROW1_OFFSET);

    /* 第二行：进度条。视觉条做细（6px），控件高度保留作为触控响应区 */
    s_slider = lv_slider_create(s_control_bar);
    lv_obj_set_size(s_slider, EXAMPLE_LCD_H_RES - 2 * NP_SIDE_PAD, NP_SLIDER_H);
    lv_obj_set_pos(s_slider, NP_SIDE_PAD, NP_SLIDER_Y);
    /* 主轨与指示器：细条（6px）、圆角；颜色 深灰轨 + 白色进度 */
    lv_obj_set_style_height(s_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_height(s_slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    /* 滑块：小圆点 */
    lv_obj_set_style_size(s_slider, 16, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_slider, lv_color_white(), LV_PART_KNOB);
    lv_slider_set_range(s_slider, 0, 1);
    lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_RELEASED, NULL);

    /* 立即填充一次 + 周期刷新 */
    np_refresh_timer_cb(NULL);
    lv_timer_create(np_refresh_timer_cb, NP_REFRESH_MS, NULL);

    ESP_LOGI(TAG, "now playing page created");
    return s_page;
}

void now_playing_set_marquee(bool enable)
{
    lv_label_long_mode_t mode = enable
        ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
    if (s_title_label != NULL) {
        lv_label_set_long_mode(s_title_label, mode);
    }
    if (s_sub_label != NULL) {
        lv_label_set_long_mode(s_sub_label, mode);
    }
}
