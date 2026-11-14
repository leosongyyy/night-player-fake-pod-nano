#include "ui/settings_menu.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/battery.h"
#include "core/hardware/display.h"
#include "core/hardware/rtc.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist.h"
#include "core/storage/playlist_store.h"
#include "core/ui/click_wheel.h"
#include "core/ui/lvgl_port.h"
#include "core/utils/deep_worker.h"
#include "core/utils/sleep_timer.h"
#include "core/utils/task_psram.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/now_playing.h"
#include "ui/time_date_popup.h"
#include "ui/time_sync_ui.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "settings_menu";

/* ------------------------------------------------------------------ */
/* 菜单尺寸与配色（暗色主题）                                            */
/* ------------------------------------------------------------------ */

/* 菜单四周留白边距（px）：菜单面板不贴屏幕边缘，四周露出背景层纯黑
   （盖住状态栏），点击边缘区域 = 返回上一级/关闭菜单。
   左右边距比上下大 23px（14+23=37）：侧边留给"外侧点击返回"的手势区
   更宽裕，上下保持 14 的紧凑留白。 */
#define MENU_MARGIN_VER  14   /* 上下边距 */
#define MENU_MARGIN_HOR  37   /* 左右边距 = 14 + 23 */

/* 菜单面板尺寸 = 全屏减去对应边距 */
#define MENU_W        (EXAMPLE_LCD_H_RES - MENU_MARGIN_HOR * 2)
#define MENU_H        (EXAMPLE_LCD_V_RES - MENU_MARGIN_VER * 2)

/* 暗色主题配色 */
#define MENU_BG_COLOR     lv_color_hex(0x000000)   /* 菜单/页面背景：纯黑 */
#define CARD_BG_COLOR     lv_color_hex(0x1E1E1E)   /* 行卡片：深灰（同播放页底色） */
#define CARD_PRESS_COLOR  lv_color_hex(0xFFC000)   /* 按下高亮：黄（与通知栏/设置页一致） */
#define TEXT_COLOR        lv_color_white()          /* 主文字：白 */
#define TEXT_DIM_COLOR    lv_palette_main(LV_PALETTE_GREY) /* 次要文字：灰 */
#define TEXT_HL_COLOR     lv_palette_main(LV_PALETTE_YELLOW) /* 选中项：黄 */

/* 行卡片内边距与间距 */
#define CARD_PAD_HOR  16
#define CARD_PAD_VER  14
#define CARD_GAP      12      /* 卡片间距（页面 flex 的 pad_row） */
#define PAGE_PAD      12      /* 页面四周留白 */
#define ICON_FONT     (&lv_font_montserrat_24)

/* 屏幕自动息屏档位（秒）：10秒/15秒/30秒/1分钟/2分钟/5分钟/10分钟 */
static const uint32_t SCREEN_OFF_STEPS[] = { 10, 15, 30, 60, 120, 300, 600 };
#define SCREEN_OFF_STEP_COUNT  (sizeof(SCREEN_OFF_STEPS) / sizeof(SCREEN_OFF_STEPS[0]))
static const char *const SCREEN_OFF_STEP_TXT[] = {
    "10 秒", "15 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟", "10 分钟",
};

/* ------------------------------------------------------------------ */
/* 状态句柄                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_menu            = NULL;  /* lv_menu 面板本体 */
static lv_obj_t *s_menu_bg         = NULL;  /* 全屏黑色背景层（盖住状态栏，接收边缘点击） */
static lv_obj_t *s_root_page       = NULL;  /* 根页面 */
static bool      s_visible         = false; /* 菜单当前是否显示 */

/* 背景层按下位置：CLICKED 时手指已抬起，需在 PRESSED 记录坐标用于判断
   "点击是否落在面板边缘"（面板内空白/控件点击会冒泡到背景层，需排除） */
static lv_point_t s_bg_press_point = { 0, 0 };

/* 根页面行的右侧状态文字（开关状态/当前档位） */
static lv_obj_t *s_sleep_auto_status   = NULL;   /* 睡眠定时器（自动开启模式） */
static lv_obj_t *s_sleep_range_status  = NULL;   /* 睡眠定时器（时间段） */
static lv_obj_t *s_awake_status        = NULL;   /* 清醒感知 */
static lv_obj_t *s_screen_off_status   = NULL;   /* 屏幕自动息屏（当前档位） */
static lv_obj_t *s_rescan_count_status = NULL;   /* 重新扫描（当前歌曲总数，
                                                    不含 .pl 歌单目录；进入菜单时统计一次） */
/* 「时间与日期」页右侧状态：当前日期 / 当前时间 / 时区 / 时间制式 */
static lv_obj_t *s_date_status         = NULL;
static lv_obj_t *s_time_status         = NULL;
static lv_obj_t *s_tz_status           = NULL;
static lv_obj_t *s_time_fmt_status     = NULL;

/* 「电源选项」页：电池信息四项 + 电池健康保护开关的右侧状态文字 */
static lv_obj_t *s_batt_percent_status  = NULL;   /* 当前电量 XX% / 未知 */
static lv_obj_t *s_batt_charging_status = NULL;   /* 充电中 是/否/未知 */
static lv_obj_t *s_batt_voltage_status  = NULL;   /* 电池电压 xxxx mV */
static lv_obj_t *s_batt_care_status     = NULL;   /* 电池健康保护 开/关 */

/* ⚠ 虚拟 ClickWheel 的根行**不再显示开/关状态**（2026-09-11 用户要求）：
   行标题直接写「虚拟 ClickWheel 选项」，开启状态在子页第一行「开启」里看 */

/* 虚拟 ClickWheel 子页三行的右侧状态文字（开关 / 方向 / 速度） */
static lv_obj_t *s_cw_enable_status    = NULL;
static lv_obj_t *s_cw_dir_status       = NULL;
static lv_obj_t *s_cw_speed_status     = NULL;

/* 速度档位显示文字（下标 = CONFIG_STORE_CW_SPEED_*）：
   普通 = 圈数不计算；快速 = ×0.75；极速 = ×0.50（0.25 一档均匀变化） */
static const char *const CW_SPEED_TXT[] = { "普通", "快速", "极速" };

/* 速度档位取值必须两边一致（core/ui 与 config_store 各自定义，防漂移）： */
_Static_assert(CONFIG_STORE_CW_SPEED_COUNT == CLICK_WHEEL_SPEED_COUNT,
               "ClickWheel speed level count mismatch");
_Static_assert(CONFIG_STORE_CW_SPEED_NORMAL == CLICK_WHEEL_SPEED_NORMAL &&
               CONFIG_STORE_CW_SPEED_FAST == CLICK_WHEEL_SPEED_FAST &&
               CONFIG_STORE_CW_SPEED_TURBO == CLICK_WHEEL_SPEED_TURBO,
               "ClickWheel speed level value mismatch");

/* 息屏档位子页的行对象（选中项高亮刷新用） */
static lv_obj_t *s_screen_off_rows[SCREEN_OFF_STEP_COUNT] = { NULL };

/* 子页句柄（**懒加载**：NULL = 尚未构建，首次进入该子页时才创建）。
   动机见 settings_menu_show() 注释：整个菜单常驻需要约 55KB 内部 RAM、
   构建耗时约 310ms，全部从开机路径上移除。 */
static lv_obj_t *s_sleep_page      = NULL;   /* 睡眠定时器 */
static lv_obj_t *s_screen_off_page = NULL;   /* 屏幕自动息屏 */
static lv_obj_t *s_cw_page         = NULL;   /* 虚拟 ClickWheel */
static lv_obj_t *s_time_page       = NULL;   /* 时间与日期 */
static lv_obj_t *s_power_page      = NULL;   /* 电源选项（占位） */
static lv_obj_t *s_help_page       = NULL;   /* 按键帮助 */
static lv_obj_t *s_about_page      = NULL;   /* 关于 */
static lv_obj_t *s_dev_page        = NULL;   /* 开发者选项（关于页的子页） */
static lv_obj_t *s_rescan_page     = NULL;   /* 重新扫描 */

