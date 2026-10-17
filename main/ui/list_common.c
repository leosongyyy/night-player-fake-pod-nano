#include "ui/list_common.h"

#include <string.h>

#include "core/fonts/system_font.h"

/* 标题栏内用户数据：保存图标/标题/计数 label 句柄，随标题栏对象一起管理 */
typedef struct {
    lv_obj_t *icon_lbl;   /* 左侧 LV_SYMBOL 图标（可无） */
    lv_obj_t *title_lbl;  /* 标题文字 */
    lv_obj_t *sub_lbl;    /* 右侧计数文字 */
} ui_title_bar_t;

/* 标题栏图标列宽（px）：图标固定占位，标题文字自动右移到其后。
   用固定列宽而非实测图标宽度，避免 label 尺寸异步更新导致布局跳动 */
#define UI_TITLE_ICON_COL_W  24
#define UI_TITLE_ICON_X      16

/* 标题栏被删除时释放用户数据结构（防止 lv_obj 删除后 user_data 泄漏） */
static void title_bar_delete_cb(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    lv_free(lv_obj_get_user_data(bar));
}

void ui_page_style(lv_obj_t *page)
{
    /* ⚠ 页面背景**透明**：黑底由 main_ui 的 tabview 内容容器（cont）统一提供。
       原因见 main_ui_create() 里的说明——partial 模式下每个不透明整块背景都会
       让每个绘制块多填一遍 2 万像素，一帧下来是好几毫秒；页面这一层被 cont
       完全覆盖，纯属重复。视觉不变（同一个黑色）。
       ⚠ 本函数只用于 tabview 里的四个页面；新建**全屏覆盖层**（不挂在 tabview
       下）时不要用它，必须自己铺不透明底色。 */
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t *ui_title_bar_create(lv_obj_t *parent, const char *title,
                              const char *sub)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), UI_TITLE_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    /* 背景透明：标题栏叠在页面的深灰卡片背景上（卡片色由卡片层提供） */
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    /* 底部分隔线：与内容区之间 1px 灰线 */
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    /* v9：lv_mem_alloc → lv_malloc（见 title_bar_delete_cb 的 lv_free 配对） */
    ui_title_bar_t *t = lv_malloc(sizeof(*t));
    if (t == NULL) {
        lv_obj_del(bar);
        return NULL;
    }
    memset(t, 0, sizeof(*t));

    t->title_lbl = lv_label_create(bar);
    lv_label_set_text(t->title_lbl, title);
    lv_obj_set_style_text_font(t->title_lbl, UI_SYSTEM_FONT, 0);
    lv_obj_set_style_text_color(t->title_lbl, lv_color_white(), 0);
    /* 标题占标题栏左侧 3/4 宽（与右侧计数 1/4 按 3:1 划分，互不覆盖）：
       超长文本直接裁剪（不换行、不跑马灯），保证永远不盖住计数显示 */
    lv_obj_set_width(t->title_lbl, LV_PCT(75));
    lv_obj_set_style_pad_left(t->title_lbl, UI_TITLE_ICON_X, 0);
    lv_label_set_long_mode(t->title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(t->title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    if (sub != NULL) {
        t->sub_lbl = lv_label_create(bar);
        lv_label_set_text(t->sub_lbl, sub);
        lv_obj_set_style_text_font(t->sub_lbl, UI_SYSTEM_FONT, 0);
        lv_obj_set_style_text_color(t->sub_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
        /* 计数占标题栏右侧 1/4 宽：文本右对齐，超长同样裁剪 */
        lv_obj_set_width(t->sub_lbl, LV_PCT(25));
        lv_obj_set_style_pad_right(t->sub_lbl, 16, 0);
        lv_obj_set_style_text_align(t->sub_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(t->sub_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(t->sub_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_set_user_data(bar, t);
    lv_obj_add_event_cb(bar, title_bar_delete_cb, LV_EVENT_DELETE, NULL);
    return bar;
}

void ui_title_bar_set_text(lv_obj_t *bar, const char *title, const char *sub)
{
    if (bar == NULL) {
        return;
    }
    ui_title_bar_t *t = (ui_title_bar_t *)lv_obj_get_user_data(bar);
    if (t == NULL) {
        return;
    }

    if (title != NULL && t->title_lbl != NULL) {
        lv_label_set_text(t->title_lbl, title);
    }

    if (sub != NULL) {
        if (t->sub_lbl == NULL) {
            /* 首次出现计数文字时动态创建（占右侧 1/4 宽、右对齐灰色小字） */
            t->sub_lbl = lv_label_create(bar);
            lv_obj_set_style_text_font(t->sub_lbl, UI_SYSTEM_FONT, 0);
            lv_obj_set_style_text_color(t->sub_lbl,
                                        lv_palette_main(LV_PALETTE_GREY), 0);
            lv_obj_set_width(t->sub_lbl, LV_PCT(25));
            lv_obj_set_style_pad_right(t->sub_lbl, 16, 0);
            lv_obj_set_style_text_align(t->sub_lbl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(t->sub_lbl, LV_LABEL_LONG_CLIP);
            lv_obj_align(t->sub_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_label_set_text(t->sub_lbl, sub);
        lv_obj_clear_flag(t->sub_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (t->sub_lbl != NULL) {
        /* sub 为 NULL：隐藏右侧文字 */
        lv_obj_add_flag(t->sub_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_title_bar_set_icon(lv_obj_t *bar, const char *icon)
{
    if (bar == NULL) {
        return;
    }
    ui_title_bar_t *t = (ui_title_bar_t *)lv_obj_get_user_data(bar);
    if (t == NULL) {
        return;
    }

    bool has_icon = (icon != NULL && icon[0] != '\0');
    if (t->icon_lbl == NULL) {
        if (!has_icon) {
            return;   /* 无图标且从未创建：无事可做 */
        }
        t->icon_lbl = lv_label_create(bar);
        /* 图标用 LVGL 内置字体（系统字体不含 PUA 区 LV_SYMBOL 字形），
           与状态栏图标同字号 20 */
        lv_obj_set_style_text_font(t->icon_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(t->icon_lbl, lv_color_white(), 0);
        lv_obj_align(t->icon_lbl, LV_ALIGN_LEFT_MID, UI_TITLE_ICON_X, 0);
        /* 标题文字右移给图标让位：图标区 + 10px 间隙，使标题起点与
           列表行文本起点（VLIST_TEXT_X=50，页内再加卡片左边距 10）
           绝对对齐 */
        lv_obj_set_style_pad_left(t->title_lbl,
                                  UI_TITLE_ICON_X + UI_TITLE_ICON_COL_W + 10, 0);
    }

    if (has_icon) {
        lv_label_set_text(t->icon_lbl, icon);
        lv_obj_clear_flag(t->icon_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(t->icon_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_card_bg_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_coord_t corner)
{
    if (corner < 0) {
        corner = 0;
    }
    /* 圆角不能超过半高（否则 LVGL 会把 radius 夹到 min(w,h)/2，视觉退化） */
    if (corner > h / 2) {
        corner = h / 2;
    }

    /* ================== 为什么拆成"圆角帽 + 直角主体" ==================
     * ⚠ 直接用一个整卡圆角矩形（radius=corner）会让**整个卡片区域**都走 LVGL
     *   的"复杂矩形"绘制路径：lv_draw_sw_fill.c 里只有
     *       if(dsc->radius == 0 && grad_dir == LV_GRAD_DIR_NONE)  →  快速路径
     *   其余情况都要先生成圆角掩码、再逐像素带掩码混合。而圆角其实只出现在
     *   卡片顶部 corner 高的一条弧上。
     *   实测影响：列表滚动时卡片区域每帧要被重画多次（partial 模式的绘制块），
     *   每次都走圆角掩码路径，单这一项就吃掉好几毫秒。
     *
     * 拆法（视觉完全等价）：
     *   · 圆角帽：高 2*corner，radius=corner（高度 2*corner 时 LVGL 不会夹小
     *     radius，四角都是标准弧）；它自己的**上面两角**就是卡片的圆角轮廓；
     *   · 直角主体：从"圆角帽的下弧起点"（y+corner）一直铺到卡片底部，
     *     radius=0 → 走快速路径（纯色 COVER 填充近似 memset）。它同时盖住
     *     圆角帽的**下面两角**（否则会露出两个圆角缺口），于是底部天然直角。
     *   ⇒ 列表内容区完全落在"直角主体"里，滚动重绘时不再碰掩码路径。
     */
    const lv_coord_t cap_h = (lv_coord_t)(2 * corner);
    if (corner > 0 && h > cap_h) {
        lv_obj_t *cap = lv_obj_create(parent);
        lv_obj_set_pos(cap, x, y);
        lv_obj_set_size(cap, w, cap_h);
        lv_obj_set_style_bg_color(cap, UI_CARD_COLOR, 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cap, corner, 0);
        lv_obj_set_style_border_width(cap, 0, 0);
        lv_obj_set_style_pad_all(cap, 0, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(cap, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    } else if (corner > 0) {
        /* 卡片本身太矮（h ≤ 2*corner）：画不了"帽+主体"，退回单个圆角矩形。
           本项目实际不会走到这里（列表卡片远高于 60 px）。 */
        lv_obj_t *round = lv_obj_create(parent);
        lv_obj_set_pos(round, x, y);
        lv_obj_set_size(round, w, h);
        lv_obj_set_style_bg_color(round, UI_CARD_COLOR, 0);
        lv_obj_set_style_bg_opa(round, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(round, corner, 0);
        lv_obj_set_style_border_width(round, 0, 0);
        lv_obj_set_style_pad_all(round, 0, 0);
        lv_obj_clear_flag(round, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(round, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(round, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    /* 直角主体：从 y+corner 铺到底（恰好覆盖圆角帽的下弧，视觉上底部为直角） */
    lv_obj_t *straight = lv_obj_create(parent);
    lv_obj_set_pos(straight, x, (lv_coord_t)(y + corner));
    lv_obj_set_size(straight, w, (lv_coord_t)(h - corner));
    lv_obj_set_style_bg_color(straight, UI_CARD_COLOR, 0);
    lv_obj_set_style_bg_opa(straight, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(straight, 0, 0);
    lv_obj_set_style_border_width(straight, 0, 0);
    lv_obj_set_style_pad_all(straight, 0, 0);
    lv_obj_clear_flag(straight, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(straight, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(straight, LV_OBJ_FLAG_CLICKABLE);
}
