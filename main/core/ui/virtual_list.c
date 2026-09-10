#include "core/ui/virtual_list.h"

#include <string.h>

#include "core/fonts/system_font.h"
#include "core/ui/global_style.h"
#include "esp_log.h"

static const char *TAG = "vlist";

/* 行布局（相对行容器，行容器占满列表可视宽）：
   - 图标列：左起 VLIST_ICON_X 处，宽约 (VLIST_TEXT_X - VLIST_ICON_X - 8)；
   - 名称文本：从 VLIST_TEXT_X 起，右侧留 VLIST_PAD_R 防原生滚动条压字 */
#define VLIST_ICON_X      16
#define VLIST_TEXT_X      50
#define VLIST_PAD_R       16

/* 行内图标用 LVGL 内置字体（系统字体不含 PUA 区 LV_SYMBOL 字形） */
#define VLIST_ICON_FONT   (&lv_font_montserrat_20)

/* 按下反馈背景色：50% 黄 + 50% 黑（0x7f7f00）。
   仅按下瞬间显示（LV_STATE_PRESSED），松开自动恢复透明，无常驻选中背景 */
#define VLIST_PRESSED_BG  0x7f7f00

/* 未绑定条目的哨兵值（行槽当前无对应数据时） */
#define VLIST_NO_INDEX    0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* 行槽结构：一行 = 容器 + 图标 label + 文本 label                     */
/* ------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *root;   /* 行容器：负责定位(index*item_height)/隐藏/点击目标 */
    lv_obj_t *icon;   /* 图标 label（固定左侧，不随文本跑马灯移动） */
    lv_obj_t *text;   /* 名称文本 label（跑马灯作用对象） */
} vlist_row_t;

/* 状态结构体：保存列表全部运行状态，通过 lv_obj 的 user_data 挂载 */
typedef struct {
    const void *data;              /* 条目数据首地址 */
    uint32_t    item_size;         /* 单条数据字节数 */
    uint32_t    item_count;        /* 条目总数 */
    lv_coord_t  item_height;       /* 每条固定高度 */

    virtual_list_bind_cb_t     bind_cb;     /* 条目绑定回调 */
    virtual_list_selected_cb_t selected_cb; /* 选中回调 */
    void *user_data;                        /* 传给回调的上下文 */

    lv_obj_t *placeholder;      /* 透明占位对象：撑开真实内容高度驱动滚动条 */
    vlist_row_t *rows;          /* 复用的行槽数组（root/icon/text） */
    uint32_t  *row_indices;     /* 每个行槽当前绑定的条目下标 */
    uint32_t   row_count;       /* 行槽数量 = 可见数 + 2 缓冲 */
    lv_coord_t viewport_h;      /* 列表可视区高度（计算视口覆盖的条目范围用） */
    int32_t    selected;        /* 选中条目下标（-1 = 无选中） */
    bool       marquee_enable;  /* 是否允许超长文本跑马灯（默认开启） */
} virtual_list_state_t;

/* 从列表对象取状态结构体（创建时挂到 user_data 上） */
static virtual_list_state_t *vlist_get_state(lv_obj_t *list)
{
    return (virtual_list_state_t *)lv_obj_get_user_data(list);
}

/* 占位对象点击穿透：不拦截其上层行的点击（纯视觉对象惯例） */
static void vlist_pass_through_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    info->res = false;
}

/* 把跑马灯开关应用到所有行槽的文本 label：
   开启 → 循环滚动（图标保持不动）；关闭 → 裁剪。
   （不再随滚动临时切换：常驻行滚动时不被重绑，跑马灯保持连续，
     只有新进入视口的行会从头开始，属正常表现）
   滚动的节奏（起始延迟/轮数/轮间停顿）统一由 core/ui/global_style 管理 */
static void vlist_apply_marquee(virtual_list_state_t *st)
{
    lv_label_long_mode_t mode = st->marquee_enable
        ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
    for (uint32_t i = 0; i < st->row_count; i++) {
        lv_label_set_long_mode(st->rows[i].text, mode);
        if (st->marquee_enable) {
            global_style_marquee_attach(st->rows[i].text);
        }
    }
}

/* 把某个行槽绑定到指定条目：把行视图（icon/text）交给用户回调填内容。
   （行槽无选中背景，"当前播放中"的标注由绑定回调用文字颜色实现）
   ⚠ 本函数**不**碰跑马灯：曲目/目录变化时上层会 virtual_list_refresh() 重绑
     全部可见行（只为刷新"正在播放"的黄色高亮，文本其实没变），若在这里无条件
     global_style_marquee_attach() 就会让整屏跑马灯一起回到起点。
     跑马灯的重启交给绑定回调里的 global_style_marquee_set_text()——它只在内容
     真的变了时才 set_text + 重跑（详见 core/ui/global_style.h） */
static void vlist_bind_row(virtual_list_state_t *st, vlist_row_t *row,
                           uint32_t index)
{
    const uint8_t *item = (const uint8_t *)st->data +
                          (size_t)index * st->item_size;
    virtual_list_cell_t cell = { .icon = row->icon, .text = row->text };
    st->bind_cb(&cell, index, item, st->user_data);
}