/* ------------------------------------------------------------------ */
/* 暗色主题应用                                                         */
/* ------------------------------------------------------------------ */

/* 行卡片：深灰圆角底，按下黄色高亮（点击行时视觉反馈） */
static void card_style_apply(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, CARD_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_right(card, CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_top(card, CARD_PAD_VER, 0);
    lv_obj_set_style_pad_bottom(card, CARD_PAD_VER, 0);
    lv_obj_set_style_bg_color(card, CARD_PRESS_COLOR, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
}

/* 页面通用样式：黑底 + 四周留白 + 卡片间距 */
static void page_style_apply(lv_obj_t *page)
{
    lv_obj_set_style_bg_color(page, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, PAGE_PAD, 0);
    lv_obj_set_style_pad_row(page, CARD_GAP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    /* 滚动条：深灰（暗色主题） */
    lv_obj_set_style_bg_color(page, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
}

/* 设置 header 里的返回按钮与标题文字为暗色风格：
   lv_menu 内部创建（back_btn + title label），遍历子对象统一处理，
   避免依赖内部结构（按类型识别 label / btn）。 */
static void header_style_apply(lv_obj_t *menu)
{
    lv_obj_t *header = lv_menu_get_main_header(menu);
    if (header == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(header, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    /* 遍历 header 子对象：返回按钮透明化 + 白色图标；标题文字白色+系统字体 */
    uint32_t cnt = lv_obj_get_child_cnt(header);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(header, i);
        if (lv_obj_check_type(child, &lv_button_class)) {   /* v9: lv_btn_class → lv_button_class */
            lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(child, 0, 0);
            lv_obj_set_style_pad_all(child, 8, 0);
            /* 按钮内图标（lv_img 渲染的符号）染白 */
            uint32_t icnt = lv_obj_get_child_cnt(child);
            for (uint32_t j = 0; j < icnt; j++) {
                lv_obj_t *icon = lv_obj_get_child(child, j);
                if (lv_obj_check_type(icon, &lv_image_class)) {   /* v9: lv_img_class → lv_image_class */
                    lv_obj_set_style_image_recolor(icon, TEXT_COLOR, 0);
                    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
                }
            }
        } else if (lv_obj_check_type(child, &lv_label_class)) {
            /* header 标题（当前页面名，中文） */
            lv_obj_set_style_text_font(child, UI_SYSTEM_FONT, 0);
            lv_obj_set_style_text_color(child, TEXT_COLOR, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 行构建辅助                                                           */
/* ------------------------------------------------------------------ */

/* 创建一行卡片（lv_menu cont）：图标 + 标题 + 可选右侧状态文字。
   返回行句柄；status_out 非 NULL 时返回状态 label 句柄（供刷新）。
   行默认可点击（调用方决定挂跳页事件还是点击事件）。 */
static lv_obj_t *menu_row_create(lv_obj_t *page, const char *icon,
                                 const char *title, lv_obj_t **status_out)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style_apply(row);

    if (icon != NULL) {
        lv_obj_t *icon_lbl = lv_label_create(row);
        lv_label_set_text(icon_lbl, icon);
        lv_obj_set_style_text_font(icon_lbl, ICON_FONT, 0);
        lv_obj_set_style_text_color(icon_lbl, TEXT_DIM_COLOR, 0);
    }

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(title_lbl, TEXT_COLOR, 0);
    lv_obj_set_flex_grow(title_lbl, 1);          /* 标题占满剩余宽度 */

    if (status_out != NULL) {
        *status_out = lv_label_create(row);
        lv_label_set_text(*status_out, "");
        lv_obj_set_style_text_font(*status_out, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(*status_out, TEXT_DIM_COLOR, 0);
    }
    return row;
}

/* 简单文本行（无图标，静态文本）：帮助/关于页用 */
static lv_obj_t *menu_text_row_create(lv_obj_t *page, const char *text)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style_apply(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, TEXT_COLOR, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);  /* 超宽自动换行 */
    lv_obj_set_flex_grow(lbl, 1);                      /* 占满行宽，换行生效 */
    return row;
}

/* 灰色小字说明行（无图标，静态文本）：设置项下方的补充说明用。
   卡片样式与普通行一致，仅文字灰色，视觉上弱化区分。 */
static lv_obj_t *menu_note_row_create(lv_obj_t *page, const char *text)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    card_style_apply(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(lbl, TEXT_DIM_COLOR, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
    return row;
}

/* ------------------------------------------------------------------ */
/* 状态刷新                                                             */
/* ------------------------------------------------------------------ */

/* 把自动息屏秒数格式化为档位文字（匹配档位表则用档位名，否则秒/分钟） */
static void screen_off_format(uint32_t sec, char *buf, size_t size)
{
    for (size_t i = 0; i < SCREEN_OFF_STEP_COUNT; i++) {
        if (SCREEN_OFF_STEPS[i] == sec) {
            snprintf(buf, size, "%s", SCREEN_OFF_STEP_TXT[i]);
            return;
        }
    }
    if (sec >= 60 && sec % 60 == 0) {
        snprintf(buf, size, "%u 分钟", (unsigned)(sec / 60));
    } else {
        snprintf(buf, size, "%u 秒", (unsigned)sec);
    }
}

/* 刷新"重新扫描"行右侧的当前歌曲总数（**不含 .pl 歌单目录**的重复计数）。
   ⚠ 必须在**菜单显示时**统计：播放列表（playlist mmap）由 boot_flow 初始化，
   而根页面在 settings_menu_show() 里也是懒加载创建的——首次打开设置菜单时
   播放列表理论上已就绪，但仍用 s_rescan_count_done 保证"整个会话只统计
   一次"（曲库只在重新扫描后变化，而重新扫描必然重启，数字不会失真）。 */
static bool s_rescan_count_done = false;

static void rescan_count_refresh_once(void)
{
    if (s_rescan_count_status == NULL || s_rescan_count_done) {
        return;
    }
    uint32_t n = playlist_song_count_excluding_pl();
    char buf[16];
    snprintf(buf, sizeof(buf), "当前 %u 首", (unsigned)n);
    lv_label_set_text(s_rescan_count_status, buf);
    s_rescan_count_done = true;
    ESP_LOGI(TAG, "rescan row song count (excluding .pl playlists): %u",
             (unsigned)n);
}

/* 刷新「时间与日期」页右侧的日期/时间/时区文字。
   RTC 不可用（未就绪/读失败/时间非法）时日期与时间显示 "--:--:--"；
   时区来自 config_store，始终可显示。 */
static void menu_refresh_time_date(void)
{
    rtc_time_t t;
    bool ok = rtc_is_ready() && (rtc_get_time(&t) == ESP_OK)
           && rtc_time_is_sane(&t);
    char buf[20];

    if (s_date_status != NULL) {
        if (ok) {
            time_date_format_date(&t, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "---- -- --");
        }
        lv_label_set_text(s_date_status, buf);
    }
    if (s_time_status != NULL) {
        if (ok) {
            time_date_format_time(&t, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "--:--:--");
        }
        lv_label_set_text(s_time_status, buf);
    }
    if (s_tz_status != NULL) {
        time_date_format_tz(config_store_get_tz_offset_min(), buf, sizeof(buf));
        lv_label_set_text(s_tz_status, buf);
    }
    if (s_time_fmt_status != NULL) {
        lv_label_set_text(s_time_fmt_status,
                          config_store_get_time_24h() ? "24 小时制" : "12 小时制");
    }
}

/* 「时间与日期」页每秒刷新一次（只在该页可见时读 RTC，I²C 读取 ~1ms）。
   ⚠ 定时器常驻，但 LVGL 在息屏时被挂起，不会在息屏期间读总线。 */
static void time_date_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_time_page == NULL || !lv_obj_is_visible(s_time_page)) {
        return;
    }
    menu_refresh_time_date();
}

/* 刷新「电源选项」页的电池信息（电量 / 充电中 / 电压 / 健康保护开关）。
   PMU 不可用或读数无效时统一显示"未知"（不显示 0 mV / 0% 这种假数据）。
   ⚠ 不显示电池温度：本板 TS 脚接的是固定电阻（无 NTC），读到的"温度"恒定无意义。 */
static void menu_refresh_power(void)
{
    battery_info_t info;
    const bool ok = (battery_read_info(&info) == ESP_OK);

    if (s_batt_percent_status != NULL) {
        if (ok && info.percent != 0xFF) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)info.percent);
            lv_label_set_text(s_batt_percent_status, buf);
        } else {
            lv_label_set_text(s_batt_percent_status, "未知");
        }
    }
    if (s_batt_charging_status != NULL) {
        /* "充电中"严格按"是否仍在充电"判定：阶段 DONE 表示已充满，显示"否"，
           而不是让充电图标/状态位（DONE 仍算 charging）误导用户。 */
        const bool actively_charging = ok && info.charging
                                       && info.charge_stage != AXP2101_CHG_DONE;
        lv_label_set_text(s_batt_charging_status,
                          ok ? (actively_charging ? "是" : "否") : "未知");
    }
    if (s_batt_voltage_status != NULL) {
        if (ok && info.vbat_mv > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u mV", (unsigned)info.vbat_mv);
            lv_label_set_text(s_batt_voltage_status, buf);
        } else {
            lv_label_set_text(s_batt_voltage_status, "未知");
        }
    }
    if (s_batt_care_status != NULL) {
        lv_label_set_text(s_batt_care_status,
                          config_store_get_batt_care() ? "开" : "关");
    }
}

/* 「电源选项」页每秒刷新一次（只在该页可见时才读 I²C / 更新 LVGL）。
   充电中电压/电量变化慢，1s 足够；读取约 1~2ms。 */
static void power_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_power_page == NULL || !lv_obj_is_visible(s_power_page)) {
        return;
    }
    menu_refresh_power();
}

/* 电池健康保护开关：点击切换 → 写 NVS → 立即下发 PMU（CV 4.0V / 4.2V）。
   不需要重启即可生效；PMU 不在时只保存配置并打日志。 */
static void batt_care_row_cb(lv_event_t *e)
{
    (void)e;
    const bool on = !config_store_get_batt_care();
    config_store_set_batt_care(on);
    esp_err_t err = battery_set_health_protection(on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery health protection apply failed: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "battery health protection -> %s (4.0V charge limit: %s)",
             on ? "on" : "off", on ? "stop at 4.0V" : "standard 4.2V");
    menu_refresh_power();
}

/* 睡眠定时「开机自动开启」三档的显示文字（下标 = config_store_sleep_auto_t） */
static const char *const SLEEP_AUTO_TXT[] = {
    "总是关闭", "按时间段", "总是开启",
};

/* 时间段显示："22:00 - 06:00"（跨零点也照原样显示，不额外解释） */
static void sleep_range_format(char *buf, size_t size)
{
    snprintf(buf, size, "%02u:00 - %02u:00",
             (unsigned)config_store_get_sleep_range_start_hour(),
             (unsigned)config_store_get_sleep_range_end_hour());
}

/* 刷新根页面各行的状态文字（打开菜单/开关切换后调用） */
static void menu_refresh_root(void)
{
    if (s_sleep_auto_status != NULL) {
        uint8_t mode = (uint8_t)config_store_get_sleep_auto_mode();
        if (mode >= CONFIG_STORE_SLEEP_AUTO_COUNT) {
            mode = CONFIG_STORE_SLEEP_AUTO_OFF;
        }
        lv_label_set_text(s_sleep_auto_status, SLEEP_AUTO_TXT[mode]);
    }
    if (s_sleep_range_status != NULL) {
        char buf[24];
        sleep_range_format(buf, sizeof(buf));
        lv_label_set_text(s_sleep_range_status, buf);
    }
    if (s_awake_status != NULL) {
        lv_label_set_text(s_awake_status,
                          config_store_get_sleep_awake() ? "开" : "关");
    }
    if (s_screen_off_status != NULL) {
        char buf[16];
        screen_off_format(config_store_get_screen_off_s(), buf, sizeof(buf));
        lv_label_set_text(s_screen_off_status, buf);
    }
    /* 虚拟 ClickWheel 子页三行（开关 / 方向 / 速度） */
    if (s_cw_enable_status != NULL) {
        lv_label_set_text(s_cw_enable_status,
                          config_store_get_click_wheel() ? "开" : "关");
    }
    if (s_cw_dir_status != NULL) {
        lv_label_set_text(s_cw_dir_status,
                          config_store_get_click_wheel_reverse() ? "反转" : "正常");
    }
    if (s_cw_speed_status != NULL) {
        lv_label_set_text(s_cw_speed_status,
                          CW_SPEED_TXT[config_store_get_click_wheel_speed()]);
    }
    menu_refresh_time_date();
}

/* 刷新息屏档位子页的选中高亮（进入子页/切换档位后调用）。
   档位行没有图标，标题 label 是行内第 0 个子对象 */
static void menu_refresh_screen_off_sel(void)
{
    uint32_t cur = config_store_get_screen_off_s();
    for (size_t i = 0; i < SCREEN_OFF_STEP_COUNT; i++) {
        if (s_screen_off_rows[i] == NULL) {
            continue;
        }
        lv_obj_t *lbl = lv_obj_get_child(s_screen_off_rows[i], 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl,
                (SCREEN_OFF_STEPS[i] == cur) ? TEXT_HL_COLOR : TEXT_COLOR, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 交互事件                                                             */
/* ------------------------------------------------------------------ */

/* 睡眠定时器「开机自动开启」：点击在 三档之间循环
   总是关闭 → 按时间段 → 总是开启 → 总是关闭…
   （只是下次开机的判定；不会立刻启动/关闭当前定时） */
static void sleep_auto_row_cb(lv_event_t *e)
{
    (void)e;
    uint8_t mode = (uint8_t)config_store_get_sleep_auto_mode();
    mode = (uint8_t)((mode + 1) % CONFIG_STORE_SLEEP_AUTO_COUNT);
    config_store_set_sleep_auto_mode((config_store_sleep_auto_t)mode);
    ESP_LOGI(TAG, "sleep auto mode -> %u", (unsigned)mode);
    menu_refresh_root();
}

/* 时间段选择回调（滚轮弹窗保存）：写配置并刷新本页状态文字 */
static void sleep_range_saved_cb(uint8_t start_hour, uint8_t end_hour)
{
    config_store_set_sleep_range_hours(start_hour, end_hour);
    ESP_LOGI(TAG, "sleep range -> %02u:00 - %02u:00", (unsigned)start_hour,
             (unsigned)end_hour);
    menu_refresh_root();
}

/* 睡眠定时器「时间段」：打开 开始/结束 小时滚轮弹窗 */
static void sleep_range_row_cb(lv_event_t *e)
{
    (void)e;
    time_date_popup_show_hour_range("睡眠时间段",
                                    config_store_get_sleep_range_start_hour(),
                                    config_store_get_sleep_range_end_hour(),
                                    sleep_range_saved_cb);
}

/* 清醒感知开关：点击切换并持久化 */
static void awake_row_cb(lv_event_t *e)
{
    (void)e;
    bool on = !config_store_get_sleep_awake();
    config_store_set_sleep_awake(on);
    ESP_LOGI(TAG, "sleep awake -> %d", (int)on);
    menu_refresh_root();
}

/* 虚拟 ClickWheel 子页三项：点击切换并持久化（NVS），**立即生效**——
   config_store 写完后直接驱动 click_wheel 的开关/方向/速度，不需要像
   开发者选项那样重启（见 core/ui/click_wheel.h） */

/* 1. 开启/关闭 */
static void cw_enable_row_cb(lv_event_t *e)
{
    (void)e;
    bool on = !config_store_get_click_wheel();
    config_store_set_click_wheel(on);
    click_wheel_set_enabled(on);
    menu_refresh_root();
    ESP_LOGI(TAG, "virtual click wheel -> %d", (int)on);
}

/* 2. 滑动方向：正常（顺时针向下滚）/ 反转（顺时针向上滚） */
static void cw_dir_row_cb(lv_event_t *e)
{
    (void)e;
    bool reverse = !config_store_get_click_wheel_reverse();
    config_store_set_click_wheel_reverse(reverse);
    click_wheel_set_reverse(reverse);
    menu_refresh_root();
    ESP_LOGI(TAG, "virtual click wheel direction -> %s",
             reverse ? "reverse" : "normal");
}

/* 3. 速度：点击**循环**切换三档（普通 ×1.00 / 快速 ×0.75 / 极速 ×0.50），
      0.25 一档均匀变化；系数越小越灵敏 */
static void cw_speed_row_cb(lv_event_t *e)
{
    (void)e;
    uint8_t next = (uint8_t)((config_store_get_click_wheel_speed() + 1u) %
                             CONFIG_STORE_CW_SPEED_COUNT);
    config_store_set_click_wheel_speed(next);
    click_wheel_set_speed(next);
    menu_refresh_root();
    ESP_LOGI(TAG, "virtual click wheel speed -> %s (level %u)",
             CW_SPEED_TXT[next], (unsigned)next);
}

/* 息屏档位行：点击设置并返回根页面。
   ⚠ 不能直接 lv_menu_set_page(root)：lv_menu 会把 root 也压入历史
   （cur_depth 增长），返回按钮会"先回子页再回根"；先 clear_history
   再 set_page(root) 才能干净地回到根（cur_depth=1）。 */
static void screen_off_row_cb(lv_event_t *e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    uint32_t sec = SCREEN_OFF_STEPS[idx];
    config_store_set_screen_off_s(sec);
    ESP_LOGI(TAG, "screen off -> %u s", (unsigned)sec);
    menu_refresh_screen_off_sel();
    menu_refresh_root();
    lv_menu_clear_history(s_menu);
    lv_menu_set_page(s_menu, s_root_page);
}

/* 返回按钮（header）：根页面时点击 = 关闭菜单；
   子页面时 lv_menu 内部已处理返回，本回调不拦截 */
static void back_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (lv_menu_back_button_is_root(s_menu, btn)) {   /* v9: lv_menu_back_btn_is_root → ..._button_... */
        settings_menu_hide();
    }
}

/* ------------------------------------------------------------------ */
/* 重新扫描：停止播放 → 使播放列表数据无效 → 重启（boot_flow 重新扫描）   */
/* ------------------------------------------------------------------ */

/* 重新扫描任务参数：流程含 mp3_player_stop 的等待（解码任务退出、释放
   文件句柄/I2S，最多 1s），放独立任务执行，避免阻塞 LVGL/按键任务。
   ⚠ 栈放 **PSRAM**（task_create_psram_stack）：触发时内部 RAM 已被 LVGL
   堆吃满（实测仅剩约 22KB，而**最大连续块只有 8180 B < 8KB 栈需求**，
   内部分配必然失败——历史症状"无法分配栈内存/点了没反应"）。
   ⚠ 但"关缓存"的那段必须跑在**内部栈**上：本流程要 unmap pldata 分区
   （player_controller_deinit → playlist_deinit → pldata_munmap →
   esp_partition_munmap → esp_mmu_unmap），关缓存期间 PSRAM 不可访问，
   栈在 PSRAM 会命中 esp_cache_freeze_caches_disable_interrupts 的断言
   s_task_stack_is_sane_when_cache_frozen() 直接崩溃。故这段委托给
   deep_worker（内部大栈，见 rescan_do_deinit）。 */
#define RESCAN_TASK_STACK  (4 * 1024)
#define RESCAN_TASK_PRIO   6
#define RESCAN_TASK_CORE   1

/* 释放"重扫前可丢"的任务：用户已确认重扫（马上 esp_restart），这些任务的
   职责不再有意义，顺带停掉它们避免重扫过程中被误触发（如睡眠定时到点渐降）：
   - cover_load（6KB，Core1，阻塞在通知上）：不再需要换封面；
   - sleep_timer（4KB，Core1，1s 轮询）：不再需要睡眠定时/音量渐降。
   两个模块内部都会把句柄置空，避免残留野句柄被后续唤醒/通知使用。 */
static void rescan_release_tasks(void)
{
    now_playing_release_cover_task();
    sleep_timer_release_task();
}

/* 关缓存段（在 deep_worker 内部栈上执行）：停止播放控制器 → 卸载播放列表
   mmap → 擦除列表头使数据失效。
   ⚠ 必须在内部栈执行（见 RESCAN_TASK_STACK 注释）；fn 内禁止再调
   deep_worker_call（会持锁等自身死锁）。 */
static void rescan_do_deinit(void *arg)
{
    (void)arg;
    player_controller_deinit();
    esp_err_t ret = playlist_store_invalidate();
    ESP_LOGI(TAG, "playlist invalidated (%s)", esp_err_to_name(ret));
}

/* 重新扫描任务（PSRAM 栈）：挂起 LVGL → 委托 deep_worker 停播放/卸载映射/
   使数据失效 → 重启。重启后 boot_flow 检测到分区无效，进入"需要扫描音乐"
   界面，用户确认后重新扫描 SD 卡（完整闭环，无需本任务做扫描）。

   ⚠ 顺序不可颠倒（内存安全关键）：
   1. 先挂起 LVGL：player_ticker/列表刷新/正在播放页等都在 LVGL 任务上下文
      读 playlist 指针，不先挂起的话，playlist 卸载后它们会访问失效映射；
   2. rescan_do_deinit（deep_worker）：停播放 → 停控制器任务 → 删队列 →
      playlist_deinit()（munmap + 清空 playlist 指针）→ 擦头使数据失效。 */
static void rescan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "=== rescan: stop playback, invalidate playlist, restart ===");

    /* 1. 挂起 LVGL：停止 player_ticker/列表刷新/播放页刷新等所有读
          playlist 数据的 UI 活动（菜单画面定格，马上重启无影响） */
    lvgl_port_suspend();
    ESP_LOGI(TAG, "LVGL suspended");

    /* 2. 停播放 + 卸载播放列表映射 + 使数据失效：必须跑在内部栈
          （deep_worker），本任务是 PSRAM 栈，直接做 munmap 会触发关缓存断言 */
    if (!deep_worker_call(rescan_do_deinit, NULL)) {
        ESP_LOGW(TAG, "deep_worker unavailable, run deinit on this task "
                      "(PSRAM stack: munmap may assert)");
        rescan_do_deinit(NULL);
    }

    /* 3. 等待日志刷出后重启 */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    vTaskDelete(NULL);   /* 理论不可达 */
}

/* 重新扫描确认（"是"）：释放可丢任务 → 创建 PSRAM 栈重扫任务 */
static void rescan_confirm_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "rescan confirmed, start rescan task");
    rescan_release_tasks();

    /* ⚠ 栈放 PSRAM（内部 RAM 最大连续块不足 8KB，见 RESCAN_TASK_STACK
       注释）；关缓存段由 rescan_task 内部委托 deep_worker 执行 */
    if (task_create_psram_stack(rescan_task, "rescan",
                                RESCAN_TASK_STACK, NULL,
                                RESCAN_TASK_PRIO, NULL,
                                RESCAN_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create rescan task failed (internal free=%u, largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

/* 重新扫描取消（"否"）：返回根页面（等同点击返回按钮回到上一级） */
static void rescan_cancel_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "rescan cancelled");
    lv_menu_clear_history(s_menu);
    lv_menu_set_page(s_menu, s_root_page);
}

/* 点击是否落在菜单面板内部（含面板区域）。边缘点击 = 面板外区域 */
static bool menu_point_in_panel(lv_coord_t x, lv_coord_t y)
{
    return x >= MENU_MARGIN_HOR && x < MENU_MARGIN_HOR + MENU_W
        && y >= MENU_MARGIN_VER && y < MENU_MARGIN_VER + MENU_H;
}

/* 全屏背景层按下：记录坐标（供 CLICKED 判断边缘/面板内） */
static void menu_bg_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev != NULL) {
        lv_indev_get_point(indev, &s_bg_press_point);
    }
}

