#include "ui/dir_list.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/player/player_controller.h"
#include "core/storage/playlist.h"
#include "core/ui/virtual_list.h"
#include "ui/list_common.h"
#include "ui/main_ui.h"
#include "ui/playlist_page.h"
#include "ui/player_ticker.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "dir_list";

/* 页面为黑底，目录/歌单列表放在一张深灰卡片上（上圆下直、左右
   UI_CARD_MARGIN_X px 黑边，见 ui_card_bg_create）。卡片顶部是 /sdcard
   标题栏（与歌曲列表页同款头部），下方为虚拟列表 */
#define DIR_CARD_X        UI_CARD_MARGIN_X
#define DIR_CARD_Y        UI_STATUS_BAR_H
#define DIR_CARD_W        (EXAMPLE_LCD_H_RES - 2 * UI_CARD_MARGIN_X)
#define DIR_CARD_H        (EXAMPLE_LCD_V_RES - UI_STATUS_BAR_H)
#define DIR_CORNER        30                 /* 卡片顶部圆角半径 */

/* 虚拟列表位置与尺寸：卡片内、标题栏下方、贴卡片底部（底部直角） */
#define DIR_LIST_X        UI_CARD_MARGIN_X
#define DIR_LIST_Y        (UI_STATUS_BAR_H + UI_TITLE_BAR_H)
#define DIR_LIST_H        (DIR_CARD_H - UI_TITLE_BAR_H)

/* 目录行固定高度：完整字体 line_height=33，内容区需容纳（item_height-12≥33），
   故取 52（见 VIRTUAL_LIST_USAGE.md 踩坑 5） */
#define DIR_ITEM_H        52

/* 等待播放器就绪的轮询周期（ms）：UI 创建早于 player_controller_init */
#define DIR_REFRESH_MS    300

static lv_obj_t   *s_page        = NULL;  /* tab 页容器 */
static lv_obj_t   *s_title_bar   = NULL;  /* 顶部标题栏（SD 卡图标 + /sdcard + 数量） */
static lv_obj_t   *s_list        = NULL;  /* 虚拟列表句柄 */
static lv_obj_t   *s_loading     = NULL;  /* 加载中提示 label */
static lv_timer_t *s_ready_timer = NULL;  /* 就绪轮询 timer */
static uint32_t   s_last_dir     = 0xFFFFFFFFu; /* 上次刷新时的当前播放目录下标 */

/* 前向声明 */
static void dir_list_build(void);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/* 判断目录/歌单条目是否以 .pl 结尾（只看名字，不区分大小写；若有文件夹
   恰叫 XXX.pl 也按歌单图标显示，可接受）。行图标据此选择。 */
static bool dir_name_is_pl(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3 && strcasecmp(name + len - 3, ".pl") == 0);
}

/* ------------------------------------------------------------------ */
/* 虚拟列表回调                                                          */
/* ------------------------------------------------------------------ */

/* 绑定回调：左侧图标（.pl → 列表图标，否则目录图标）+ 名称；
   当前播放目录文字/图标黄色高亮，其余白色 */
static void dir_bind_cb(virtual_list_cell_t *cell, uint32_t index,
                        const void *item, void *user_data)
{
    (void)user_data;
    const playlist_dir_entry_t *dir = (const playlist_dir_entry_t *)item;
    const char *name = playlist_get_string(dir->name_str);
    lv_label_set_text(cell->text, name);
    lv_label_set_text(cell->icon,
                      dir_name_is_pl(name) ? LV_SYMBOL_LIST : LV_SYMBOL_DIRECTORY);

    player_controller_status_t st;
    player_controller_get_status(&st);
    bool is_cur = st.has_song && (st.cur_dir == index);
    lv_color_t color = is_cur ? lv_palette_main(LV_PALETTE_YELLOW)
                              : lv_color_white();
    lv_obj_set_style_text_color(cell->text, color, 0);
    lv_obj_set_style_text_color(cell->icon, color, 0);
}

/* 选中回调：点击目录 → 播放列表页更新为该目录，并切换到播放列表页查看 */
static void dir_selected_cb(uint32_t index, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "dir selected: %u", (unsigned)index);
    playlist_page_show_dir(index);
    main_ui_switch_tab_anim(1);   /* 带动画切到播放列表页（tab 1） */
}

/* 目录变化刷新（共享 player_ticker 周期调用）：正在播放的目录切换后，把
   可见行全部重绑一次（notifyDataSetChanged 语义），让 dir_bind_cb 重新
   判定"正在播放"的黄色高亮。不重建列表、不滚动位置。 */