/* 视口槽位式重绑（虚拟滚动核心）：
   行槽锚定在**内容坐标系**中该条目的真实位置（index * item_height），滚动时
   LVGL 整体平移内容，仍留在视口内的行完全不动——位置、文本、颜色、跑马灯
   动画全部保留，一个都不重绑；
   只有滚出视口的行被回收（隐藏置空闲），重新绑定到另一端新进入视口的条目
   上。每跨一个条目高度只重绑 1 行，复用是"逐个进出"的，不是全量重绑。
   force=true 强制重绑所有视口内行（数据刷新用）。 */
static void vlist_rebind(virtual_list_state_t *st, lv_obj_t *list, bool force)
{
    lv_coord_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) {
        scroll_y = 0;
    }

    uint32_t first = (uint32_t)scroll_y / (uint32_t)st->item_height;

    /* 防御：视口已完全越过数据末尾（理论不会发生），隐藏全部行 */
    if (first >= st->item_count) {
        for (uint32_t i = 0; i < st->row_count; i++) {
            lv_obj_add_flag(st->rows[i].root, LV_OBJ_FLAG_HIDDEN);
            st->row_indices[i] = VLIST_NO_INDEX;
        }
        return;
    }

    /* 当前视口覆盖的条目范围 [first, last]（含两端部分可见的条目） */
    uint32_t last = ((uint32_t)scroll_y + (uint32_t)st->viewport_h - 1) /
                    (uint32_t)st->item_height;
    if (last >= st->item_count) {
        last = st->item_count - 1;
    }

    /* 第一步：回收滚出视口的行（隐藏并置为空闲）。
       仍在视口内的行下标不变，位置/文本/颜色/跑马灯全部保留。 */
    for (uint32_t i = 0; i < st->row_count; i++) {
        uint32_t idx = st->row_indices[i];
        if (idx != VLIST_NO_INDEX && (idx < first || idx > last)) {
            lv_obj_add_flag(st->rows[i].root, LV_OBJ_FLAG_HIDDEN);
            st->row_indices[i] = VLIST_NO_INDEX;
        }
    }

    /* 第二步：确保 [first, last] 内每个条目都有行覆盖。
       已有覆盖的行不动（force 时重绑内容）；缺的条目从空闲行中取一个，
       定位到该条目的内容坐标并绑定（row_count 很小，线性查找即可）。 */
    for (uint32_t index = first; index <= last; index++) {
        vlist_row_t *row = NULL;
        for (uint32_t i = 0; i < st->row_count; i++) {
            if (st->row_indices[i] == index) {
                row = &st->rows[i];
                break;
            }
        }

        if (row != NULL) {
            if (force) {
                /* 强制刷新：数据可能已变化，重绑内容（位置本就正确，无需 set_pos） */
                vlist_bind_row(st, row, index);
            }
            continue;
        }

        /* 无覆盖：从空闲（已回收/未使用）行中取一个 */
        for (uint32_t i = 0; i < st->row_count; i++) {
            if (st->row_indices[i] == VLIST_NO_INDEX) {
                row = &st->rows[i];
                lv_obj_clear_flag(row->root, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(row->root, 0,
                               (lv_coord_t)((size_t)index * st->item_height));
                st->row_indices[i] = index;
                vlist_bind_row(st, row, index);
                break;
            }
        }

        if (row == NULL) {
            /* 池子耗尽：理论不会发生（池 = 可见数 + 2 ≥ 视口条目数） */
            ESP_LOGW(TAG, "vlist: row pool exhausted first=%u last=%u",
                     (unsigned)first, (unsigned)last);
            break;
        }
    }
}

/* 滚动事件：滚动/惯性/scroll_to 动画过程中持续触发，实时重绑定 */
static void vlist_scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    vlist_rebind(vlist_get_state(list), list, false);
}

/* 点击事件（由行容器冒泡到列表）：找出被点的行对应的条目并选中。
   图标/文本 label 已做命中穿透，正常情况 target 就是行容器；为防个别
   对象仍收到点击，向上回溯找所在行容器（与旧版行为兼容）。 */
