#include "ui/playlist_page.h"

#include <stdio.h>
#include <stdint.h>

#include "core/fonts/system_font.h"
#include "core/hardware/display.h"
#include "core/player/player_controller.h"
#include "core/storage/playlist.h"
#include "core/ui/virtual_list.h"
#include "ui/list_common.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "playlist_page";

/* 页面容器撑满全屏（由 main_ui 的 tabview 决定），内容需避开顶部状态栏区 */
#define PLIST_STATUS_Y    UI_STATUS_BAR_H                     /* 标题栏顶部 y */
#define PLIST_PAGE_W      EXAMPLE_LCD_H_RES
#define PLIST_PAGE_H      (EXAMPLE_LCD_V_RES - UI_STATUS_BAR_H) /* 内容区总高 */

/* 歌曲行固定高度：完整字体 line_height=33，内容区 44px 足够（56-12=44，
   见 VIRTUAL_LIST_USAGE.md 踩坑 5） */
#define PLIST_ITEM_H      56

/* 等待播放器就绪的轮询周期（ms） */
#define PLIST_REFRESH_MS  300

static lv_obj_t   *s_page        = NULL;  /* tab 页容器 */
static lv_obj_t   *s_list        = NULL;  /* 虚拟列表句柄 */
static lv_obj_t   *s_title_bar   = NULL;  /* 标题栏（目录名 + 歌曲数） */
static lv_obj_t   *s_loading     = NULL;  /* 加载中/空状态提示 label */
static lv_timer_t *s_ready_timer = NULL;  /* 就绪轮询 timer */
static uint32_t   s_cur_dir      = 0xFFFFFFFFu; /* 当前展示的目录下标 */

/* ------------------------------------------------------------------ */
/* 虚拟列表回调                                                          */
/* ------------------------------------------------------------------ */

/* 绑定回调：设置歌曲文件名；当前播放歌曲文字黄色高亮，其余白色。
   user_data = 该列表第一首歌的全局下标（song_start），用于判断当前播放 */
