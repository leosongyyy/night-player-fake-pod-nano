#include "ui/dir_list.h"

#include <stdio.h>
#include <stdint.h>

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

/* 页面容器撑满全屏（由 main_ui 的 tabview 决定），内容需避开顶部状态栏区 */
#define DIR_PAGE_W        EXAMPLE_LCD_H_RES
#define DIR_PAGE_H        (EXAMPLE_LCD_V_RES - UI_STATUS_BAR_H) /* 列表高度 */
#define DIR_LIST_Y        UI_STATUS_BAR_H                       /* 列表顶部 y */

/* 目录行固定高度：完整字体 line_height=33，内容区需容纳（item_height-12≥33），
   故取 52（见 VIRTUAL_LIST_USAGE.md 踩坑 5） */
#define DIR_ITEM_H      52

/* 等待播放器就绪的轮询周期（ms）：UI 创建早于 player_controller_init */
#define DIR_REFRESH_MS  300

static lv_obj_t   *s_page        = NULL;  /* tab 页容器 */
static lv_obj_t   *s_list        = NULL;  /* 虚拟列表句柄 */
static lv_obj_t   *s_loading     = NULL;  /* 加载中提示 label */
static lv_timer_t *s_ready_timer = NULL;  /* 就绪轮询 timer */
static uint32_t   s_last_dir     = 0xFFFFFFFFu; /* 上次刷新时的当前播放目录下标 */

/* 前向声明 */
static void dir_list_build(void);

/* ------------------------------------------------------------------ */
/* 虚拟列表回调                                                          */
/* ------------------------------------------------------------------ */

/* 绑定回调：设置目录名文本；当前播放目录文字黄色高亮，其余白色 */
static void dir_bind_cb(lv_obj_t *cell, uint32_t index,
                        const void *item, void *user_data)
{
    (void)user_data;
    const playlist_dir_entry_t *dir = (const playlist_dir_entry_t *)item;
    lv_label_set_text(cell, playlist_get_string(dir->name_str));

    player_controller_status_t st;
    player_controller_get_status(&st);
    bool is_cur = st.has_song && (st.cur_dir == index);
    lv_obj_set_style_text_color(cell,
        is_cur ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
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
   可见 cell 全部重绑一次（notifyDataSetChanged 语义），让 dir_bind_cb 重新
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
    s_list = virtual_list_create(s_page, DIR_PAGE_W, DIR_PAGE_H, DIR_ITEM_H,
                                 count, dirs, sizeof(playlist_dir_entry_t),
                                 dir_bind_cb, NULL);
    if (s_list == NULL) {
        ESP_LOGE(TAG, "create dir virtual list failed");
        return;
    }
    lv_obj_set_pos(s_list, 0, DIR_LIST_Y);
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

    if (!player_controller_is_ready()) {
        /* 未就绪：显示加载提示，就绪后由 timer 构建列表 */
        s_loading = lv_label_create(s_page);
        lv_label_set_text(s_loading, "正在加载音乐目录...");
        lv_obj_set_style_text_font(s_loading, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(s_loading, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_align(s_loading, LV_ALIGN_CENTER, 0, 0);
        s_ready_timer = lv_timer_create(ready_timer_cb, DIR_REFRESH_MS, NULL);
    } else {
        dir_list_build();
    }

    /* 订阅共享 player_ticker：目录变化时重绑可见 cell 更新"正在播放"高亮 */
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