static void vlist_click_event_cb(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_current_target(e);
    lv_obj_t *t = lv_event_get_target(e);
    virtual_list_state_t *st = vlist_get_state(list);

    while (t != NULL && t != list) {
        for (uint32_t i = 0; i < st->row_count; i++) {
            if (st->rows[i].root == t) {
                uint32_t idx = st->row_indices[i];
                if (idx == VLIST_NO_INDEX) {
                    return;
                }
                virtual_list_set_selected(list, (int32_t)idx);
                if (st->selected_cb != NULL) {
                    st->selected_cb(idx, st->user_data);
                }
                return;
            }
        }
        t = lv_obj_get_parent(t);
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

    /* 列表容器：可垂直滚动、原生滚动条、无边框无内边距（透明背景：
       深灰卡片底色由列表页的卡片背景层提供，见 list_common） */
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, w, h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
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
    st->viewport_h  = h;
    st->bind_cb     = bind_cb;
    st->selected_cb = NULL;
    st->user_data   = user_data;
    st->selected       = -1;
    st->marquee_enable = true;   /* 默认开启跑马灯 */
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
    lv_obj_add_event_cb(ph, vlist_pass_through_cb, LV_EVENT_HIT_TEST, NULL);
    st->placeholder = ph;

    /* 创建固定数量行槽（可见数 + 2 缓冲），每行 = 图标 + 文本两个 label */
    st->row_count = (uint32_t)((h + item_height - 1) / item_height) + 2;
    st->rows       = lv_mem_alloc(sizeof(vlist_row_t) * st->row_count);
    st->row_indices = lv_mem_alloc(sizeof(uint32_t) * st->row_count);
    if (st->rows == NULL || st->row_indices == NULL) {
        ESP_LOGE(TAG, "alloc rows failed");
        lv_mem_free(st->rows);
        lv_mem_free(st->row_indices);
        lv_mem_free(st);
        lv_obj_del(list);
        return NULL;
    }

    /* 文本垂直居中所需的顶部内边距：行容器高度被强制固定为 item_height
       （虚拟列表定位要求，不能自适应高度），而文本从内容区顶部绘制，
       故顶部留白 = (item_height - 行高)/2 使文本视觉居中；
       底部不留边距，保证内容区高度 ≥ 行高，避免超行高文本被裁切 */
    lv_coord_t line_h = lv_font_get_line_height(UI_SYSTEM_FONT);
    lv_coord_t pad_top = (item_height - line_h) / 2;
    if (pad_top < 0) {
        pad_top = 0;   /* 行高超过条目高度时的防御：不设上边距 */
    }

    /* 文本 label 宽度：从 VLIST_TEXT_X 起到右侧留白（滚动条区）止 */
    lv_coord_t text_w = w - VLIST_TEXT_X - VLIST_PAD_R;

    for (uint32_t i = 0; i < st->row_count; i++) {
        /* 行容器：定位/隐藏/点击目标，按下反馈背景铺满整行 */
        lv_obj_t *root = lv_obj_create(list);
        lv_obj_set_size(root, LV_PCT(100), item_height);
        lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(root, 0, 0);
        lv_obj_set_style_pad_all(root, 0, 0);
        lv_obj_set_style_radius(root, 0, 0);
        /* 按下反馈：仅按压瞬间显示黄色背景（50% 黄 + 50% 黑），
           松开后 LVGL 自动清除 PRESSED 状态、恢复透明背景（无颜色） */
        lv_obj_set_style_bg_color(root, lv_color_hex(VLIST_PRESSED_BG),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);    /* 作为点击目标 */
        lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE); /* CLICKED 冒泡到 list */

        /* 图标 label：固定左侧，不拦截点击（命中穿透到行容器） */
        lv_obj_t *icon = lv_label_create(root);
        lv_label_set_text(icon, "");
        lv_obj_set_style_text_font(icon, VLIST_ICON_FONT, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, VLIST_ICON_X, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, vlist_pass_through_cb, LV_EVENT_HIT_TEST, NULL);

        /* 名称文本 label：图标右侧起，超长跑马灯/裁剪，同样不拦截点击 */
        lv_obj_t *text = lv_label_create(root);
        lv_obj_set_pos(text, VLIST_TEXT_X, 0);
        lv_obj_set_size(text, text_w, item_height);
        lv_obj_set_style_text_font(text, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(text, lv_color_white(), 0);
        lv_obj_set_style_pad_top(text, pad_top, 0);
        lv_obj_set_style_pad_bottom(text, 0, 0);
        lv_label_set_long_mode(text, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(text, vlist_pass_through_cb, LV_EVENT_HIT_TEST, NULL);
        /* 登记到全局跑马灯管理（此刻还没有文本、不会滚动；文本绑定后由
           vlist_bind_row 重新接管并开始滚动）。登记一次即可，之后由守护
           定时器兜底，无需每个行槽反复登记 */
        global_style_marquee_attach(text);

        st->rows[i].root = root;
        st->rows[i].icon = icon;
        st->rows[i].text = text;
        st->row_indices[i] = VLIST_NO_INDEX;

        /* 初始隐藏：行槽只有被绑定到某个条目时才显示。否则池中未被首屏
           覆盖的缓冲行会停在 (0,0)，叠在第一项上 */
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }

    /* 事件：滚动重绑 + 点击选中 */
    lv_obj_add_event_cb(list, vlist_scroll_event_cb, LV_EVENT_SCROLL, NULL);
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
        return;   /* 未变化，无需处理 */
    }
    st->selected = index;
    /* 选中状态仅内部记录（供 get_selected 查询），不再渲染背景：
       常驻选中背景已移除，点击反馈改为按下瞬间的黄色高亮（松开即消失）；
       "当前播放中"的条目由绑定回调以文字黄色标注 */
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
    vlist_apply_marquee(st);   /* 立即切换所有行文本的长文本模式 */
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
