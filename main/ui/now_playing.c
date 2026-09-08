#include "ui/now_playing.h"

#include <stdio.h>
#include <string.h>

#include "core/fonts/system_font.h"
#include "core/fonts/system_smaller_font.h"
#include "core/hardware/display.h"
#include "core/image/cover_jpeg.h"
#include "core/image/default_cover.h"
#include "core/music/song_meta.h"
#include "core/player/player_controller.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/power_service.h"
#include "core/utils/sleep_timer.h"
#include "core/utils/task_psram.h"
#include "ui/app.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/playlist_page.h"
#include "ui/player_ticker.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* 页面数据刷新周期由共享 player_ticker 统一提供（见 player_ticker.h） */

/* 封面图渐显时长（ms）：滑动时立即隐藏，停在播放页时渐变显示 */
#define NP_COVER_FADE_MS  300

/* 播放页底色：深灰色（非纯黑），封面图渐隐时露出，与其它页面形成层次 */
#define NP_PAGE_BG_COLOR  0x1e1e1e

/* 标题栏/控制栏半透明黑背景透明度（75% ≈ 191/255）：封面隐约透出。
   LVGL 预定义透明度无 75% 档（70/80），直接用数值 */
#define NP_BARS_OPA       ((lv_opa_t)191)

/* 状态 */
static lv_obj_t *s_page         = NULL;  /* tab 页容器 */
static lv_obj_t *s_cover        = NULL;  /* 全屏封面图（本页子对象，滚动时隐藏、停止后渐显） */
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
#if NP_HIDE_COVER_ON_SCROLL
static bool     s_cover_visible = false;  /* 封面图当前可见状态（避免重复触发渐变） */
#endif
static bool     s_bars_opaque   = false;  /* 栏当前是否处于不透明纯黑状态（滚动中） */
static bool     s_scroll_anim_running = false; /* 松手后的吸附/惯性滚动动画是否在进行中 */

/* ------------------------------------------------------------------ */
/* 封面加载（后台任务，歌曲变化时异步读取 .npd + JPEG 解码）              */
/* ------------------------------------------------------------------ */

/* ⚠ 栈 6KB：解码调用链较深（tjpgd JPEG 解码，460×460 帧缓冲落 PSRAM）。
   实测栈高水位峰值约 3.8KB（mem_monitor 每 20s 打印的 stk 行），6KB 留约
   2.2KB 余量；若未来解码库/封面尺寸变化导致余量吃紧，可回调到 8KB。 */
#define COVER_TASK_STACK (6 * 1024)
#define COVER_TASK_PRIO  1      /* 低优先级：不抢音频解码（mp3 播放任务 8/9） */
#define COVER_TASK_CORE  1      /* 与音频同核但优先级低，空闲时才解码 */

static lv_img_dsc_t s_cover_img   = { 0 }; /* 当前显示的封面图（data 为分配内存） */
static bool         s_cover_owned = false; /* 当前封面是否由本模块分配（需释放） */
static volatile uint32_t s_pending_global = 0xFFFFFFFFu; /* 待加载封面的歌曲下标 */
static TaskHandle_t s_cover_task = NULL;   /* 封面加载任务句柄 */

/* 请求加载指定歌曲的封面（LVGL 任务上下文调用；仅置标志 + 通知，不阻塞） */
static void cover_load_request(uint32_t global)
{
    s_pending_global = global;
    if (s_cover_task != NULL) {
        xTaskNotifyGive(s_cover_task);
    }
}

/* 无封面/加载失败：切回默认封面（持锁，可在任务上下文调用） */
static void cover_show_default(void)
{
    lvgl_port_lock(-1);
    if (s_cover_owned) {
        cover_jpeg_free(&s_cover_img);
        s_cover_owned = false;
    }
    if (s_cover != NULL) {
        lv_img_set_src(s_cover, DEFAULT_COVER_IMG);
    }
    lvgl_port_unlock();
}

