#include "ui/settings_page.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/eq.h"
#include "core/hardware/display.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/utils/sleep_timer.h"
#include "ui/eq_popup.h"
#include "ui/list_common.h"
#include "ui/toast.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "settings_page";

/* ------------------------------------------------------------------ */
/* 九宫格布局参数                                                       */
/* ------------------------------------------------------------------ */

/* 设置页整体放在一张深灰圆角卡片上（周围黑色留边）：左右/下边距 15px
   （列表页同款留白）、上边距 6px（用户查看后定终值） */
#define SETTINGS_CARD_GAP_TOP   6                  /* 与状态栏下缘的间距 */
#define SETTINGS_CARD_GAP_SIDE  UI_CARD_MARGIN_X    /* 左右/下黑边 = 15px */
#define SETTINGS_CARD_RADIUS    30                 /* 卡片圆角（与列表页一致） */

/* 卡片内网格参数：3 列等宽 + 上下留白（顶部留白收小让格子更靠上） */
#define SETTINGS_CELL_W         ((SETTINGS_CARD_W - 2 * 24 - 2 * 16) / 3)
#define SETTINGS_CARD_X         SETTINGS_CARD_GAP_SIDE
#define SETTINGS_CARD_Y         (UI_STATUS_BAR_H + SETTINGS_CARD_GAP_TOP)
#define SETTINGS_CARD_W         (EXAMPLE_LCD_H_RES - 2 * SETTINGS_CARD_GAP_SIDE)
#define SETTINGS_CARD_H         (EXAMPLE_LCD_V_RES - SETTINGS_CARD_Y \
                                 - SETTINGS_CARD_GAP_SIDE)
#define SETTINGS_CELL_H         ((SETTINGS_CARD_H - 24 - 24 - 2 * 16) / 3)
#define SETTINGS_GRID_PAD_TOP   24                 /* 顶部留白 */
#define SETTINGS_GRID_PAD_BOT   24

/* 按下（pressed）时的黄色背景：默认无边框无背景，仅按压瞬间高亮 */
#define SETTINGS_PRESS_COLOR  lv_color_hex(0xFFC000)

/* 睡眠定时档位（分钟），点击在档位间轮换 */
static const uint32_t SLEEP_STEPS[] = { 15, 30, 45, 60, 90, 120, 180, 240 };
#define SLEEP_STEP_COUNT  (sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]))

/* 睡眠剩余时间刷新周期（ms）：剩余时间显示"分:秒"，需每秒更新 */
#define SLEEP_REFRESH_MS  1000

/* 三个功能格子的图标/文字句柄（供交互与定时刷新更新） */
static lv_obj_t *s_loop_icon     = NULL;   /* 播放模式图标（随模式切换） */
static lv_obj_t *s_loop_status   = NULL;   /* 播放模式文字（当前循环方式） */
static lv_obj_t *s_shuffle_icon  = NULL;   /* 顺序/随机图标 */
static lv_obj_t *s_shuffle_status = NULL;  /* 顺序播放/随机播放 */
static lv_obj_t *s_sleep_status  = NULL;   /* 睡眠定时：OFF / 剩余 MM:SS */
static lv_obj_t *s_eq_status     = NULL;   /* EQ：当前 .eq 文件名 / 无 EQ */

/* 本页 tab 对象（EQ 弹窗挂载锚点，点击回调使用） */
static lv_obj_t *s_settings_page = NULL;

/* 上次显示的剩余秒数：文本未变时跳过刷新，避免每秒无谓重绘 */
static uint32_t s_sleep_last_s = 0;

/* 本次按压是否已发生过长按：LVGL 8.4 中长按后松开仍会发送 CLICKED
   （long_pr_sent 只抑制 SHORT_CLICKED），用该标志忽略长按后的点击，
   避免"长按切 Off 后松手又被点击逻辑切回档位"。 */
static bool s_sleep_long_pressed = false;

/* ------------------------------------------------------------------ */
/* 状态文字刷新                                                         */
/* ------------------------------------------------------------------ */

