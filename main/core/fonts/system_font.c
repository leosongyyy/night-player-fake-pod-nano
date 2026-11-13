/*******************************************************************************
 * system_font.c
 * 系统字体选择：优先 font 分区的 NPF1 全字库，失败回退内嵌精简位图字库。
 *
 * 为什么保留内嵌精简字库：font 分区是"手动刷入"的（idfc flash 不含字体），
 * 新板子/换分区表/刷坏字体时如果没有任何兜底字体，整个 UI 会一个字都画不出来
 * （LVGL 会退回内置 montserrat 或画方框）。精简字库只有几百 KB，覆盖全部 UI
 * 固定文案，用来兜底正合适；用户数据（歌名/目录名）缺字只会出现在这种异常态。
 ******************************************************************************/

#include "system_font.h"

#include "esp_log.h"
#include "font_npf.h"
#include "system_global_font_lite.h"

static const char *TAG = "system_font";

static const lv_font_t *s_active;
static bool s_full;

void system_font_init(void)
{
    if (s_active != NULL) {
        return;
    }

    if (font_npf_init() == ESP_OK) {
        s_active = font_npf_get();
        s_full = (s_active != NULL);
    }

    if (s_active == NULL) {
        s_active = &system_global_font_lite;
        s_full = false;
        ESP_LOGW(TAG, "使用内嵌精简字库兜底：用户数据里的生僻字会缺字（刷字见 NPF_FONT.md）");
    } else {
        ESP_LOGI(TAG, "系统字体：NPF1 全字库（%u 个字形）",
                 (unsigned)font_npf_glyph_count());
    }
}

const lv_font_t *system_font_get(void)
{
    /* init 之前（或 init 被跳过）也能安全取到兜底字体 */
    return (s_active != NULL) ? s_active : &system_global_font_lite;
}

bool system_font_is_full(void)
{
    return s_full;
}
