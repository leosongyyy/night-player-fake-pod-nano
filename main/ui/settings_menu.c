#include "ui/settings_menu.h"

#include "core/fonts/system_font.h"
#include "core/hardware/audio/mp3_player.h"
#include "core/hardware/display.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist_store.h"
#include "core/ui/lvgl_port.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
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
static lv_obj_t *s_sleep_def_on_status = NULL;   /* 睡眠定时器（开机默认开启） */
static lv_obj_t *s_awake_status        = NULL;   /* 清醒感知 */
static lv_obj_t *s_screen_off_status   = NULL;   /* 屏幕自动息屏（当前档位） */

/* 息屏档位子页的行对象（选中项高亮刷新用） */
static lv_obj_t *s_screen_off_rows[SCREEN_OFF_STEP_COUNT] = { NULL };

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
        if (lv_obj_check_type(child, &lv_btn_class)) {
            lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(child, 0, 0);
            lv_obj_set_style_pad_all(child, 8, 0);
            /* 按钮内图标（lv_img 渲染的符号）染白 */
            uint32_t icnt = lv_obj_get_child_cnt(child);
            for (uint32_t j = 0; j < icnt; j++) {
                lv_obj_t *icon = lv_obj_get_child(child, j);
                if (lv_obj_check_type(icon, &lv_img_class)) {
                    lv_obj_set_style_img_recolor(icon, TEXT_COLOR, 0);
                    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
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

/* 刷新根页面各行的状态文字（打开菜单/开关切换后调用） */
static void menu_refresh_root(void)
{
    if (s_sleep_def_on_status != NULL) {
        lv_label_set_text(s_sleep_def_on_status,
                          config_store_get_sleep_def_on() ? "开" : "关");
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

/* 睡眠定时器（开机默认开启）开关：点击切换并持久化 */
static void sleep_def_on_row_cb(lv_event_t *e)
{
    (void)e;
    bool on = !config_store_get_sleep_def_on();
    config_store_set_sleep_def_on(on);
    ESP_LOGI(TAG, "sleep def on -> %d", (int)on);
    menu_refresh_root();
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
    if (lv_menu_back_btn_is_root(s_menu, btn)) {
        settings_menu_hide();
    }
}

/* ------------------------------------------------------------------ */
/* 重新扫描：停止播放 → 使播放列表数据无效 → 重启（boot_flow 重新扫描）   */
/* ------------------------------------------------------------------ */

/* 重新扫描任务参数：流程含 mp3_player_stop 的等待（解码任务退出、释放
   文件句柄/I2S，最多 1s），放独立高栈任务执行，避免阻塞 LVGL/按键任务 */
#define RESCAN_TASK_STACK  (8 * 1024)
#define RESCAN_TASK_PRIO   6
#define RESCAN_TASK_CORE   1

/* 重新扫描任务：停止所有播放事务 → 释放资源（含 playlist mmap）→ 使已存
   播放列表数据无效 → 重启。重启后 boot_flow 检测到分区无效，进入"需要
   扫描音乐"界面，用户确认后重新扫描 SD 卡（完整闭环，无需本任务做扫描）。

   ⚠ 顺序不可颠倒（内存安全关键）：
   1. 先挂起 LVGL：player_ticker/列表刷新/正在播放页等都在 LVGL 任务上下文
      读 playlist 指针，不先挂起的话，playlist 卸载后它们会访问失效映射；
   2. player_controller_deinit()：停播放 → 停控制器任务 → 删队列 →
      playlist_deinit()（munmap + 清空 playlist 指针）；
   3. playlist_store_invalidate()：擦头部扇区（此刻已无任何任务访问
      playlist 数据，pldata 层也会在擦除前自动 unmap，双重保险）。 */
static void rescan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "=== rescan: stop playback, invalidate playlist, restart ===");

    /* 1. 挂起 LVGL：停止 player_ticker/列表刷新/播放页刷新等所有读
          playlist 数据的 UI 活动（菜单画面定格，马上重启无影响） */
    lvgl_port_suspend();
    ESP_LOGI(TAG, "LVGL suspended");

    /* 2. 停止播放 + 释放播放列表映射（停止控制器任务后才安全 unmap） */
    player_controller_deinit();

    /* 3. 使已存播放列表数据无效（只擦头部扇区，数据区保留）：
          重启后 playlist_store_is_valid() == false → boot_flow 提示重新扫描 */
    esp_err_t ret = playlist_store_invalidate();
    ESP_LOGI(TAG, "playlist invalidated (%s), restarting to rescan",
             esp_err_to_name(ret));

    /* 4. 等待日志刷出后重启 */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    vTaskDelete(NULL);   /* 理论不可达 */
}

/* 重新扫描确认（"是"）：创建独立任务执行停止→无效化→重启 */
static void rescan_confirm_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "rescan confirmed, start rescan task");
    if (xTaskCreatePinnedToCore(rescan_task, "rescan",
                                RESCAN_TASK_STACK, NULL,
                                RESCAN_TASK_PRIO, NULL,
                                RESCAN_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create rescan task failed");
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
    lv_obj_t *back = lv_menu_get_main_header_back_btn(s_menu);
    lv_event_send(back, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------ */
/* 页面构建                                                             */
/* ------------------------------------------------------------------ */

/* 根页面：5 个功能行 */
static void root_page_build(void)
{
    /* ⚠ lv_menu_page_create 的 title 是 char*（非 const），内部会拷贝，
       字符串字面量需强转（LVGL 8.4 的 API 签名如此） */
    s_root_page = lv_menu_page_create(s_menu, (char *)"设置");
    page_style_apply(s_root_page);

    /* 1. 睡眠定时器：子菜单 = 开机默认开启 + 清醒感知开关，
       底部放清醒感知与休眠行为说明 */
    lv_obj_t *sleep_page = lv_menu_page_create(s_menu, (char *)"睡眠定时器");
    page_style_apply(sleep_page);

    /* 开机默认开启：开关 */
    lv_obj_t *sleep_toggle = menu_row_create(sleep_page, NULL,
                                             "开机默认开启", &s_sleep_def_on_status);
    lv_obj_add_flag(sleep_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_toggle, sleep_def_on_row_cb, LV_EVENT_CLICKED, NULL);

    /* 睡眠清醒感知：开关（睡眠定时器的子功能，一并收进本子菜单） */
    lv_obj_t *awake_toggle = menu_row_create(sleep_page, NULL,
                                             "清醒感知", &s_awake_status);
    lv_obj_add_flag(awake_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(awake_toggle, awake_row_cb, LV_EVENT_CLICKED, NULL);

    /* 底部说明 1：清醒感知（用户文案）+倒计时即将结束时音量逐渐减小（NightPlayer 原版机制） */
    menu_note_row_create(sleep_page,
        "开启清醒感知后，若在睡眠定时器期间进行操作，"
        "剩余时间会按照一定比例增加。让您在入梦前音乐不断。倒计时即将结束时，音量会逐渐减小，让音乐自然淡出。");
    /* 底部说明 2：休眠行为（到点只休眠，彻底关机需上键长按 3 秒） */
    menu_note_row_create(sleep_page,
        "睡眠定时到点后，播放器只进入休眠状态（所有外设关闭，低功耗待机，同时按下两个按键既可唤醒）"
        "这是硬件的限制。如果要彻底关机省电，请在第二天早上长按3秒上键手动关机。");
    lv_obj_t *row1 = menu_row_create(s_root_page, LV_SYMBOL_POWER,
                                     "睡眠定时器", NULL);
    lv_menu_set_load_page_event(s_menu, row1, sleep_page);

    /* 2. 屏幕自动息屏：子页选择档位 */
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
    lv_obj_t *row3 = menu_row_create(s_root_page, LV_SYMBOL_EYE_CLOSE,
                                     "屏幕自动息屏", &s_screen_off_status);
    lv_menu_set_load_page_event(s_menu, row3, screen_off_page);

    /* 4. 按键帮助：子页静态说明。
       按键命名用社区通用说法：上键 = A 键（电源键），下键 = B 键；
       "关机"指长按上键进入关机流程（约 3 秒彻底断电）。 */
    lv_obj_t *help_page = lv_menu_page_create(s_menu, (char *)"按键帮助");
    page_style_apply(help_page);
    menu_text_row_create(help_page,
        "亮屏状态：\n上键 短按 音量+\n下键 短按 音量-\n"
        "上键 长按 关机\n下键 长按 音量-10\n两个按键一起按下 息屏");
    menu_text_row_create(help_page,
        "息屏状态：\n上键 短按 播放/暂停\n下键 短按 下一曲\n"
        "下键 长按 上一曲\n两个按键一起按下 亮屏");
    lv_obj_t *row4 = menu_row_create(s_root_page, LV_SYMBOL_KEYBOARD,
                                     "按键帮助", NULL);
    lv_menu_set_load_page_event(s_menu, row4, help_page);

    /* 5. 关于：子页作者信息 + 固件版本（Kconfig FIRMWARE_VERSION_STRING） */
    lv_obj_t *about_page = lv_menu_page_create(s_menu, (char *)"关于");
    page_style_apply(about_page);
    menu_text_row_create(about_page,
        "FakePodNanoNightPlayer\n基于 NightPlayer 的第三方固件");
    menu_text_row_create(about_page, "作者：ZYFDroid\nAI 协作：DeepSeek");
    menu_text_row_create(about_page, "固件版本：" CONFIG_FIRMWARE_VERSION_STRING);
    lv_obj_t *row5 = menu_row_create(s_root_page, LV_SYMBOL_SETTINGS,
                                     "关于", NULL);
    lv_menu_set_load_page_event(s_menu, row5, about_page);

    /* 6. 重新扫描：子页确认（是=停止播放+数据无效化+重启重新扫描；
       否=返回上一级） */
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
    lv_obj_t *row6 = menu_row_create(s_root_page, LV_SYMBOL_REFRESH,
                                     "重新扫描", NULL);
    lv_menu_set_load_page_event(s_menu, row6, rescan_page);

    menu_refresh_root();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

void settings_menu_create(void)
{
    if (s_menu != NULL) {
        return;
    }

    /* 全屏黑色背景层：盖住顶层状态栏（边缘露出的区域显示纯黑），
       同时接收"面板外"的点击（边缘点击 = 返回上一级/关闭菜单）。
       最后创建 → 位于屏幕最上层；默认隐藏，与面板一起 show/hide */
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

    /* 根页面也显示返回按钮（= 关闭菜单），子页面返回按钮 = 返回上级 */
    lv_menu_set_mode_root_back_btn(s_menu, LV_MENU_ROOT_BACK_BTN_ENABLED);

    /* 构建页面 */
    root_page_build();

    /* header 暗色化（返回按钮 + 标题） */
    header_style_apply(s_menu);

    /* 根页面返回按钮：点击 = 关闭菜单 */
    lv_obj_t *back = lv_menu_get_main_header_back_btn(s_menu);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 初始显示根页面 */
    lv_menu_set_page(s_menu, s_root_page);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_menu_bg, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "settings menu created (lv_menu, dark theme, margin h=%d v=%d)",
             MENU_MARGIN_HOR, MENU_MARGIN_VER);
}

void settings_menu_show(void)
{
    if (s_menu == NULL) {
        return;
    }
    /* 隐藏主界面（lv_tabview）：底层页面（尤其播放页 460x460 封面大图）
       不再参与重绘，消除菜单滚动时被底层大图拖慢的卡顿。
       菜单本身是全屏独立覆盖层，隐藏主界面不影响其显示 */
    main_ui_set_visible(false);

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
    /* 恢复主界面显示（页面状态/滚动位置全部保留） */
    main_ui_set_visible(true);
    ESP_LOGI(TAG, "settings menu hidden");
}

bool settings_menu_is_visible(void)
{
    return s_visible;
}