/* 播放模式：图标与文字随循环方式切换（"文字压缩到一行"） */
static void settings_refresh_loop(void)
{
    if (s_loop_icon == NULL || s_loop_status == NULL) {
        return;
    }
    switch (player_controller_get_loop_mode()) {
    case PLAYER_LOOP_ALL:
        lv_label_set_text(s_loop_icon, LV_SYMBOL_LOOP);
        lv_label_set_text(s_loop_status, "全部循环");
        break;
    case PLAYER_LOOP_SINGLE:
        lv_label_set_text(s_loop_icon, LV_SYMBOL_LOOP "1");
        lv_label_set_text(s_loop_status, "单曲循环");
        break;
    default: /* PLAYER_LOOP_LIST */
        lv_label_set_text(s_loop_icon, LV_SYMBOL_LIST);
        lv_label_set_text(s_loop_status, "列表循环");
        break;
    }
}

/* 顺序/随机播放：图标与文字随开关切换。
   顺序播放用 "AZ" 文字当图标（类似睡眠定时器的 "Zz"），随机播放用
   LV_SYMBOL_SHUFFLE 图标。 */
static void settings_refresh_shuffle(void)
{
    if (s_shuffle_icon == NULL || s_shuffle_status == NULL) {
        return;
    }
    if (config_store_get_shuffle()) {
        lv_label_set_text(s_shuffle_icon, LV_SYMBOL_SHUFFLE);
        lv_label_set_text(s_shuffle_status, "随机播放");
    } else {
        lv_label_set_text(s_shuffle_icon, "AZ");
        lv_label_set_text(s_shuffle_status, "顺序播放");
    }
}

/* 睡眠定时：未定时显示 OFF；定时中显示剩余时间"分:秒"（每秒刷新） */
static void settings_refresh_sleep(void)
{
    if (s_sleep_status == NULL) {
        return;
    }
    uint32_t remain = sleep_timer_get_remaining_s();
    if (remain == 0) {
        lv_label_set_text(s_sleep_status, "OFF");
        s_sleep_last_s = 0;
        return;
    }
    if (remain != s_sleep_last_s) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u:%02u", remain / 60, remain % 60);
        lv_label_set_text(s_sleep_status, buf);
        s_sleep_last_s = remain;
    }
}

/* EQ 状态文字：有生效 EQ 显示文件名，否则"无 EQ"（config_store 为数据源，
   选择弹窗应用后经变更回调同步到这里） */
static void settings_refresh_eq(void)
{
    if (s_eq_status == NULL) {
        return;
    }
    char name[EQ_FILE_NAME_MAX];
    if (config_store_get_eq(name, sizeof(name), NULL)) {
        lv_label_set_text(s_eq_status, name);
    } else {
        lv_label_set_text(s_eq_status, "无 EQ");
    }
}

/* 自动息屏超时（config_store 配置）：不在此页展示，后续放独立的不常用
   设置页（见 FUTURE_PLAN 第 5 点），此处仅保留配置存取。 */

/* 配置变更回调（config_store 通知）：外部入口修改配置后同步本页显示 */
static void settings_config_changed_cb(void *ctx)
{
    (void)ctx;
    settings_refresh_loop();
    settings_refresh_shuffle();
    settings_refresh_sleep();
    settings_refresh_eq();
}

/* 每秒刷新睡眠剩余时间 */
static void sleep_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    settings_refresh_sleep();
}

/* ------------------------------------------------------------------ */
/* 格子交互事件                                                        */
/* ------------------------------------------------------------------ */

/* 播放模式格子：点击在 全部循环 → 单曲循环 → 列表循环 间轮换。
   player_controller_set_loop_mode 内部会写 NVS 并通知 config_store 回调。 */
static void loop_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    player_loop_mode_t mode = player_controller_get_loop_mode();
    mode = (player_loop_mode_t)(((int)mode + 1) % 3);
    player_controller_set_loop_mode(mode);
    ESP_LOGI(TAG, "loop mode -> %d", (int)mode);
    settings_refresh_loop();
}

/* 顺序/随机播放格子：点击切换并持久化。
   经 player_controller_set_shuffle 生效：关闭→开启时会重新生成随机位置
   并定位到当前歌曲（"重新开始随机播放"），随后下一曲/上一曲走随机序列。 */
static void shuffle_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    bool on = !player_controller_get_shuffle();
    player_controller_set_shuffle(on);
    ESP_LOGI(TAG, "shuffle -> %d", (int)on);
    settings_refresh_shuffle();
}

