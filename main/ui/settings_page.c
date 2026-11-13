#include "ui/settings_page.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/eq.h"
#include "core/hardware/display.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/utils/sleep_timer.h"
#include "ui/config_slot_popup.h"
#include "ui/eq_popup.h"
#include "ui/list_common.h"
#include "ui/player_ticker.h"
#include "ui/toast.h"

#include <math.h>
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

/* 图标区统一尺寸（px，见 settings_icon_area_create）：
   双行文字图标（回放增益的"回放\n增益"用 UI_SMALL_FONT 20px）需要放得下两行；
   其它格子是单行 Montserrat 24（行高 29px）。
   若不统一，格子内"图标 + 状态文字"的总高不同，flex 居中后各格子的
   图标/文字基线互相错开（矮格子的状态文字被往下推）。这里让所有格子的
   图标区都是 48×48，内部单行文字自行垂直居中，从而各格子完全对齐。
   ⚠ 行高随字体变化：UI_SMALL_FONT 是内嵌 20px 点阵，行高 23（两行 46 < 48，
   放得下）；TTF 时代 20px 行高 27、需要 text_line_space=-6 收紧，位图字体
   不需要那个修正（见 settings_icon_label_create）。改字体/字号时两处要一起看。
   ⚠ 若把"回放/增益"改用 UI_SYSTEM_FONT（24px NPF 行高 32），两行要 64px，
   需同步把这里加到 64。 */
#define SETTINGS_ICON_W         48
#define SETTINGS_ICON_H         48

/* 按下（pressed）时的黄色背景：默认无边框无背景，仅按压瞬间高亮 */
#define SETTINGS_PRESS_COLOR  lv_color_hex(0xFFC000)

/* 睡眠定时档位（分钟），点击在档位间轮换 */
static const uint32_t SLEEP_STEPS[] = { 15, 30, 45, 60, 90, 120, 180, 240 };
#define SLEEP_STEP_COUNT  (sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]))

/* 睡眠剩余时间刷新周期（ms）：剩余时间显示"分:秒"，需每秒更新 */
#define SLEEP_REFRESH_MS  1000

/* 功能格子的图标/文字句柄（供交互与定时刷新更新） */
static lv_obj_t *s_loop_icon     = NULL;   /* 播放模式图标（随模式切换） */
static lv_obj_t *s_loop_status   = NULL;   /* 播放模式文字（当前循环方式） */
static lv_obj_t *s_shuffle_icon  = NULL;   /* 顺序/随机图标 */
static lv_obj_t *s_shuffle_status = NULL;  /* 顺序播放/随机播放 */
static lv_obj_t *s_sleep_status  = NULL;   /* 睡眠定时：OFF / 剩余 MM:SS */
static lv_obj_t *s_eq_status     = NULL;   /* EQ：当前 .eq 文件名 / 无 EQ */
static lv_obj_t *s_rg_status     = NULL;   /* 回放增益：±x.x dB / 无增益 / 关闭 */
static lv_obj_t *s_slot_status   = NULL;   /* 配置切换：当前配置 SAVE1~SAVE4 */

/* ------------------------------------------------------------------ */
/* 回放增益开关（第 5 格）                                              */
/* ------------------------------------------------------------------ */

/* 增益数值的文字色：
   - 负增益（衰减）→ 蓝；
   - 正增益（提升）→ 浅绿；
   关闭/无增益用白色（见 settings_refresh_replay_gain）。
   ⚠ 不用本项目"选中项"惯用的黄色：格子的按下态背景就是黄色，黄色文字
   按下去会与背景融在一起看不清。 */
#define RG_NEG_COLOR   lv_palette_main(LV_PALETTE_BLUE)
#define RG_POS_COLOR   lv_color_hex(0x7DE08A)   /* 浅绿：正增益 */

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

/* 播放模式：图标与文字随循环方式切换（"文字压缩到一行"）。
   ⚠ 数据源用 config_store 而不是 player_controller_get_loop_mode()：切换配置
   槽位时循环模式由控制器任务**异步**应用（CMD_RESTORE_CONFIG），配置变更回调
   派发时控制器可能还没处理到，读控制器会显示上一个槽位的模式。config_store
   的活跃槽位值在切换那一刻就已更新，是这里的正确来源（两者平时总是一致的：
   player_controller_set_loop_mode 会同时写两处）。 */