/* 封面加载任务：等待通知 → 读取 .npd → JPEG 解码 → 持锁更新封面。
   路径在任务开头快照（player_controller 的路径缓冲会被后续切歌覆盖）；
   解码完成后校验"当前歌曲未变"才应用，避免快速切歌显示错封面。
   ⚠ Bug 20 修复：本任务自带完整的 .npd 解析结果（song_meta_t 含标题/
   副标题），在换封面的同一时机把标题/副标题标签纠正为元数据——UI 侧
   500ms 轮询（np_refresh_cb）切歌瞬间可能恰在 player_controller 读 .npd
   的阻塞窗口内，读到"新歌 + 空元数据"快照并把标题一次性写成 fallback
   （文件名/目录名）且之后不再刷新；封面由本任务独立加载不受该窗口影响，
   因此"封面解码完成"正好是"元数据必已就绪"的可靠时机，借此把标题收回
   正确值（无该字段时回退文件名/目录名）。 */
static void cover_load_task(void *arg)
{
    (void)arg;
    char path[512];
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t target = s_pending_global;

        const char *cur = player_controller_get_current_path();
        if (cur == NULL || cur[0] == '\0') {
            continue;
        }
        snprintf(path, sizeof(path), "%s", cur);

        song_meta_t meta;
        if (song_meta_load(path, &meta) != ESP_OK) {
            song_meta_free(&meta);
            cover_show_default();   /* 无 .npd：标题本就回退文件名，无需补 */
            continue;
        }

        /* 解码前先校验歌曲未变（快速切歌时省掉无谓的解码） */
        player_controller_status_t st;
        player_controller_get_status(&st);
        if (st.cur_global != target) {
            song_meta_free(&meta);
            continue;
        }

        /* 解码封面（.npd 无封面数据则跳过，走默认封面） */
        lv_img_dsc_t img = { 0 };
        bool have_cover = false;
        if (meta.jpeg_data != NULL && meta.jpeg_len > 0 &&
            cover_jpeg_decode(meta.jpeg_data, meta.jpeg_len, &img) == ESP_OK) {
            have_cover = true;
        }

        /* 解码完成：再确认一次还是这首歌才应用（解码 100~300ms 内可能又切歌） */
        player_controller_get_status(&st);
        if (st.cur_global != target) {
            if (have_cover) {
                cover_jpeg_free(&img);
            }
            song_meta_free(&meta);
            continue;
        }

        /* 标题/副标题：meta 优先，无字段回退文件名/目录名（st 已校验为新歌） */
        const char *title = (meta.title != NULL && meta.title[0] != '\0')
                            ? meta.title : st.song_name;
        const char *subtitle = (meta.subtitle != NULL && meta.subtitle[0] != '\0')
                               ? meta.subtitle : st.dir_name;

        lvgl_port_lock(-1);
        /* 换封面：成功用解码图（含释放上一首封面），无封面/解码失败回默认 */
        if (have_cover) {
            if (s_cover_owned) {
                cover_jpeg_free(&s_cover_img);
            }
            s_cover_img = img;
            s_cover_owned = true;
        } else {
            if (s_cover_owned) {
                cover_jpeg_free(&s_cover_img);
                s_cover_owned = false;
            }
        }
        if (s_cover != NULL) {
            if (have_cover) {
                lv_img_set_src(s_cover, &s_cover_img);
            } else {
                lv_img_set_src(s_cover, DEFAULT_COVER_IMG);
            }
        }
        /* Bug 20：换封面同一时机补写标题/副标题（见函数头注释） */
        if (s_title_label != NULL) {
            lv_label_set_text(s_title_label, title);
        }
        if (s_sub_label != NULL) {
            lv_label_set_text(s_sub_label, subtitle);
        }
        lvgl_port_unlock();

        song_meta_free(&meta);
    }
}

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

/* 播放页挂机自动隐藏标题栏/控制栏（秒）：用户无操作（按键/触摸）超过该
   时长，自动执行"点击封面隐藏"的同样效果。复用 power_service 的空闲计时
   （与自动息屏同一计时源），由本页自己的 lv_timer 每 1s 检查一次；
   LVGL 挂起（息屏）时 timer 不运行，恢复后继续，无副作用。 */
#define NP_AUTO_HIDE_IDLE_S  10

/* 挂机自动隐藏检查（LVGL 任务上下文，每 1s）：
   - 仅正在播放页（tab 2）且标题栏/控制栏当前可见时执行；
   - 拖动进度条中不算"挂机"（用户在操作），跳过；
   - 空闲达到 NP_AUTO_HIDE_IDLE_S 秒 → 隐藏（与点击封面效果一致）。 */