/* 睡眠定时格子：
   - 点击：在档位（15/30/45/60/90/120/180/240 分钟）间轮换，并保存为
     默认睡眠时长（NVS）；当前关闭时从第一档（15 分钟）开始；
   - 长按：当前开启 → 关闭；当前关闭 → 切到默认睡眠时长（初始 45 分钟）。
     长按属"开关"操作，用 Toast 弹反馈（已关闭 / XX分钟后停止播放）。
   ⚠ 长按后松开 LVGL 仍会发送 CLICKED，需用 s_sleep_long_pressed 忽略。 */
static void sleep_cell_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        /* 一次新的按压：复位长按标志 */
        s_sleep_long_pressed = false;
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        s_sleep_long_pressed = true;
        if (sleep_timer_is_active()) {
            sleep_timer_set(0);                      /* 长按：关闭 */
            ESP_LOGI(TAG, "sleep timer off (long press)");
            /* Toast 反馈：关闭成功 */
            ui_toast_show("已关闭睡眠定时器");
        } else {
            uint32_t def = config_store_get_sleep_default_min();
            sleep_timer_set(def);                    /* 长按：切到默认时长 */
            ESP_LOGI(TAG, "sleep timer -> default %u min (long press)",
                     (unsigned)def);
            /* Toast 反馈：开启成功，提示几分钟后停止播放 */
            char buf[48];
            snprintf(buf, sizeof(buf), "%u分钟后停止播放", (unsigned)def);
            ui_toast_show(buf);
        }
        settings_refresh_sleep();
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        if (s_sleep_long_pressed) {
            /* 长按后的点击：忽略，防止长按切 Off 后松手又切回档位 */
            s_sleep_long_pressed = false;
            return;
        }
        uint32_t cur = sleep_timer_get_minutes();
        uint32_t next = SLEEP_STEPS[0];
        if (cur != 0) {
            /* 在档位表中找当前位置，向后轮换；不在表中（异常值）从头开始 */
            for (size_t i = 0; i < SLEEP_STEP_COUNT; i++) {
                if (SLEEP_STEPS[i] == cur) {
                    next = SLEEP_STEPS[(i + 1) % SLEEP_STEP_COUNT];
                    break;
                }
            }
        }
        sleep_timer_set(next);
        config_store_set_sleep_default_min(next);   /* 保存为默认时长 */
        ESP_LOGI(TAG, "sleep timer -> %u min", (unsigned)next);
        settings_refresh_sleep();
        return;
    }
}

/* EQ 格子：点击打开预设选择弹窗（无 EQ + 扫描 .musicfx 的 .eq 文件）。
   选择/解析/Toast 反馈均在弹窗模块内完成（见 ui/eq_popup.c） */
static void eq_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    eq_popup_toggle(s_settings_page);
}

/* ------------------------------------------------------------------ */
/* 格子构建辅助                                                        */
/* ------------------------------------------------------------------ */

/* 创建九宫格格子：默认无边框无背景，按下（pressed）时黄色背景。
   内容由调用方添加（图标 + 一行状态文字）。 */
static lv_obj_t *settings_cell_create(lv_obj_t *grid)
{
    lv_obj_t *cell = lv_obj_create(grid);
    lv_obj_set_size(cell, SETTINGS_CELL_W, SETTINGS_CELL_H);

    /* 默认透明：无背景、无边框 */
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_radius(cell, 12, 0);
    /* 按下时黄色背景 */
    lv_obj_set_style_bg_color(cell, SETTINGS_PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);

    /* 内容纵向排列居中（图标在上，文字在下） */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 8, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
    return cell;
}

/* 图标文字（LVGL 内置 Montserrat，含 PUA 区图标字形），返回句柄供切换 */
static lv_obj_t *settings_icon_label_create(lv_obj_t *cell, const char *icon)
{
    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    return lbl;
}

/* 状态文字（一行）：
   - ascii=true 用 Montserrat（OFF / 剩余 MM:SS 纯数字冒号，紧凑）；
   - ascii=false 用系统字体（中文模式名）。返回句柄供刷新。 */