static void settings_refresh_loop(void)
{
    if (s_loop_icon == NULL || s_loop_status == NULL) {
        return;
    }
    switch (config_store_get_loop_mode()) {
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

/* 配置切换格子状态文字：显示当前活跃配置槽位（SAVE1~SAVE4）。
   数据源是 config_store 的活跃槽位索引——config_store_switch_slot 会通知
   变更回调，本页文字随之刷新（不需要额外注册回调）。 */
static void settings_refresh_slot(void)
{
    if (s_slot_status == NULL) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "SAVE%u",
             (unsigned)(config_store_get_active_slot() + 1u));
    lv_label_set_text(s_slot_status, buf);
}

/* 自动息屏超时（config_store 配置）：不在此页展示，后续放独立的不常用
   设置页（见 FUTURE_PLAN 第 5 点），此处仅保留配置存取。 */

/* ------------------------------------------------------------------ */
/* 回放增益状态显示                                                     */
/* ------------------------------------------------------------------ */

/* 把线性回放增益换算成"分贝×10"的整数：输入是 .npd 元数据里的 16.16 定点
   线性值（65536 = 1.0 = 0dB），换算 20*log10f 后四舍五入到 0.1dB。
   项目其它模块（eq.c / click_wheel.c）本就用 <math.h>，直接用浮点最直观；
   Q16 量化误差仅 ±0.00005dB，远小于 0.1dB 显示精度，不会出现边界跳位。 */
static int32_t rg_tenths_db_from_q16(uint32_t gain_q16)
{
    float gain = (float)gain_q16 / 65536.0f;
    if (gain <= 0.0f) {
        return 0;                          /* 无效值（调用方已过滤） */
    }
    return (int32_t)lroundf(200.0f * log10f(gain));
}

/* 回放增益格子的状态文字：
   - 总开关关闭 → "关闭"（白色）
   - 开关打开但该曲无增益数据 → "无增益"（白色）：判定标准是"这首歌没有
     .npd 元数据"，即没被电脑端扫描工具处理过；扫描过的曲目即使标签里没有
     ReplayGain，工具也会兜底一个值，属于"有数据"
   - 打开且有数据 → 带符号、保留 1 位小数的分贝值（如 "+3.2 dB" / "-9.1 dB"），
     正增益浅绿、负增益蓝色 */
static void settings_refresh_replay_gain(bool has_gain, uint32_t gain_q16)
{
    if (s_rg_status == NULL) {
        return;
    }

    if (!config_store_get_replay_gain()) {
        lv_label_set_text(s_rg_status, "关闭");
        lv_obj_set_style_text_color(s_rg_status, lv_color_white(), 0);
        return;
    }
    if (!has_gain) {
        lv_label_set_text(s_rg_status, "无增益");
        lv_obj_set_style_text_color(s_rg_status, lv_color_white(), 0);
        return;
    }

    /* 有数据：拼出 ±x.x dB 文本（0 显示 +0.0，正数带 + 号） */
    int32_t tenths = rg_tenths_db_from_q16(gain_q16);
    int32_t abs_t  = (tenths < 0) ? -tenths : tenths;
    char buf[16];
    snprintf(buf, sizeof(buf), "%c%d.%d dB",
             (tenths < 0) ? '-' : '+', (int)(abs_t / 10), (int)(abs_t % 10));
    lv_label_set_text(s_rg_status, buf);
    lv_obj_set_style_text_color(s_rg_status, (tenths < 0) ? RG_NEG_COLOR
                                                          : RG_POS_COLOR, 0);
}

/* 回放增益格子：点击切换总开关（持久化到 NVS；具体生效由控制器同步到
   音频链路，本页文字由 config_store 变更回调立即刷新） */
static void replay_gain_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    bool on = !config_store_get_replay_gain();
    config_store_set_replay_gain(on);
    ESP_LOGI(TAG, "replay gain switch -> %d", (int)on);
}

/* 回放增益格子"上次显示的内容"快照：用于判断是否需要重刷。
   ⚠ 不能只比曲目下标：设置页创建（开机约 1.3s）早于控制器处理播放命令
   （约 1.4s 才载入元数据并置 has_replay_gain），此时读到的是初始的
   "无数据"；若只比曲目号，后续数据就绪时曲目号没变就再也不会刷新，
   表现为"开机后一直显示无增益"。因此曲目号/有无数据/增益值任一变化都重刷。 */
typedef struct {
    uint32_t song;       /* 曲目全局下标 */
    bool     valid;      /* 是否已取到快照 */
    bool     has_gain;   /* 该曲是否有回放增益数据 */
    uint32_t gain_q16;   /* 该曲真实增益 */
} rg_shown_t;

static rg_shown_t s_rg_shown;

/* 播放状态轮询（player_ticker，500ms）：曲目或增益数据变化时刷新显示；
   开关本身的变化由 config_store 回调负责 */
static void settings_player_tick_cb(const player_controller_status_t *st,
                                    void *ctx)
{
    (void)ctx;
    if (st == NULL || !st->has_song) {
        return;
    }
    if (s_rg_shown.valid && st->cur_global == s_rg_shown.song &&
        st->has_replay_gain == s_rg_shown.has_gain &&
        st->replay_gain_q16 == s_rg_shown.gain_q16) {
        return;                            /* 显示内容未变，跳过重绘 */
    }
    s_rg_shown.song     = st->cur_global;
    s_rg_shown.has_gain = st->has_replay_gain;
    s_rg_shown.gain_q16 = st->replay_gain_q16;
    s_rg_shown.valid    = true;
    settings_refresh_replay_gain(st->has_replay_gain, st->replay_gain_q16);
}