static void song_bind_cb(lv_obj_t *cell, uint32_t index,
                         const void *item, void *user_data)
{
    const playlist_song_entry_t *song = (const playlist_song_entry_t *)item;
    lv_label_set_text(cell, playlist_get_string(song->name_str));

    uint32_t song_start = (uint32_t)(uintptr_t)user_data;
    player_controller_status_t st;
    player_controller_get_status(&st);
    bool is_cur = st.has_song && (st.cur_global == song_start + index);
    lv_obj_set_style_text_color(cell,
        is_cur ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
}

/* 选中回调：点击歌曲 → 直接播放该曲（全局下标） */
static void song_selected_cb(uint32_t index, void *user_data)
{
    uint32_t song_start = (uint32_t)(uintptr_t)user_data;
    uint32_t global = song_start + index;
    ESP_LOGI(TAG, "play song: index=%u global=%u",
             (unsigned)index, (unsigned)global);
    player_controller_play_song(global);
}

/* ------------------------------------------------------------------ */
/* 内部工具                                                             */
/* ------------------------------------------------------------------ */

/* 删除当前列表与提示 label（重建前清理） */
static void playlist_page_clear_views(void)
{
    if (s_list != NULL) {
        lv_obj_del(s_list);
        s_list = NULL;
    }
    if (s_loading != NULL) {
        lv_obj_del(s_loading);
        s_loading = NULL;
    }
}

/* 在页面中央显示一条灰色提示（加载中/空状态） */
static void playlist_page_show_hint(const char *text)
{
    if (s_loading != NULL) {
        lv_label_set_text(s_loading, text);
        return;
    }
    s_loading = lv_label_create(s_page);
    lv_label_set_text(s_loading, text);
    lv_obj_set_style_text_font(s_loading, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(s_loading, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(s_loading, LV_ALIGN_CENTER, 0, 20);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

void playlist_page_show_dir(uint32_t dir_index)
{
    if (s_page == NULL) {
        return;
    }
    const playlist_dir_entry_t *dir = playlist_dir_at(dir_index);
    if (dir == NULL) {
        ESP_LOGW(TAG, "show dir: index %u out of range", (unsigned)dir_index);
        return;
    }

    s_cur_dir = dir_index;
    playlist_page_clear_views();

    /* 标题：目录名 + 歌曲数 */
    char title[160];
    snprintf(title, sizeof(title), "%s", playlist_get_string(dir->name_str));
    char sub[32];
    snprintf(sub, sizeof(sub), "%u 首", (unsigned)dir->song_count);
    ui_title_bar_set_text(s_title_bar, title, sub);

    if (dir->song_count == 0) {
        playlist_page_show_hint("该目录暂无歌曲");
        ESP_LOGI(TAG, "playlist page: dir=%u empty", (unsigned)dir_index);
        return;
    }

    /* 歌曲数据：8 字节条目，首地址 = 该目录第一首歌 */
    uint32_t song_start = dir->song_start;
    const playlist_song_entry_t *songs = playlist_song_at(song_start);
    s_list = virtual_list_create(s_page, PLIST_PAGE_W,
                                 PLIST_PAGE_H - UI_TITLE_BAR_H, PLIST_ITEM_H,
                                 dir->song_count, songs,
                                 sizeof(playlist_song_entry_t),
                                 song_bind_cb, (void *)(uintptr_t)song_start);
    if (s_list == NULL) {
        ESP_LOGE(TAG, "create song virtual list failed");
        return;
    }
    lv_obj_set_pos(s_list, 0, PLIST_STATUS_Y + UI_TITLE_BAR_H);
    virtual_list_set_selected_cb(s_list, song_selected_cb);

    /* 若展示的正是正在播放的目录：定位并高亮当前歌曲 */
    player_controller_status_t st;
    player_controller_get_status(&st);
    if (st.has_song && st.cur_dir == dir_index) {
        virtual_list_scroll_to_index(s_list, st.cur_song_in_dir, LV_ANIM_OFF);
        virtual_list_set_selected(s_list, (int32_t)st.cur_song_in_dir);
    }

    ESP_LOGI(TAG, "playlist page show dir=%u (%u songs)",
             (unsigned)dir_index, (unsigned)dir->song_count);
}

void playlist_page_show_current(void)
{
    player_controller_status_t st;
    player_controller_get_status(&st);
    if (st.has_song) {
        playlist_page_show_dir(st.cur_dir);
    } else {
        /* 无播放：清空并提示 */
        s_cur_dir = 0xFFFFFFFFu;
        playlist_page_clear_views();
        ui_title_bar_set_text(s_title_bar, "播放列表", NULL);
        playlist_page_show_hint("暂无播放");
        ESP_LOGI(TAG, "playlist page: no song playing");
    }
}

/* 播放器就绪后默认显示正在播放的目录（开机初始内容） */
static void ready_timer_cb(lv_timer_t *timer)
{
    if (!player_controller_is_ready()) {
        return;
    }
    lv_timer_del(timer);
    s_ready_timer = NULL;
    playlist_page_show_current();
}

lv_obj_t *playlist_page_create(lv_obj_t *tv)
{
    s_page = lv_tabview_add_tab(tv, "");
    ui_page_style(s_page);

    s_title_bar = ui_title_bar_create(s_page, "播放列表", NULL);
    /* 标题栏整体下移避开顶部状态栏区（ui_title_bar_create 内部默认 y=0） */
    lv_obj_set_pos(s_title_bar, 0, PLIST_STATUS_Y);
    s_list = NULL;
    s_loading = NULL;

    if (!player_controller_is_ready()) {
        /* 未就绪：显示加载提示，就绪后显示正在播放的目录 */
        playlist_page_show_hint("正在加载播放列表...");
        s_ready_timer = lv_timer_create(ready_timer_cb, PLIST_REFRESH_MS, NULL);
    } else {
        playlist_page_show_current();
    }

    ESP_LOGI(TAG, "playlist page created");
    return s_page;
}

void playlist_page_set_marquee(bool enable)
{
    if (s_list != NULL) {
        virtual_list_set_marquee(s_list, enable);
    }
}