/* 全屏背景层点击（边缘区域）：模拟点击 header 返回按钮——
   子页面时返回上一级，根页面（最后一级）时关闭菜单。
   ⚠ 面板内控件（行/按钮）的事件会冒泡到背景层，必须按坐标过滤：
   落在面板内的点击交给面板自身处理，只有面板外（32px 边缘）才返回/关闭。 */
static void menu_bg_click_cb(lv_event_t *e)
{
    if (menu_point_in_panel(s_bg_press_point.x, s_bg_press_point.y)) {
        return;   /* 面板内：不处理（面板控件自行消费） */
    }
    ESP_LOGI(TAG, "edge click (%d,%d): back one level or close",
             (int)s_bg_press_point.x, (int)s_bg_press_point.y);
    /* 触发与 header 返回按钮相同的行为（复用 back_btn_cb 的关闭逻辑） */
    lv_obj_t *back = lv_menu_get_main_header_back_button(s_menu);   /* v9 改名 */
    /* ⚠ v9 的 lv_event_send() 已是底层接口（参数为事件链表、lv_event_t 与布尔量），
       向对象派发事件必须用 lv_obj_send_event()。v8 的 lv_event_send(obj, code, param)
       写法在 v9 里参数个数相同但含义完全不同，能编过却会跑飞 */
    lv_obj_send_event(back, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------ */
/* 开发者选项：触摸测试白点（NVS 运行时开关，重启生效）                   */
/* （S3 时代的电池电压采样日志已删除：S31 由 AXP2101 直接给百分比）        */
/* ------------------------------------------------------------------ */

/* 开发者选项子页行的右侧状态文字句柄 */
static lv_obj_t *s_dev_touch_status    = NULL;  /* 触摸测试白点：开/关 */

static void dev_refresh_touch(void)
{
    if (s_dev_touch_status == NULL) {
        return;
    }
    lv_label_set_text(s_dev_touch_status,
                      config_store_get_touch_test() ? "开" : "关");
}

/* 触摸测试白点开关：点击切换并持久化（NVS），重启后生效 */
static void dev_touch_row_cb(lv_event_t *e)
{
    (void)e;
    config_store_set_touch_test(!config_store_get_touch_test());
    dev_refresh_touch();
    ESP_LOGI(TAG, "touch test -> %d (reboot to apply)",
             (int)config_store_get_touch_test());
}

/* 立即重启任务参数：只需短暂延时等点击反馈/日志刷出后重启，栈 2KB 足够 */
#define DEV_RESTART_TASK_STACK  (2 * 1024)
#define DEV_RESTART_TASK_PRIO   6
#define DEV_RESTART_TASK_CORE   1

/* 立即重启任务：延时后 esp_restart，让开发者选项的修改在本机生效。
   不需要走 rescan 那套停止播放/释放资源的流程——设置项已同步写 NVS，
   重启只重新读取，不涉及内存中的播放列表资源 */
static void dev_restart_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "restart now to apply developer options");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    vTaskDelete(NULL);   /* 理论不可达 */
}