static void np_auto_hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui_visible) {
        return;                    /* 已隐藏（用户点击隐藏过），不重复处理 */
    }
    if (s_slider_dragging) {
        return;                    /* 正在拖进度条：不算挂机 */
    }
    if (main_ui_get_current_tab() != 2) {
        return;                    /* 不在正在播放页 */
    }
    if (power_service_get_idle_sec() >= NP_AUTO_HIDE_IDLE_S) {
        ESP_LOGI(TAG, "idle %d s on now playing, auto hide bars",
                 NP_AUTO_HIDE_IDLE_S);
        np_set_ui_visible(false);
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

/* 点击标题栏：播放列表更新为正在播放的目录，并带动画跳转到播放列表页 */
static void title_bar_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "title bar clicked, go to playlist");
    playlist_page_show_current();
    main_ui_switch_tab_anim(1);
}

/* 控制按钮 */
static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_prev();
    sleep_timer_note_activity();   /* 切歌属于"用户动作"：清醒感知加时 */
}

static void play_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_toggle();
    sleep_timer_note_activity();   /* 播放/暂停属于"用户动作"：清醒感知加时 */
}

static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    player_controller_next();
    sleep_timer_note_activity();   /* 切歌属于"用户动作"：清醒感知加时 */
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
        sleep_timer_note_activity();   /* seek 属于"用户动作"：清醒感知加时 */
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

/* 周期刷新：歌曲/时长/进度/播放状态（由共享 player_ticker 周期调用，
   进度/时长属连续变化每 tick 直接刷新；标题等离散变化对比旧值再刷） */
static void np_refresh_cb(const player_controller_status_t *st, void *ctx)
{
    (void)ctx;

    /* 歌曲变化时更新标题/副标题（跑马灯随 set_text 重新开始），
       并异步加载新歌曲的封面（后台任务读 .npd + JPEG 解码） */
    if (st->cur_global != s_last_global) {
        s_last_global = st->cur_global;
        lv_label_set_text(s_title_label, st->title);
        lv_label_set_text(s_sub_label, st->subtitle);
        ESP_LOGI(TAG, "now playing: %s [%s]", st->title, st->subtitle);
        cover_load_request(st->cur_global);
    }

    /* 时长 */
    char buf[16];
    np_format_time(buf, sizeof(buf), st->position_s);
    lv_label_set_text(s_cur_time, buf);
    np_format_time(buf, sizeof(buf), st->duration_s);
    lv_label_set_text(s_total_time, buf);

    /* 进度条（非拖动时跟随播放位置） */
    if (!s_slider_dragging) {
        if (st->duration_s > 0) {
            lv_slider_set_range(s_slider, 0, (int32_t)st->duration_s);
            lv_slider_set_value(s_slider, (int32_t)st->position_s, LV_ANIM_OFF);
        } else {
            lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
        }
    }

    /* 播放/暂停图标 */
    const char *icon = (st->playing == MP3_STATE_PLAYING)
                       ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    if (strcmp(lv_label_get_text(s_play_icon), icon) != 0) {
        lv_label_set_text(s_play_icon, icon);
    }
}

/* ------------------------------------------------------------------ */
/* 封面图隐藏/渐显 + 栏背景不透明度（滚动时省带宽优化）                   */
/* ------------------------------------------------------------------ */

#if NP_HIDE_COVER_ON_SCROLL
/* 封面图透明度动画回调（仅隐藏封面模式使用） */
static void cover_opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}
#endif

/* 栏（标题栏/控制栏）背景不透明度动画回调：var = 栏对象 */
static void bar_bg_opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/* 设置标题栏/控制栏背景不透明度：
   - animated=true：渐变到目标值（滚动停止后恢复半透明）；
   - animated=false：立即设置（滚动开始时直接不透明纯黑，
     滚动期间栏不参与 alpha 混合，减少渲染开销）。 */
