#include "ui/cassette/cassette_theme.h"

/* 三个共享样式各初始化一次；不改运行时字体/色板（Palette 编译期常量）。 */
static lv_style_t s_style_metal;
static lv_style_t s_style_groove;
static lv_style_t s_style_paper;
static bool s_styles_ready = false;

static void cas_styles_init_once(void)
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;

    /* 金属：自上而下由亮到暗的拉丝感，1px 深槽描边 + 顶部高光。
       LVGL 9 渐变用 bg_grad_color / bg_grad_dir / bg_grad_stop。 */
    lv_style_init(&s_style_metal);
    lv_style_set_bg_color(&s_style_metal, CAS_COL_METAL);
    lv_style_set_bg_grad_color(&s_style_metal, CAS_COL_METAL_HI);
    lv_style_set_bg_grad_dir(&s_style_metal, LV_GRAD_DIR_VER);
    lv_style_set_bg_grad_stop(&s_style_metal, 80);
    lv_style_set_bg_opa(&s_style_metal, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_metal, 1);
    lv_style_set_border_color(&s_style_metal, CAS_COL_GROOVE);
    lv_style_set_radius(&s_style_metal, 4);
    lv_style_set_pad_all(&s_style_metal, 0);

    /* 凹陷槽：底色比机箱更黑，外圈金属亮边模拟倒角。 */
    lv_style_init(&s_style_groove);
    lv_style_set_bg_color(&s_style_groove, CAS_COL_GROOVE);
    lv_style_set_bg_opa(&s_style_groove, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_groove, 1);
    lv_style_set_border_color(&s_style_groove, CAS_COL_METAL_HI);
    lv_style_set_radius(&s_style_groove, 6);
    lv_style_set_pad_all(&s_style_groove, 0);

    /* 米白贴纸/铭牌：纸底 + 墨色字 + 极小阴影模拟贴纸翘边。 */
    lv_style_init(&s_style_paper);
    lv_style_set_bg_color(&s_style_paper, CAS_COL_PAPER);
    lv_style_set_bg_opa(&s_style_paper, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_paper, CAS_COL_INK);
    lv_style_set_text_font(&s_style_paper, CAS_FONT_BODY);
    lv_style_set_radius(&s_style_paper, 3);
    lv_style_set_pad_all(&s_style_paper, 4);
    lv_style_set_shadow_width(&s_style_paper, 4);
    lv_style_set_shadow_color(&s_style_paper, CAS_COL_GROOVE);
}

const lv_style_t *cas_style_metal(void)
{
    cas_styles_init_once();
    return &s_style_metal;
}

const lv_style_t *cas_style_groove(void)
{
    cas_styles_init_once();
    return &s_style_groove;
}

const lv_style_t *cas_style_paper(void)
{
    cas_styles_init_once();
    return &s_style_paper;
}