/* 配置变更回调（config_store 通知）：外部入口修改配置后同步本页显示 */
static void settings_config_changed_cb(void *ctx)
{
    (void)ctx;
    settings_refresh_loop();
    settings_refresh_shuffle();
    settings_refresh_sleep();
    settings_refresh_eq();
    /* 配置槽位：切换配置（含切换后的循环/随机/EQ 同步）后刷新本页 */
    settings_refresh_slot();
    /* 回放增益：开关变化需立即刷新文字；数值用最近一次 tick 读到的曲目数据 */
    player_controller_status_t st;
    player_controller_get_status(&st);
    settings_refresh_replay_gain(st.has_replay_gain, st.replay_gain_q16);
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
   player_controller_set_loop_mode 内部会写 NVS 并通知 config_store 回调。
   ⚠ 取当前值用 config_store（不是 player_controller_get_loop_mode）：切换配置
   槽位后控制器的循环模式是异步应用的，紧接着点击会从"上一个槽位的模式"起算。 */
static void loop_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    player_loop_mode_t mode = (player_loop_mode_t)config_store_get_loop_mode();
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
    /* ⚠ 当前值读 config_store（切换配置槽位后控制器是异步应用的；两者平时
       一致：player_cmd_set_shuffle 会把结果写回 config_store） */
    bool on = !config_store_get_shuffle();
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

/* 配置切换格子：点击打开配置槽位选择弹窗（SAVE1~SAVE4）。
   切换与应用（音量/EQ/睡眠定时/播放器）全部在弹窗模块内完成
   （见 ui/config_slot_popup.c → core/storage/config_slots.c） */
static void slot_cell_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    config_slot_popup_toggle(s_settings_page);
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
    lv_obj_set_style_pad_row(cell, 4, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
    return cell;
}

/* 创建"图标区"容器（固定 SETTINGS_ICON_W × SETTINGS_ICON_H）：里面只放
   图标标签并垂直居中。所有格子都用它包图标，保证图标区高度完全一致
   （单行图标不会让格子变矮，见 SETTINGS_ICON_H 注释） */
static lv_obj_t *settings_icon_area_create(lv_obj_t *cell)
{
    lv_obj_t *area = lv_obj_create(cell);
    lv_obj_set_size(area, SETTINGS_ICON_W, SETTINGS_ICON_H);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_CLICKABLE);   /* 点击穿透到格子 */
    return area;
}

/* 图标标签：
   - ascii=true 用 LVGL 内置 Montserrat 24（含 PUA 区图标字形，如 ↻ / ⇄）；
   - ascii=false 用 20px 系统小字体 UI_SMALL_FONT（内嵌位图 system_smaller_font），
     供"文字图标"格使用（回放增益格的 "回放\n增益"）——系统字体不含 PUA 图标
     字形，只能排文字。
   ⚠ 图标里的文字不能改用 24px 的 UI_SYSTEM_FONT：NPF 全字库 24px 行高 32，
     两行 64px 远超 SETTINGS_ICON_H(48)；UI_SMALL_FONT 是 20px 点阵（行高 23），
     两行 46px 正好放进 48px 图标区，不需要再压行距（TTF 时代 20px 行高 27，
     当时才需要 text_line_space=-6）。
   ⚠ 换行只能用 "\n"：LVGL 只认 \n 为换行符，"\r\n" 里的 \r 会被当成普通
     字符排版，表现为字间多出一段空隙。 */