static void dir_track_cb(const player_controller_status_t *st, void *ctx)
{
    (void)ctx;
    if (s_list == NULL || !st->has_song) {
        return;   /* 无列表或无播放：无需刷新 */
    }
    if (st->cur_dir != s_last_dir) {
        s_last_dir = st->cur_dir;
        virtual_list_refresh(s_list);
    }
}

/* ------------------------------------------------------------------ */
/* 构建与就绪等待                                                        */
/* ------------------------------------------------------------------ */

/* 播放器就绪后构建目录虚拟列表（playlist 数据已加载） */
static void dir_list_build(void)
{
    /* 删除加载中提示 */
    if (s_loading != NULL) {
        lv_obj_del(s_loading);
        s_loading = NULL;
    }

    uint32_t count = playlist_dir_count();
    const playlist_dir_entry_t *dirs =
        (count > 0) ? playlist_dir_at(0) : NULL;

    /* 标题栏右侧显示列表数量（目录 + 歌单条目总数） */
    char sub[32];
    snprintf(sub, sizeof(sub), "%u 项", (unsigned)count);
    ui_title_bar_set_text(s_title_bar, NULL, sub);

    s_list = virtual_list_create(s_page, DIR_CARD_W, DIR_LIST_H, DIR_ITEM_H,
                                 count, dirs, sizeof(playlist_dir_entry_t),
                                 dir_bind_cb, NULL);
    if (s_list == NULL) {
        ESP_LOGE(TAG, "create dir virtual list failed");
        return;
    }
    lv_obj_set_pos(s_list, DIR_LIST_X, DIR_LIST_Y);
    virtual_list_set_selected_cb(s_list, dir_selected_cb);

    /* 定位到当前播放目录（进入界面即能看到正在播放的列表；
       "正在播放"由绑定回调以文字黄色标注，选中状态仅内部记录） */
    player_controller_status_t st;
    player_controller_get_status(&st);
    if (st.has_song) {
        virtual_list_scroll_to_index(s_list, st.cur_dir, LV_ANIM_OFF);
        virtual_list_set_selected(s_list, (int32_t)st.cur_dir);
    }

    /* 记录当前播放目录：目录变化轮询以此对比（构建时同步，避免首轮误刷新） */
    s_last_dir = st.has_song ? st.cur_dir : 0xFFFFFFFFu;

    ESP_LOGI(TAG, "dir list built: %u dirs", (unsigned)count);
}

/* 等待播放器就绪：UI 创建时 player_controller_init 尚未执行（boot_flow
   在 app_ui_create 之后调用），playlist 数据未加载，需轮询等待 */
static void ready_timer_cb(lv_timer_t *timer)
{
    if (!player_controller_is_ready()) {
        return;
    }
    lv_timer_del(timer);
    s_ready_timer = NULL;
    dir_list_build();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

lv_obj_t *dir_list_create(lv_obj_t *tv)
{
    s_page = lv_tabview_add_tab(tv, "");
    ui_page_style(s_page);

    s_list = NULL;
    s_loading = NULL;

    /* 深灰卡片背景（上圆下直，左右 10px 黑边），标题栏/列表叠于其上 */
    ui_card_bg_create(s_page, DIR_CARD_X, DIR_CARD_Y, DIR_CARD_W, DIR_CARD_H,
                      DIR_CORNER);

    /* 头部与歌曲列表页同款：左侧固定 SD 卡图标 + /sdcard（根目录），
       右侧数量在列表构建后填入 */
    s_title_bar = ui_title_bar_create(s_page, "/sdcard", NULL);
    lv_obj_set_size(s_title_bar, DIR_CARD_W, UI_TITLE_BAR_H);
    lv_obj_set_pos(s_title_bar, DIR_CARD_X, DIR_CARD_Y);
    ui_title_bar_set_icon(s_title_bar, LV_SYMBOL_SD_CARD);

    if (!player_controller_is_ready()) {
        /* 未就绪：显示加载提示，就绪后由 timer 构建列表 */
        s_loading = lv_label_create(s_page);
        lv_label_set_text(s_loading, "正在加载音乐目录...");
        lv_obj_set_style_text_font(s_loading, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(s_loading, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_align(s_loading, LV_ALIGN_CENTER, 0, UI_STATUS_BAR_H / 2);
        s_ready_timer = lv_timer_create(ready_timer_cb, DIR_REFRESH_MS, NULL);
    } else {
        dir_list_build();
    }

    /* 订阅共享 player_ticker：目录变化时重绑可见行更新"正在播放"高亮 */
    player_ticker_subscribe(dir_track_cb, NULL);

    ESP_LOGI(TAG, "dir list page created");
    return s_page;
}

void dir_list_set_marquee(bool enable)
{
    if (s_list != NULL) {
        virtual_list_set_marquee(s_list, enable);
    }
}