/* 立即重启生效行：点击创建重启任务 */
static void dev_restart_row_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "developer options: restart requested");
    if (xTaskCreatePinnedToCore(dev_restart_task, "dev_restart",
                                DEV_RESTART_TASK_STACK, NULL,
                                DEV_RESTART_TASK_PRIO, NULL,
                                DEV_RESTART_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create dev restart task failed");
    }
}

/* 构建开发者选项子页（**懒加载**：从关于页首次点进来时才构建）。
   S31 版本只保留"触摸测试白点"（开机检查、重启生效）：
   它对应 lvgl_port_init 末尾是否创建圆点。
   入口行由 about_page_build() 挂在关于页最下方。 */
static lv_obj_t *dev_page_build(void)
{
    if (s_dev_page != NULL) {
        return s_dev_page;
    }
    lv_obj_t *dev_page = lv_menu_page_create(s_menu, (char *)"开发者选项");
    page_style_apply(dev_page);

    /* 1. 触摸测试白点：开关 */
    lv_obj_t *touch_row = menu_row_create(dev_page, NULL,
                                          "触摸测试白点", &s_dev_touch_status);
    lv_obj_add_flag(touch_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_row, dev_touch_row_cb, LV_EVENT_CLICKED, NULL);

    /* 2. 功能说明（小字）：同样按段落拆成多张卡片（滚动性能，见睡眠定时器页注释） */
    menu_note_row_create(dev_page,
        "触摸测试白点：白色圆点跟随手指，用于校准触摸区域。");
    menu_note_row_create(dev_page,
        "以上修改保存到设备（NVS），重启后生效。");

    /* 3. 立即重启：一键生效 */
    lv_obj_t *restart_row = menu_row_create(dev_page, LV_SYMBOL_REFRESH,
                                            "立即重启生效", NULL);
    lv_obj_add_flag(restart_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(restart_row, dev_restart_row_cb, LV_EVENT_CLICKED, NULL);

    /* 初始刷新状态文字（打开页面时显示当前 NVS 值） */
    dev_refresh_touch();

    s_dev_page = dev_page;
    ESP_LOGI(TAG, "developer options page built (touch test)");
    return dev_page;
}

/* ------------------------------------------------------------------ */
/* 页面构建（子页懒加载）                                                */
/* ------------------------------------------------------------------ */

/* ⚠ 整个设置菜单常驻**实测约 29KB 内部 RAM（= DMA 池）、构建耗时约 310ms**
   （`IRAM_DMA_AUDIT.md` 早期按残差估的"约 55KB"偏大，2026-09 实测修正为
   29KB：`ui:after app_ui_create` 的内部 RAM 从 27.6KB 余量变回 56.4KB）。
   为把它从**开机路径**上彻底移除，页面改为**分级懒加载**：
     - 外壳（背景层 + lv_menu + header）与根页面：首次打开菜单时构建
       （实测 44ms）；
     - 6 个子页（+ 关于页里的"开发者选项"）：**首次点进该子页时**才构建。
   实测效果：开机主界面提前约 305ms 出现（1943 → 1638ms），且未打开过设置时
   常驻内部 RAM / DMA 各多出约 29KB（SD 读余量同步变宽松）。 */

/* 各子页构建器（**幂等**：已构建则直接返回句柄）。定义在本节末尾，
   这里先统一前向声明（root_page_build / about_page_build 会引用）。 */
static lv_obj_t *sleep_page_build(void);
static lv_obj_t *screen_off_page_build(void);
static lv_obj_t *cw_page_build(void);
static lv_obj_t *time_date_page_build(void);
static lv_obj_t *power_page_build(void);
static lv_obj_t *help_page_build(void);
static lv_obj_t *about_page_build(void);
static lv_obj_t *dev_page_build(void);
static lv_obj_t *rescan_page_build(void);

/* 懒加载入口表：行的事件 user_data 传**下标**（C 不允许函数指针与 void*
   互转，故不走"指针塞 user_data"那条路） */
#define LAZY_ENTRY_MAX  12
static lv_obj_t *(*s_lazy_builders[LAZY_ENTRY_MAX])(void);
static uint8_t s_lazy_count = 0;

/* 子页入口行点击（懒加载）：首次点击才构建子页，随后按 lv_menu 的常规
   方式加载。等价于 LVGL 的 lv_menu_set_load_page_event（它内部也只是
   click → lv_menu_set_page），只多一步"按需构建"。 */
static void lazy_page_row_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *page = s_lazy_builders[idx]();
    if (page != NULL) {
        lv_menu_set_page(s_menu, page);
    }
}

