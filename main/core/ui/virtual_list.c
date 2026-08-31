#include "core/ui/virtual_list.h"

#include <string.h>

#include "core/fonts/system_font.h"
#include "esp_log.h"

static const char *TAG = "vlist";

/* 选中条目的背景色（深蓝灰）：叠加在绑定回调设置的文字颜色之上，
   只作为选中高亮，不覆盖文字颜色，便于与"当前播放中"的文字变色区分 */
#define VLIST_SELECTED_BG   lv_palette_darken(LV_PALETTE_BLUE, 3)

/* 条目文字左右内边距（右侧留白避免被原生滚动条压住） */
#define VLIST_PAD_H         16

/* 条目文字上下内边距（用于在固定高度的 cell 里视觉上垂直居中） */
#define VLIST_PAD_V         6

/* 未绑定条目的哨兵值（cell 当前无对应数据时） */
#define VLIST_NO_INDEX      0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* 状态结构体：保存列表全部运行状态，通过 lv_obj 的 user_data 挂载      */
/* ------------------------------------------------------------------ */
typedef struct {
    const void *data;              /* 条目数据首地址 */
    uint32_t    item_size;         /* 单条数据字节数 */
    uint32_t    item_count;        /* 条目总数 */
    lv_coord_t  item_height;       /* 每条固定高度 */

    virtual_list_bind_cb_t     bind_cb;     /* 条目绑定回调 */
    virtual_list_selected_cb_t selected_cb; /* 选中回调 */
    void *user_data;                        /* 传给回调的上下文 */

    lv_obj_t *placeholder;      /* 透明占位对象：撑开真实内容高度驱动滚动条 */
    lv_obj_t **cells;           /* 复用的 cell 数组（lv_label） */
    uint32_t  *cell_indices;    /* 每个 cell 当前绑定的条目下标 */
    uint32_t   cell_count;      /* cell 数量 = 可见数 + 2 缓冲 */
    int32_t    selected;        /* 选中条目下标（-1 = 无选中） */
    bool       marquee_enable;  /* 是否允许超长文本跑马灯（默认开启） */
    bool       scrolling;       /* 当前是否正在滚动（滚动中临时禁用跑马灯） */
} virtual_list_state_t;

/* 从列表对象取状态结构体（创建时挂到 user_data 上） */
static virtual_list_state_t *vlist_get_state(lv_obj_t *list)
{
    return (virtual_list_state_t *)lv_obj_get_user_data(list);
}

/* 占位对象点击穿透：不拦截其上层 cell 的点击（纯视觉对象惯例） */
static void vlist_hit_test_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    info->res = false;
}

/* 仅更新某个 cell 的选中背景（不碰文本/动画，供选中变化时单独刷新） */
static void vlist_apply_selected_style(virtual_list_state_t *st,
                                       lv_obj_t *cell, uint32_t index)
{
    if ((int32_t)index == st->selected) {
        lv_obj_set_style_bg_color(cell, VLIST_SELECTED_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    }
}

/* 把跑马灯开关应用到所有 cell 的长文本模式：
   开启且非滚动中 → 循环滚动；否则（滚动中/被关闭）→ 裁剪 */
static void vlist_apply_marquee(virtual_list_state_t *st)
{
    lv_label_long_mode_t mode = (st->marquee_enable && !st->scrolling)
        ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
    for (uint32_t i = 0; i < st->cell_count; i++) {
        lv_label_set_long_mode(st->cells[i], mode);
    }
}

/* 把某个 cell 绑定到指定条目：先让用户回调填文本/颜色，再叠加选中高亮 */
static void vlist_bind_cell(virtual_list_state_t *st, lv_obj_t *cell,
                            uint32_t index)
{
    const uint8_t *item = (const uint8_t *)st->data +
                          (size_t)index * st->item_size;
    st->bind_cb(cell, index, item, st->user_data);
    vlist_apply_selected_style(st, cell, index);
}

/* 根据当前滚动位置重新绑定 cell（虚拟滚动核心）：
   第一个可见条目 = scroll_y / item_height，随后每个 cell 顺延绑定下一个条目。
   cell 的位置放在**内容坐标系**中该条目的真实位置（index * item_height），
   这样列表滚动时 LVGL 会整体平移内容，cell 自然随动，视口连续无空白。
   force=true 强制全量重绑（数据刷新用）；false 跳过下标未变的 cell（滚动用，
   避免重复赋值导致 label 跑马灯动画被重置）。 */
static void vlist_rebind(virtual_list_state_t *st, lv_obj_t *list, bool force)
{
    lv_coord_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) {
        scroll_y = 0;
    }

    uint32_t first = (uint32_t)scroll_y / (uint32_t)st->item_height;

    for (uint32_t i = 0; i < st->cell_count; i++) {
        uint32_t  index = first + i;
        lv_obj_t *cell  = st->cells[i];

        if (index >= st->item_count) {
            /* 已超出数据范围：隐藏该 cell */
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
            st->cell_indices[i] = VLIST_NO_INDEX;
            continue;
        }

        lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);

        /* ID 判断：下标未变且非强制刷新时跳过（数据静态，ID 相同内容不变） */
        if (!force && st->cell_indices[i] == index) {
            continue;
        }

        lv_obj_set_pos(cell, 0, (lv_coord_t)((size_t)index * st->item_height));
        st->cell_indices[i] = index;
        vlist_bind_cell(st, cell, index);
    }
}