static lv_obj_t *settings_icon_label_create(lv_obj_t *cell, const char *icon,
                                            bool ascii)
{
    lv_obj_t *area = settings_icon_area_create(cell);
    lv_obj_t *lbl = lv_label_create(area);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_font(lbl, ascii ? &lv_font_montserrat_24 : UI_SMALL_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    return lbl;
}

/* 状态文字（一行）：
   - ascii=true 用 LVGL 内置 Montserrat（OFF / 剩余 MM:SS 等纯数字冒号，紧凑）；
   - ascii=false 用系统字体 UI_SYSTEM_FONT（中文，或中英混排的文本）。
   ⚠ 只要文本里可能出现中文（哪怕只在某个状态下出现，如回放增益的"关闭"/
   "无增益"），就必须走 ascii=false 的系统字体：Montserrat 是纯 ASCII 字体，
   没有中文字形，无论完整字库是否开启都会画成方框（完整字库只影响
   UI_SYSTEM_FONT 指向哪个文件，不会给内置字体补字）。
   返回句柄供刷新。 */
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
    s_loop_icon = settings_icon_label_create(loop_cell, LV_SYMBOL_LOOP, true);
    s_loop_status = settings_status_label_create(loop_cell, false);
    settings_refresh_loop();
    lv_obj_add_event_cb(loop_cell, loop_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 2：顺序/随机播放（点击切换，图标随状态切换：
       顺序 "AZ" 文字 / 随机 LV_SYMBOL_SHUFFLE） */
    lv_obj_t *shuffle_cell = settings_cell_create(grid);
    s_shuffle_icon = settings_icon_label_create(shuffle_cell, "AZ", true);
    s_shuffle_status = settings_status_label_create(shuffle_cell, false);
    settings_refresh_shuffle();
    lv_obj_add_event_cb(shuffle_cell, shuffle_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 3：睡眠定时（"Zz" 图标 + OFF/剩余 MM:SS；点击轮换档位 + 保存
       默认；长按 关闭/默认时长） */
    lv_obj_t *sleep_cell = settings_cell_create(grid);
    settings_icon_label_create(sleep_cell, "Zz", true);
    s_sleep_status = settings_status_label_create(sleep_cell, true);
    settings_refresh_sleep();
    /* 需同时监听 PRESSED（复位长按标志）、CLICKED（轮换）、LONG_PRESSED（开关） */
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(sleep_cell, sleep_cell_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* 格子 4（左中）：EQ 预设选择（FUTURE_PLAN 第 10 点）。
       图标固定 "EQ" 字样；点击弹出选择列表（无 EQ / 扫描 .musicfx）。
       ⚠ 2026-11 从第 7 格（左下）移到第 4 格（左中）：第 9 格要留给"配置切换" */
    lv_obj_t *eq_cell = settings_cell_create(grid);
    settings_icon_label_create(eq_cell, "EQ", true);
    s_eq_status = settings_status_label_create(eq_cell, false);
    settings_refresh_eq();
    lv_obj_add_event_cb(eq_cell, eq_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 格子 5（中中）：回放增益总开关。
       文字图标用状态栏同款小字体排两行 "回放\n增益"；下方状态文字显示
       当前增益（"−9.1 dB" / "+3.2 dB"）、"无增益"（该曲无 .npd 元数据）
       或"关闭"（总开关关闭），点击切换开关。
       ⚠ 2026-11 从第 8 格（中下）移到第 5 格（中中） */
    lv_obj_t *rg_cell = settings_cell_create(grid);
    settings_icon_label_create(rg_cell, "回放\n增益", false);
    /* ⚠ 状态文字必须用系统字体（ascii=false）："关闭"/"无增益"是中文 */
    s_rg_status = settings_status_label_create(rg_cell, false);
    player_controller_status_t rg_st;
    player_controller_get_status(&rg_st);
    if (rg_st.has_song) {
        s_rg_shown.song     = rg_st.cur_global;
        s_rg_shown.has_gain = rg_st.has_replay_gain;
        s_rg_shown.gain_q16 = rg_st.replay_gain_q16;
        s_rg_shown.valid    = true;
    }
    settings_refresh_replay_gain(rg_st.has_replay_gain, rg_st.replay_gain_q16);
    lv_obj_add_event_cb(rg_cell, replay_gain_cell_cb, LV_EVENT_CLICKED, NULL);
    /* 订阅播放状态轮询：切歌后刷新增益数值 */
    player_ticker_subscribe(settings_player_tick_cb, NULL);

    /* 格子 6~8：预留空位 */
    for (int i = 0; i < 3; i++) {
        settings_cell_create(grid);
    }

    /* 格子 9（右下角）：配置切换（NightPlayer「配置插槽」，2026-11）。
       图标用两行文字"配置\n切换"（系统字体不含 PUA 图标字形，只能排文字，
       与回放增益格同理）；下方状态文字显示当前活跃配置 SAVE1~SAVE4。
       点击弹出 4 个槽位的选择列表（见 ui/config_slot_popup.c） */
    lv_obj_t *slot_cell = settings_cell_create(grid);
    settings_icon_label_create(slot_cell, "配置\n切换", false);
    s_slot_status = settings_status_label_create(slot_cell, false);
    settings_refresh_slot();
    lv_obj_add_event_cb(slot_cell, slot_cell_cb, LV_EVENT_CLICKED, NULL);

    /* 每秒刷新睡眠剩余时间（分:秒） */
    lv_timer_create(sleep_status_timer_cb, SLEEP_REFRESH_MS, NULL);

    /* 订阅配置变更：其它入口修改配置（如通知栏亮度）时同步本页显示 */
    config_store_register_cb(settings_config_changed_cb, NULL);

    ESP_LOGI(TAG, "settings page created (grid: loop/shuffle/sleep timer/EQ/"
                  "replay gain/config slot)");
    return page;
}