/* 创建"子页入口行"（parent 内）：点击时才构建子页。
   行标记与 lv_menu_set_load_page_event 保持一致（可点击 + 不滚动 +
   聚焦时滚动），保证视觉与滚动行为不变。 */
static lv_obj_t *lazy_row_create(lv_obj_t *parent, const char *icon,
                                 const char *title, lv_obj_t **status_out,
                                 lv_obj_t *(*build)(void))
{
    lv_obj_t *row = menu_row_create(parent, icon, title, status_out);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    if (s_lazy_count >= LAZY_ENTRY_MAX) {
        ESP_LOGW(TAG, "lazy entry table full, row '%s' cannot open page", title);
        return row;
    }
    s_lazy_builders[s_lazy_count] = build;
    lv_obj_add_event_cb(row, lazy_page_row_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)s_lazy_count);
    s_lazy_count++;
    return row;
}

/* 根页面：8 个功能行（睡眠定时器 / 屏幕自动息屏 / 虚拟 ClickWheel /
   时间与日期 / 电源选项 / 按键帮助 / 关于 / 重新扫描），全部为子页入口
   （子页懒加载，见上）。
   ⚠ 行数增加后根页面内容高度超过可视区（实测 6 行需 426px、可视 357px），
   页面会出现滚动条——这是预期行为，不再压缩行距。 */