/* 滚动开始：临时禁用跑马灯（滚动中 cell 会复用，跑马灯从头滚是必然的，
   滚动期间先裁剪，停止后再恢复，避免视觉跳动） */
static void vlist_scroll_begin_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    virtual_list_state_t *st = vlist_get_state(list);
    st->scrolling = true;
    vlist_apply_marquee(st);
}

/* 滚动结束（含惯性停止）：恢复跑马灯 */
static void vlist_scroll_end_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    virtual_list_state_t *st = vlist_get_state(list);
    st->scrolling = false;
    vlist_apply_marquee(st);
}

/* 滚动事件：滚动/惯性/scroll_to 动画过程中持续触发，实时重绑定 */
static void vlist_scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    vlist_rebind(vlist_get_state(list), list, false);
}

/* 点击事件（由 cell 冒泡到列表）：找出被点的 cell 对应的条目并选中 */
static void vlist_click_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    lv_obj_t *cell = lv_event_get_target(e);
    virtual_list_state_t *st = vlist_get_state(list);

    for (uint32_t i = 0; i < st->cell_count; i++) {
        if (st->cells[i] != cell) {
            continue;
        }
        uint32_t idx = st->cell_indices[i];
        if (idx == VLIST_NO_INDEX) {
            break;
        }
        virtual_list_set_selected(list, (int32_t)idx);
        if (st->selected_cb != NULL) {
            st->selected_cb(idx, st->user_data);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

lv_obj_t *virtual_list_create(lv_obj_t *parent,
                              lv_coord_t w, lv_coord_t h,
                              lv_coord_t item_height,
                              uint32_t item_count,
                              const void *data, uint32_t item_size,
                              virtual_list_bind_cb_t bind_cb,
                              void *user_data)
{
    if (item_height <= 0 || item_size == 0 || bind_cb == NULL) {
        ESP_LOGE(TAG, "invalid args: item_height=%d item_size=%u bind_cb=%p",
                 (int)item_height, (unsigned)item_size, (void *)bind_cb);
        return NULL;
    }

    /* 列表容器：可垂直滚动、原生滚动条、无边框无内边距 */
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, w, h);
    lv_obj_set_style_bg_color(list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    /* 分配并初始化状态结构体，挂到列表 user_data 上 */
    virtual_list_state_t *st = lv_mem_alloc(sizeof(*st));
    if (st == NULL) {
        ESP_LOGE(TAG, "alloc state failed");
        lv_obj_del(list);
        return NULL;
    }
    memset(st, 0, sizeof(*st));
    st->data        = data;
    st->item_size   = item_size;
    st->item_count  = item_count;
    st->item_height = item_height;
    st->bind_cb     = bind_cb;
    st->selected_cb = NULL;
    st->user_data   = user_data;
    st->selected       = -1;
    st->marquee_enable = true;   /* 默认开启跑马灯 */
    st->scrolling      = false;
    lv_obj_set_user_data(list, st);

    /* 透明占位对象：高度 = 真实内容高度，撑开 scroll 范围以驱动原生滚动条。
       （依赖 CONFIG_LV_USE_LARGE_COORD=y，否则高度会溢出 int16 坐标） */
    lv_coord_t content_h = (lv_coord_t)((size_t)item_count * (size_t)item_height);
    lv_obj_t *ph = lv_obj_create(list);
    lv_obj_set_size(ph, 1, content_h);
    lv_obj_set_pos(ph, 0, 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_pad_all(ph, 0, 0);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ph, vlist_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
    st->placeholder = ph;

    /* 创建固定数量 cell（可见数 + 2 缓冲），全部为 lv_label */
    st->cell_count = (uint32_t)((h + item_height - 1) / item_height) + 2;
    st->cells       = lv_mem_alloc(sizeof(lv_obj_t *) * st->cell_count);
    st->cell_indices = lv_mem_alloc(sizeof(uint32_t) * st->cell_count);
    if (st->cells == NULL || st->cell_indices == NULL) {
        ESP_LOGE(TAG, "alloc cells failed");
        lv_mem_free(st->cells);
        lv_mem_free(st->cell_indices);
        lv_mem_free(st);
        lv_obj_del(list);
        return NULL;
    }

    for (uint32_t i = 0; i < st->cell_count; i++) {
        lv_obj_t *cell = lv_label_create(list);
        lv_obj_set_size(cell, LV_PCT(100), item_height);
        lv_obj_set_style_text_font(cell, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(cell, lv_color_white(), 0);
        lv_obj_set_style_pad_left(cell, VLIST_PAD_H, 0);
        lv_obj_set_style_pad_right(cell, VLIST_PAD_H, 0);
        lv_obj_set_style_pad_top(cell, VLIST_PAD_V, 0);
        lv_obj_set_style_pad_bottom(cell, VLIST_PAD_V, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(cell, LV_LABEL_LONG_SCROLL_CIRCULAR); /* 超长文本跑马灯循环滚动 */
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);    /* 作为点击目标 */
        lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE); /* 让 CLICKED 冒泡到 list（LVGL 8 默认不冒泡） */
        st->cells[i]        = cell;
        st->cell_indices[i] = VLIST_NO_INDEX;
    }

    /* 事件：滚动开始/中/结束 + 点击选中 */
    lv_obj_add_event_cb(list, vlist_scroll_begin_event_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, vlist_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, vlist_scroll_end_event_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(list, vlist_click_event_cb, LV_EVENT_CLICKED, NULL);

    /* 初始绑定（渲染首屏可见条目） */
    vlist_rebind(st, list, true);

    return list;
}

void virtual_list_scroll_to_index(lv_obj_t *list, uint32_t index,
                                  lv_anim_enable_t anim)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return;
    }
    if (st->item_count == 0) {
        return;
    }
    if (index >= st->item_count) {
        index = st->item_count - 1;   /* 越界收敛到末尾 */
    }
    lv_coord_t y = (lv_coord_t)((size_t)index * (size_t)st->item_height);
    lv_obj_scroll_to_y(list, y, anim);   /* 触发 SCROLL 事件自动重绑定 */
}

void virtual_list_refresh(lv_obj_t *list)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return;
    }
    vlist_rebind(st, list, true);   /* 数据刷新：强制全量重绑 */
}