static void np_set_bars_bg_opa(lv_opa_t target, bool animated)
{
    lv_obj_t *bars[] = { s_title_bar, s_control_bar };
    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        lv_obj_t *bar = bars[i];
        if (bar == NULL) {
            continue;
        }
        if (!animated) {
            lv_anim_del(bar, bar_bg_opa_anim_cb);
            lv_obj_set_style_bg_opa(bar, target, 0);
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        lv_anim_set_exec_cb(&a, bar_bg_opa_anim_cb);
        lv_anim_set_values(&a, (int32_t)lv_obj_get_style_bg_opa(bar, 0), target);
        lv_anim_set_time(&a, NP_COVER_FADE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/* 滚动开始：标题栏/控制栏/状态栏立即不透明纯黑（滚动期间不做 alpha 混合，
   省带宽；封面按 NP_HIDE_COVER_ON_SCROLL 决定是否同时立即隐藏）。
   s_bars_opaque 防抖：SCROLL_BEGIN 可能多次到达，只在状态变化时执行。 */
static void np_on_scroll_begin(void)
{
    if (!s_bars_opaque) {
        s_bars_opaque = true;
        np_set_bars_bg_opa(LV_OPA_COVER, false);
        app_status_set_bg_opa(LV_OPA_COVER, false);
    }
#if NP_HIDE_COVER_ON_SCROLL
    if (s_cover_visible) {
        /* 封面立即隐藏（无动画） */
        s_cover_visible = false;
        lv_anim_del(s_cover, cover_opa_anim_cb);
        lv_obj_set_style_opa(s_cover, LV_OPA_TRANSP, 0);
    }
#endif
}

/* 滚动完全停止且停在播放页：封面渐显（可选）+ 栏渐变回半透明
   （栏被用户隐藏时则渐变回全透明）。同样带防抖。 */
static void np_on_scroll_end(void)
{
#if NP_HIDE_COVER_ON_SCROLL
    if (!s_cover_visible) {
        /* 封面 0.3s 渐变显示 */
        s_cover_visible = true;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_cover);
        lv_anim_set_exec_cb(&a, cover_opa_anim_cb);
        lv_anim_set_values(&a, (int32_t)lv_obj_get_style_opa(s_cover, 0),
                           LV_OPA_COVER);
        lv_anim_set_time(&a, NP_COVER_FADE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
#endif
    if (s_bars_opaque) {
        s_bars_opaque = false;
        lv_opa_t bar_opa = s_ui_visible ? NP_BARS_OPA : LV_OPA_TRANSP;
        np_set_bars_bg_opa(bar_opa, true);
        app_status_set_bg_opa(bar_opa, true);
    }
}

/* tabview 内容容器滚动事件：
   - SCROLL_BEGIN：开始滑动 → 栏立即不透明纯黑（NP_HIDE_COVER_ON_SCROLL=1 时
     封面同时立即隐藏）；
   - SCROLL_END：滚动完全停止且停在正在播放页（tab 2）→ 栏渐变回半透明
     （NP_HIDE_COVER_ON_SCROLL=1 时封面同步渐显）。
   ⚠ 关键：松手时 lv_tabview 内部会先发出一次 SCROLL_END（事件参数=indev，
   此时吸附/惯性滚动动画才刚开始），随后动画真正完成时再发一次
   （事件参数=NULL）。因此：
   - SCROLL_BEGIN 参数非 NULL 表示滚动动画开始（置 s_scroll_anim_running）；
   - SCROLL_END 参数为 NULL 才代表滚动动画完成；
   - 仅在 s_scroll_anim_running==false 时执行恢复，保证渐变发生在滚动完全
     停止后，不在惯性滚动动画中开始（否则动画卡顿）；
   - 与 lv_tabview 内部一致地忽略"手指仍按下"的收尾 SCROLL_END。 */
static void np_scroll_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *cont = lv_event_get_target(e);

    if (code == LV_EVENT_SCROLL_BEGIN) {
        if (lv_event_get_param(e) != NULL) {
            /* 滚动动画（松手后的吸附/惯性）开始 */
            s_scroll_anim_running = true;
        }
        np_on_scroll_begin();
    } else if (code == LV_EVENT_SCROLL_END) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev != NULL && indev->proc.state == LV_INDEV_STATE_PRESSED) {
            return;   /* 手指未抬起（新一轮拖拽中旧动画的收尾），忽略 */
        }
        if (lv_event_get_param(e) == NULL) {
            /* 滚动动画完成/程序化滚动结束：滚动已真正停止 */
            s_scroll_anim_running = false;
        }
        if (!s_scroll_anim_running &&
            lv_tabview_get_tab_act(lv_obj_get_parent(cont)) == 2) {
            np_on_scroll_end();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 页面创建                                                             */
/* ------------------------------------------------------------------ */

lv_obj_t *now_playing_create(lv_obj_t *tv)
{
    s_page = lv_tabview_add_tab(tv, "");
    ui_page_style(s_page);
    /* 播放页底色：深灰色而非纯黑（封面图渐隐时露出，增加层次感） */
    lv_obj_set_style_bg_color(s_page, lv_color_hex(NP_PAGE_BG_COLOR), 0);

    /* 封面图：全屏 460x460，点击切换标题栏/控制栏显示。
       NP_HIDE_COVER_ON_SCROLL=1 时初始透明度 0（隐藏），首次停在播放页时
       渐显、滑动时立即隐藏；=0 时始终显示 */
    s_cover = lv_img_create(s_page);
    lv_img_set_src(s_cover, DEFAULT_COVER_IMG);
    lv_obj_set_size(s_cover, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_cover, 0, 0);
#if NP_HIDE_COVER_ON_SCROLL
    lv_obj_set_style_opa(s_cover, LV_OPA_TRANSP, 0);
#endif
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cover, cover_click_cb, LV_EVENT_CLICKED, NULL);

    /* 监听 tabview 内容容器滚动：滚动开始隐藏封面+栏纯黑，停在播放页恢复 */
    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_add_event_cb(cont, np_scroll_event_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(cont, np_scroll_event_cb, LV_EVENT_SCROLL_END, NULL);

    /* 顶部标题栏：25% 半透明黑，位于状态栏下方；点击跳转到播放列表 */
    s_title_bar = lv_obj_create(s_page);
    lv_obj_set_size(s_title_bar, EXAMPLE_LCD_H_RES, NP_TITLE_BAR_H);
    lv_obj_set_pos(s_title_bar, 0, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_title_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_title_bar, NP_BARS_OPA, 0);
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

    /* 底部控制栏：25% 半透明黑 */
    s_control_bar = lv_obj_create(s_page);
    lv_obj_set_size(s_control_bar, EXAMPLE_LCD_H_RES, NP_CONTROL_BAR_H);
    lv_obj_set_pos(s_control_bar, 0,
                   EXAMPLE_LCD_V_RES - NP_CONTROL_BAR_H);
    lv_obj_set_style_bg_color(s_control_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_control_bar, NP_BARS_OPA, 0);
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

    /* 立即填充一次 + 周期刷新（复用共享 player_ticker，不再自建 timer） */
    {
        player_controller_status_t st_now;
        player_controller_get_status(&st_now);
        np_refresh_cb(&st_now, NULL);
    }
    player_ticker_subscribe(np_refresh_cb, NULL);

    /* 创建封面加载任务（歌曲变化时后台读 .npd + 解码 JPEG，完成后持锁换图）。
       首次 np_refresh_cb 发生在任务创建前，当时的加载请求因句柄为空被丢弃，
       创建成功后需主动补发一次，加载当前歌曲封面。
       ⚠ 栈放 PSRAM：6KB 内部栈在开机末期内部 RAM 耗尽时创建失败（封面
       永远不更新）；本任务后台解码、无实时性要求（见 task_psram.h） */
    BaseType_t created = task_create_psram_stack(cover_load_task, "cover_load",
                                                 COVER_TASK_STACK, NULL,
                                                 COVER_TASK_PRIO, &s_cover_task,
                                                 COVER_TASK_CORE);
    if (created == pdPASS) {
        player_controller_status_t st_init;
        player_controller_get_status(&st_init);
        cover_load_request(st_init.cur_global);
    } else {
        s_cover_task = NULL;
        ESP_LOGE(TAG, "create cover load task failed");
    }

    /* 挂机自动隐藏标题栏/控制栏：每 1s 检查空闲时长（复用自动息屏的空闲
       计时），达到 NP_AUTO_HIDE_IDLE_S 秒且停在播放页时自动隐藏 */
    lv_timer_create(np_auto_hide_timer_cb, 1000, NULL);

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

/* 释放封面加载任务（仅重扫/重启前腾内部 RAM 用，见头文件说明）：
   删除任务并清空句柄。任务通常阻塞在 ulTaskNotifyTake 上，删除安全；
   封面图片缓冲（PSRAM）随重启一并回收，无需在此释放。 */
void now_playing_release_cover_task(void)
{
    if (s_cover_task == NULL) {
        return;
    }
    TaskHandle_t h = s_cover_task;
    s_cover_task = NULL;
    vTaskDelete(h);
    ESP_LOGI(TAG, "cover load task released (freeing internal RAM for rescan)");
}