static void root_page_build(void)
{
    /* ⚠ lv_menu_page_create 的 title 是 char*（非 const），内部会拷贝，
       字符串字面量需强转（LVGL 8.4 的 API 签名如此） */
    s_root_page = lv_menu_page_create(s_menu, (char *)"设置");
    page_style_apply(s_root_page);

    /* 入口行：点击时才构建对应子页 */
    lazy_row_create(s_root_page, LV_SYMBOL_POWER, "睡眠定时器",
                    NULL, sleep_page_build);
    lazy_row_create(s_root_page, LV_SYMBOL_EYE_CLOSE, "屏幕自动息屏",
                    &s_screen_off_status, screen_off_page_build);
    /* 入口行：**不显示右侧状态**（开关状态在子页第一行「开启」里看），
       标题直接点明是"选项"页——2026-09-11 用户要求 */
    lazy_row_create(s_root_page, LV_SYMBOL_REFRESH, "虚拟 ClickWheel 选项",
                    NULL, cw_page_build);
    /* 时间与日期 / 电源选项：按要求插在「按键帮助」之前。
       电源选项本轮先占位（空白页），后续会话继续写 */
    lazy_row_create(s_root_page, LV_SYMBOL_EDIT, "时间与日期",
                    NULL, time_date_page_build);
    lazy_row_create(s_root_page, LV_SYMBOL_BATTERY_FULL, "电源选项",
                    NULL, power_page_build);
    lazy_row_create(s_root_page, LV_SYMBOL_KEYBOARD, "按键帮助",
                    NULL, help_page_build);
    lazy_row_create(s_root_page, LV_SYMBOL_SETTINGS, "关于",
                    NULL, about_page_build);
    /* 右侧状态 = 当前歌曲总数（不含 .pl 歌单目录），显示菜单时统计一次 */
    lazy_row_create(s_root_page, LV_SYMBOL_REFRESH, "重新扫描",
                    &s_rescan_count_status, rescan_page_build);

    menu_refresh_root();
}

/* 1. 睡眠定时器子页：开机默认开启 + 清醒感知开关，
   底部放清醒感知与休眠行为说明 */