void virtual_list_set_selected(lv_obj_t *list, int32_t index)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return;
    }
    if (index >= (int32_t)st->item_count) {
        index = -1;   /* 越界当作取消选中 */
    }
    if (st->selected == index) {
        return;   /* 未变化，无需重渲染 */
    }
    st->selected = index;
    /* 只刷新可见 cell 的选中背景，不重新赋值文本（避免跑马灯动画被重置） */
    for (uint32_t i = 0; i < st->cell_count; i++) {
        uint32_t idx = st->cell_indices[i];
        if (idx != VLIST_NO_INDEX) {
            vlist_apply_selected_style(st, st->cells[i], idx);
        }
    }
}

int32_t virtual_list_get_selected(lv_obj_t *list)
{
    virtual_list_state_t *st = vlist_get_state(list);
    return (st == NULL) ? -1 : st->selected;
}

void virtual_list_set_selected_cb(lv_obj_t *list,
                                  virtual_list_selected_cb_t cb)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return;
    }
    st->selected_cb = cb;
}

void virtual_list_set_marquee(lv_obj_t *list, bool enable)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return;
    }
    if (st->marquee_enable == enable) {
        return;
    }
    st->marquee_enable = enable;
    vlist_apply_marquee(st);   /* 立即切换所有 cell 的长文本模式 */
}

uint32_t virtual_list_get_first_index(lv_obj_t *list)
{
    virtual_list_state_t *st = vlist_get_state(list);
    if (st == NULL) {
        return 0;
    }
    lv_coord_t sy = lv_obj_get_scroll_y(list);
    if (sy < 0) {
        sy = 0;
    }
    return (uint32_t)sy / (uint32_t)st->item_height;
}