static lv_obj_t *settings_status_label_create(lv_obj_t *cell, bool ascii)
{
    lv_obj_t *st = lv_label_create(cell);
    lv_obj_set_style_text_font(st, ascii ? &lv_font_montserrat_24 : UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(st, lv_color_white(), 0);
    return st;
}

/* ------------------------------------------------------------------ */
/* 页面创建                                                            */
/* ------------------------------------------------------------------ */

lv_obj_t *settings_page_create(lv_obj_t *tv)
{
    lv_obj_t *page = lv_tabview_add_tab(tv, "");
    ui_page_style(page);
    s_settings_page = page;

    /* 深灰圆角卡片（周围黑色留边）：九宫格格子全部放在卡片内。
       上边距收小（SETTINGS_CARD_GAP_TOP = 6px，与状态栏贴近） */
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_pos(card, SETTINGS_CARD_X, SETTINGS_CARD_Y);
    lv_obj_set_size(card, SETTINGS_CARD_W, SETTINGS_CARD_H);
    lv_obj_set_style_bg_color(card, UI_CARD_COLOR, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, SETTINGS_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* 九宫格容器：3 列 × 3 行 flex 换行，填满卡片：
         宽 = (430 - 24*2 - 16*2) / 3 = 116
         高 = (393 - 24 - 24 - 16*2) / 3 = 104 */
    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(grid, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(grid, 24, 0);
    lv_obj_set_style_pad_right(grid, 24, 0);
    lv_obj_set_style_pad_top(grid, SETTINGS_GRID_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(grid, SETTINGS_GRID_PAD_BOT, 0);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);

    /* 格子 1：播放模式（图标随模式切换，文字直接显示当前循环方式，一行） */
    lv_obj_t *loop_cell = settings_cell_create(grid);
    s_loop_icon = settings_icon_label_create(loop_cell, LV_SYMBOL_LOOP);
    s_loop_status = settings_status_label_create(loop_cell, false);
    settings_refresh_loop();
    lv_obj_add_event_cb(loop_cell, loop_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 2：顺序/随机播放（点击切换，图标随状态切换：
       顺序 "AZ" 文字 / 随机 LV_SYMBOL_SHUFFLE） */
    lv_obj_t *shuffle_cell = settings_cell_create(grid);
    s_shuffle_icon = settings_icon_label_create(shuffle_cell, "AZ");
    s_shuffle_status = settings_status_label_create(shuffle_cell, false);
    settings_refresh_shuffle();
    lv_obj_add_event_cb(shuffle_cell, shuffle_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 3：睡眠定时（"Zz" 图标 + OFF/剩余 MM:SS；点击轮换档位 + 保存
       默认；长按 关闭/默认时长） */
    lv_obj_t *sleep_cell = settings_cell_create(grid);
    settings_icon_label_create(sleep_cell, "Zz");
    s_sleep_status = settings_status_label_create(sleep_cell, true);
    settings_refresh_sleep();
    /* 需同时监听 PRESSED（复位长按标志）、CLICKED（轮换）、LONG_PRESSED（开关） */
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* 格子 4~6：预留空位 */
    for (int i = 0; i < 3; i++) {
        settings_cell_create(grid);
    }

    /* 格子 7（左下角）：EQ 预设选择（FUTURE_PLAN 第 10 点）。
       图标固定 "EQ" 字样；点击弹出选择列表（无 EQ / 扫描 .musicfx） */
    lv_obj_t *eq_cell = settings_cell_create(grid);
    settings_icon_label_create(eq_cell, "EQ");
    s_eq_status = settings_status_label_create(eq_cell, false);
    settings_refresh_eq();
    lv_obj_add_event_cb(eq_cell, eq_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 8~9：预留空位 */
    for (int i = 0; i < 2; i++) {
        settings_cell_create(grid);
    }

    /* 每秒刷新睡眠剩余时间（分:秒） */
    lv_timer_create(sleep_status_timer_cb, SLEEP_REFRESH_MS, NULL);

    /* 订阅配置变更：其它入口修改配置（如通知栏亮度）时同步本页显示 */
    config_store_register_cb(settings_config_changed_cb, NULL);

    ESP_LOGI(TAG, "settings page created (grid: loop/shuffle/sleep timer/EQ)");
    return page;
}