static lv_obj_t *sleep_page_build(void)
{
    if (s_sleep_page != NULL) {
        return s_sleep_page;
    }
    lv_obj_t *sleep_page = lv_menu_page_create(s_menu, (char *)"睡眠定时器");
    page_style_apply(sleep_page);

    /* 开机自动开启：点击在 总是关闭 / 按时间段 / 总是开启 之间循环 */
    lv_obj_t *sleep_toggle = menu_row_create(sleep_page, NULL,
                                             "开机自动开启", &s_sleep_auto_status);
    lv_obj_add_flag(sleep_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_toggle, sleep_auto_row_cb, LV_EVENT_CLICKED, NULL);

    /* 时间段：开始/结束小时滚轮弹窗（仅「按时间段」模式生效，支持跨零点） */
    lv_obj_t *range_row = menu_row_create(sleep_page, NULL,
                                          "时间段", &s_sleep_range_status);
    lv_obj_add_flag(range_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(range_row, sleep_range_row_cb, LV_EVENT_CLICKED, NULL);

    /* 睡眠清醒感知：开关（睡眠定时器的子功能，一并收进本子菜单） */
    lv_obj_t *awake_toggle = menu_row_create(sleep_page, NULL,
                                             "清醒感知", &s_awake_status);
    lv_obj_add_flag(awake_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(awake_toggle, awake_row_cb, LV_EVENT_CLICKED, NULL);

    /* 底部说明：按**段落拆成多条**独立行卡片（每段一张卡）。
       ⚠ 性能原因（实测本页滚动卡顿）：原先整页说明写成**一个**超长
       `LV_LABEL_LONG_WRAP` 标签，标签高度数百像素；LVGL 的失效区域按
       **对象整体**计算，滚动时只要有一行文字在视口内，整个大标签都会被
       重绘（含视口外的部分），换行计算与字形渲染都在同一帧里做。
       拆成多张卡片后，每帧只有与失效区域相交的卡片重绘，滚动明显顺畅。
       ⚠ 后续给本菜单加说明文字时，请沿用"一段一卡"，不要写回单个长标签。 */
    menu_note_row_create(sleep_page,
        "开启清醒感知后，若在睡眠定时器期间进行操作，剩余时间会按照一定比例增加，"
        "让您在入梦前音乐不断；倒计时即将结束时，音量会逐渐减小，让音乐自然淡出。");
    menu_refresh_root();   /* 本页三行的状态文字 */
    s_sleep_page = sleep_page;
    ESP_LOGI(TAG, "sleep timer page built");
    return sleep_page;
}

/* 2. 屏幕自动息屏子页：选择档位 */
static lv_obj_t *screen_off_page_build(void)
{
    if (s_screen_off_page != NULL) {
        return s_screen_off_page;
    }
    lv_obj_t *screen_off_page = lv_menu_page_create(s_menu, (char *)"屏幕自动息屏");
    page_style_apply(screen_off_page);
    for (size_t i = 0; i < SCREEN_OFF_STEP_COUNT; i++) {
        lv_obj_t *opt = menu_row_create(screen_off_page, NULL,
                                        SCREEN_OFF_STEP_TXT[i], NULL);
        lv_obj_add_flag(opt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(opt, screen_off_row_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_screen_off_rows[i] = opt;
    }
    menu_refresh_screen_off_sel();
    s_screen_off_page = screen_off_page;
    ESP_LOGI(TAG, "screen off page built");
    return screen_off_page;
}

/* 3. 虚拟 ClickWheel 子页（长列表右下角速滑按钮）。
    原先是一行"点击即开关"，现改为**子页入口**（本页三项）：
      开启/关闭、滑动方向（正常/反转）、速度（普通/快速/极速，点击循环）；
    三项均点击立即生效（写 NVS + 直接驱动 click_wheel，无需重启）。
    图标与按钮本身一致（LV_SYMBOL_REFRESH） */
static lv_obj_t *cw_page_build(void)
{
    if (s_cw_page != NULL) {
        return s_cw_page;
    }
    lv_obj_t *cw_page = lv_menu_page_create(s_menu, (char *)"虚拟 ClickWheel");
    page_style_apply(cw_page);

    lv_obj_t *cw_enable = menu_row_create(cw_page, NULL,
                                          "开启", &s_cw_enable_status);
    lv_obj_add_flag(cw_enable, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cw_enable, cw_enable_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cw_dir = menu_row_create(cw_page, NULL,
                                       "滑动方向", &s_cw_dir_status);
    lv_obj_add_flag(cw_dir, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cw_dir, cw_dir_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cw_speed = menu_row_create(cw_page, NULL,
                                         "速度", &s_cw_speed_status);
    lv_obj_add_flag(cw_speed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cw_speed, cw_speed_row_cb, LV_EVENT_CLICKED, NULL);

    /* 说明：**只保留一条"这个功能是什么"**（沿用"一段一卡"写法，滚动性能
       见睡眠定时器页注释）。⚠ 方向/速度的细节**刻意不写**（2026-09-11 用户
       要求）：那属于手感差异，交给用户自己试；文字里也不提转向，避免暗示
       只有一种转法。 */
    menu_note_row_create(cw_page,
        "开启后，目录/歌曲列表超过 10 页（72 条）时右下角出现黄色圆钮："
        "按住并在屏幕上画圈即可快速翻页。");
    menu_refresh_root();   /* 本页三行状态（开关 / 方向 / 速度） */
    s_cw_page = cw_page;
    ESP_LOGI(TAG, "click wheel page built");
    return cw_page;
}

/* 时间与日期页四行的点击回调：前三项打开滚轮弹窗 / 全屏对时，
   弹窗与进度界面都挂在 lv_layer_top()，覆盖菜单与状态栏 */
static void time_date_date_row_cb(lv_event_t *e)
{
    (void)e;
    time_date_popup_show_date();
}

static void time_date_time_row_cb(lv_event_t *e)
{
    (void)e;
    time_date_popup_show_time();
}

static void time_date_sync_row_cb(lv_event_t *e)
{
    (void)e;
    time_sync_ui_start();
}

static void time_date_tz_row_cb(lv_event_t *e)
{
    (void)e;
    time_date_popup_show_tz();
}

/* 时间格式：点击在 24h / 12h 之间切换（12h 不显示上下午），立即写 NVS，
   状态栏/下拉栏的时钟由 config_store 变更回调即时刷新 */
static void time_date_fmt_row_cb(lv_event_t *e)
{
    (void)e;
    bool on = !config_store_get_time_24h();
    config_store_set_time_24h(on);
    menu_refresh_time_date();
    ESP_LOGI(TAG, "time format -> %s", on ? "24h" : "12h");
}

/* 4. 时间与日期子页（2026-11 新增）：
      设置日期 / 时间设置 / WiFi对时 / 时区设置 / 时间格式 五项，
      右侧显示当前值。
      日期/时间/时区点击后弹出滚轮弹窗（ui/time_date_popup.c）；
      WiFi对时进入全屏进度界面（ui/time_sync_ui.c），读取
      /sdcard/.ecov/wifi.txt 连 WiFi 并做 NTP。
      RTC 里存的是本地墙钟时间，时区只影响网络对时的 UTC 换算；
      改时区时由弹窗把 RTC 平移，保持绝对时刻不变。 */
static lv_obj_t *time_date_page_build(void)
{
    if (s_time_page != NULL) {
        return s_time_page;
    }
    lv_obj_t *page = lv_menu_page_create(s_menu, (char *)"时间与日期");
    page_style_apply(page);

    lv_obj_t *date_row = menu_row_create(page, NULL, "设置日期",
                                         &s_date_status);
    lv_obj_add_flag(date_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(date_row, time_date_date_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *time_row = menu_row_create(page, NULL, "时间设置",
                                         &s_time_status);
    lv_obj_add_flag(time_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(time_row, time_date_time_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sync_row = menu_row_create(page, LV_SYMBOL_WIFI, "WiFi对时", NULL);
    lv_obj_add_flag(sync_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sync_row, time_date_sync_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tz_row = menu_row_create(page, NULL, "时区设置", &s_tz_status);
    lv_obj_add_flag(tz_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tz_row, time_date_tz_row_cb, LV_EVENT_CLICKED, NULL);

    /* 时间格式：24 小时制 / 12 小时制（12h 不显示上下午） */
    lv_obj_t *fmt_row = menu_row_create(page, NULL, "时间格式",
                                        &s_time_fmt_status);
    lv_obj_add_flag(fmt_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(fmt_row, time_date_fmt_row_cb, LV_EVENT_CLICKED, NULL);

    /* 只保留一句简短说明：WiFi 配置由电脑版工具写入，本页不再堆细节
       （2026-11-14 用户要求删掉原来的两段长说明） */
    menu_note_row_create(page, "* WiFi对时可在电脑版工具里配置");

    /* 弹窗保存/对时成功后刷新本页右侧状态 */
    time_date_popup_set_refresh_cb(menu_refresh_time_date);
    menu_refresh_time_date();

    /* 每秒刷新"时间设置"右侧的秒数（仅本页可见时才真正读 RTC） */
    lv_timer_create(time_date_tick_cb, 1000, NULL);

    s_time_page = page;
    ESP_LOGI(TAG, "time & date page built");
    return page;
}

/* 5. 电源选项子页（2026-11）：
      · 电池信息（无分组标题）：典型容量（Kconfig 固定值，仅展示）/ 当前电量 /
        充电中 / 电池电压；后三项每 1 秒刷新（仅本页可见时读 PMU）。
      · 电池健康保护开关（4.0V 软件限充），点击立即生效；下方只有一条说明。
      ⚠ PMU 不可用 / 读数越界时统一显示"未知"，不显示假的 0。 */
static lv_obj_t *power_page_build(void)
{
    if (s_power_page != NULL) {
        return s_power_page;
    }
    lv_obj_t *page = lv_menu_page_create(s_menu, (char *)"电源选项");
    page_style_apply(page);

    /* 典型容量：编译期配置（CONFIG_PMU_BAT_CAPACITY_MAH，默认 600mAh），
       纯展示，给用户一个"这机器电池多大"的参考 */
    lv_obj_t *cap_status = NULL;
    menu_row_create(page, NULL, "典型容量", &cap_status);
    if (cap_status != NULL) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d mAh", CONFIG_PMU_BAT_CAPACITY_MAH);
        lv_label_set_text(cap_status, buf);
    }

    lv_obj_t *percent_row = menu_row_create(page, NULL, "当前电量",
                                            &s_batt_percent_status);
    lv_obj_add_flag(percent_row, LV_OBJ_FLAG_CLICKABLE);   /* 可点（无动作），
                                                              只为跟其它行一致的按压反馈 */
    lv_obj_t *charging_row = menu_row_create(page, NULL, "充电中",
                                             &s_batt_charging_status);
    lv_obj_add_flag(charging_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *voltage_row = menu_row_create(page, NULL, "电池电压",
                                            &s_batt_voltage_status);
    lv_obj_add_flag(voltage_row, LV_OBJ_FLAG_CLICKABLE);
    /* ⚠ 不显示「电池温度」：本板 TS 脚是固定电阻（无 NTC），读数恒定、没有意义 */

    lv_obj_t *care_row = menu_row_create(page, NULL, "电池健康保护",
                                         &s_batt_care_status);
    lv_obj_add_flag(care_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(care_row, batt_care_row_cb, LV_EVENT_CLICKED, NULL);

    /* 只保留一条说明（用户指定文案，不要拆成多段/不要加温度说明） */
    menu_note_row_create(page,
        "开启电池健康保护后，充电会限制在约80%-90%左右，以延长电池寿命。"
        "建议定期充满一次来校准电量显示。");

    /* 初始刷新 + 每秒刷新（仅本页可见时） */
    menu_refresh_power();
    lv_timer_create(power_tick_cb, 1000, NULL);

    s_power_page = page;
    ESP_LOGI(TAG, "power options page built (battery info + health protection)");
    return page;
}

/* 6. 按键帮助子页：静态说明。
    按键命名用社区通用说法：上键 = A 键（电源键），下键 = B 键；
    "关机"指长按上键进入关机流程（约 3 秒彻底断电）。 */
static lv_obj_t *help_page_build(void)
{
    if (s_help_page != NULL) {
        return s_help_page;
    }
    lv_obj_t *help_page = lv_menu_page_create(s_menu, (char *)"按键帮助");
    page_style_apply(help_page);
    menu_text_row_create(help_page,
        "亮屏状态：\n上键 短按 音量+\n下键 短按 音量-\n"
        "上键 长按 关机\n下键 长按 音量-10\n两个按键一起按下 息屏");
    menu_text_row_create(help_page,
        "息屏状态：\n上键 短按 播放/暂停\n下键 短按 下一曲\n"
        "下键 长按 上一曲\n两个按键一起按下 亮屏");
    s_help_page = help_page;
    ESP_LOGI(TAG, "key help page built");
    return help_page;
}

/* 7. 关于子页：作者信息 + 固件版本（Kconfig FIRMWARE_VERSION_STRING）。
    最下方追加"开发者选项"入口子页（同样懒加载，见 dev_page_build） */
static lv_obj_t *about_page_build(void)
{
    if (s_about_page != NULL) {
        return s_about_page;
    }
    lv_obj_t *about_page = lv_menu_page_create(s_menu, (char *)"关于");
    page_style_apply(about_page);
    menu_text_row_create(about_page,
        "FakePodNanoNightPlayer\n基于 NightPlayer 的第三方固件");
    menu_text_row_create(about_page, "作者：ZYFDroid\nAI 协作：DeepSeek");
    menu_text_row_create(about_page, "固件版本：" CONFIG_FIRMWARE_VERSION_STRING);
    /* 开发者选项：在关于页最下方挂入口行（本页最后一行），同样懒加载 */
    lazy_row_create(about_page, LV_SYMBOL_WARNING, "开发者选项",
                    NULL, dev_page_build);

    s_about_page = about_page;
    ESP_LOGI(TAG, "about page built");
    return about_page;
}

/* 8. 重新扫描子页：确认（是=停止播放+数据无效化+重启重新扫描；否=返回上一级） */
static lv_obj_t *rescan_page_build(void)
{
    if (s_rescan_page != NULL) {
        return s_rescan_page;
    }
    lv_obj_t *rescan_page = lv_menu_page_create(s_menu, (char *)"重新扫描");
    page_style_apply(rescan_page);
    menu_note_row_create(rescan_page,
        "将重新扫描 SD 卡中的音乐：停止播放、"
        "使已存播放列表数据无效并重启，"
        "开机后需要重新扫描。是否继续？");
    lv_obj_t *rescan_yes = menu_row_create(rescan_page, LV_SYMBOL_OK,
                                           "是，重新扫描", NULL);
    lv_obj_add_flag(rescan_yes, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rescan_yes, rescan_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rescan_no = menu_row_create(rescan_page, LV_SYMBOL_CLOSE,
                                          "否，取消", NULL);
    lv_obj_add_flag(rescan_no, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rescan_no, rescan_cancel_cb, LV_EVENT_CLICKED, NULL);
    s_rescan_page = rescan_page;
    ESP_LOGI(TAG, "rescan page built");
    return rescan_page;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

/* 构建菜单外壳（背景层 + lv_menu + header + 根页面），**仅**由首次
   settings_menu_show() 调用（懒加载，见文件头与 settings_menu_show 注释）。
   各子页不在这里构建，由 lazy_row_create 的行在首次点进时构建。 */
static void settings_menu_shell_create(void)
{
    if (s_menu != NULL) {
        return;
    }

    /* 全屏黑色背景层：盖住顶层状态栏（边缘露出的区域显示纯黑），
       同时接收"面板外"的点击（边缘点击 = 返回上一级/关闭菜单）。
       ⚠ 本层必须比状态栏/通知栏**晚**创建才能盖住它们：现在改为首次打开
       菜单时创建，天然晚于它们（也晚于 ClickWheel），层级关系不变 */
    s_menu_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_menu_bg, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_obj_set_pos(s_menu_bg, 0, 0);
    lv_obj_set_style_bg_color(s_menu_bg, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_menu_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_menu_bg, 0, 0);
    lv_obj_set_style_radius(s_menu_bg, 0, 0);
    lv_obj_set_style_pad_all(s_menu_bg, 0, 0);
    lv_obj_clear_flag(s_menu_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_menu_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_menu_bg, menu_bg_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_menu_bg, menu_bg_click_cb, LV_EVENT_CLICKED, NULL);

    /* lv_menu 面板：全屏背景层的子对象（相对它偏移左右/上下边距），
       四周留边距，圆角 + 深灰细边框勾勒面板，方便点击面板边缘的返回按钮。
       默认隐藏，由 show 显示 */
    s_menu = lv_menu_create(s_menu_bg);
    lv_obj_set_size(s_menu, MENU_W, MENU_H);
    lv_obj_set_pos(s_menu, MENU_MARGIN_HOR, MENU_MARGIN_VER);
    lv_obj_set_style_bg_color(s_menu, MENU_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu, 24, 0);          /* 面板圆角 */
    lv_obj_set_style_border_width(s_menu, 1, 0);     /* 深灰细边框 */
    lv_obj_set_style_border_color(s_menu, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_menu, 0, 0);
    lv_obj_set_scrollbar_mode(s_menu, LV_SCROLLBAR_MODE_OFF);

    /* 根页面也显示返回按钮（= 关闭菜单），子页面返回按钮 = 返回上级
       （v9 改名：lv_menu_set_mode_root_back_btn → ..._button，枚举也由
        LV_MENU_ROOT_BACK_BTN_* 改为 LV_MENU_ROOT_BACK_BUTTON_*） */
    lv_menu_set_mode_root_back_button(s_menu, LV_MENU_ROOT_BACK_BUTTON_ENABLED);

    /* 构建页面 */
    root_page_build();

    /* header 暗色化（返回按钮 + 标题） */
    header_style_apply(s_menu);

    /* 根页面返回按钮：点击 = 关闭菜单（v9: ..._back_btn → ..._back_button） */
    lv_obj_t *back = lv_menu_get_main_header_back_button(s_menu);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 初始显示根页面 */
    lv_menu_set_page(s_menu, s_root_page);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_menu_bg, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "settings menu shell created (lv_menu, dark theme, "
                  "margin h=%d v=%d)", MENU_MARGIN_HOR, MENU_MARGIN_VER);
}

void settings_menu_show(void)
{
    /* 懒加载：整个菜单不再开机常驻（见文件头"页面构建"注释）。首次打开时
       在这里建"外壳 + 根页面"（实测 44ms），各子页在首次点进时才建——开机
       因此省下约 29KB 内部 RAM（= DMA 池）与约 305ms 的主界面等待。 */
    if (s_menu == NULL) {
        int64_t t0 = esp_timer_get_time();
        settings_menu_shell_create();
        ESP_LOGI(TAG, "settings menu created on first open (%d ms)",
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    if (s_menu == NULL) {
        return;   /* 创建失败（理论上不会；LVGL 分配失败会直接 assert） */
    }
    /* 隐藏主界面（lv_tabview）：底层页面（尤其播放页 460x460 封面大图）
       不再参与重绘，消除菜单滚动时被底层大图拖慢的卡顿。
       菜单本身是全屏独立覆盖层，隐藏主界面不影响其显示 */
    main_ui_set_visible(false);
    /* 隐藏 ClickWheel 按钮：它与本菜单同在屏幕层、但创建更早（见
       core/ui/click_wheel.h），菜单的 s_menu_bg 虽盖住它，按钮仍会拦截
       该区域的触摸，故显式隐藏 */
    click_wheel_set_overlay(true);

    /* 重置到根页面：仅当当前不在根页面时才清历史并切回。
       ⚠ 不能无条件 clear_history + set_page(root)：main_page 已是 root 时
       lv_menu_set_page 会因"相同页面"guard 直接返回，历史被清成 0，
       导致从根进入子页时 cur_depth=1、返回按钮被误判为根（点击变成
       关闭菜单而非返回）。保持在根时不动，cur_depth 维持 >=1。 */
    if (lv_menu_get_cur_main_page(s_menu) != s_root_page) {
        lv_menu_clear_history(s_menu);
        lv_menu_set_page(s_menu, s_root_page);
    }
    menu_refresh_root();
    /* 重新扫描行的歌曲总数：整个会话只在首次显示菜单时统计一次
       （见 rescan_count_refresh_once 注释） */
    rescan_count_refresh_once();

    lv_obj_clear_flag(s_menu_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    ESP_LOGI(TAG, "settings menu shown");
}

void settings_menu_hide(void)
{
    if (s_menu == NULL) {
        return;
    }
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_menu_bg, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    /* 恢复主界面显示（页面状态/滚动位置全部保留）+ 恢复顶层 ClickWheel */
    main_ui_set_visible(true);
    click_wheel_set_overlay(false);
    ESP_LOGI(TAG, "settings menu hidden");
}

bool settings_menu_is_visible(void)
{
    return s_visible;
}
