/*******************************************************************************
 * font_ttf.h
 * 系统字体（TTF / FreeType）接口：界面文字统一从这里取字体。
 *
 * 背景：2026-10 把系统字体从"内嵌位图字库"迁移到 font 分区里的 system.ttf
 * （FreeType 按需光栅化；文件名写死为 spiffs/system.ttf，当前内容 Droid Sans
 * Fallback）。⚠ 字体**不在固件里**：刷完固件还要用 esptool 手动刷一遍 font 分区
 * （一条命令，见 ideas_and_exps/FONT_FLASH.md）。详细选型、分区计算与踩坑见
 * ideas_and_exps/FontSystem.md，分区定义见根目录 partitions.csv。
 *
 * 用法：
 *   · main.c 在 lvgl_port_init() 之后调一次 font_ttf_init()；
 *   · UI 代码用 UI_SYSTEM_FONT / UI_SMALL_FONT（见 core/fonts/system_font.h），
 *     或直接调 font_ttf_get(px)；
 *   · TTF 不可用（分区为空/文件损坏）时 font_ttf_get()
 *     自动返回对应的**内嵌位图回退字体**，UI 不会因为字体问题变空白。
 ******************************************************************************/
#ifndef FONT_TTF_H
#define FONT_TTF_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UI 用到的字号（px）——与原位图字体保持一致，避免界面布局变化：
   - 24px：原 UI_SYSTEM_FONT（system_global_font[_lite] 就是 24px 点阵）
   - 20px：原 system_smaller_font（20px 点阵，由 MiSans-Bold 生成；
           TTF 用的是 Droid Sans Fallback 的常规字重，所以这两处会由"粗"变"常规"） */
#define UI_FONT_PX_MAIN   24
#define UI_FONT_PX_SMALL  20

/**
 * @brief 初始化字体：校验 font 分区 → 注册 lv_fs 驱动 → 预创建各字号字体
 *
 * ⚠ 必须在 lvgl_port_init() 之后调用（依赖 lv_init() 已初始化 FreeType）。
 * ⚠ **调用方必须已持 LVGL 锁**：字体是**延迟加载**的，唯一调用点是
 *   `boot_enter_normal()`（在 `boot_flow_run()` 的 LVGL 锁内执行）——也就是
 *   只有"确实要进常规界面"时才加载 TTF。
 *   低电 / 无卡 / 待扫描这些非正常启动分支全程使用内嵌位图字体：boot 阶段不碰
 *   FreeType、不占 PSRAM 字形缓存，也就不会在随后扫描写 flash（关外部内存缓存、
 *   PSRAM 不可访问）期间踩到"字形取不到 → 文字变方框"的坑。
 *   该函数是幂等的：已加载时直接返回 ESP_OK。
 *   失败时返回错误码并打印原因，界面仍可用（自动回退内嵌位图字体）。
 *
 * @return ESP_OK 成功；其它为分区查找/校验失败（此时已回退位图字体）
 */
esp_err_t font_ttf_init(void);

/**
 * @brief 取指定字号的系统字体
 *
 * @param px 字号（像素）。目前只预创建了 UI_FONT_PX_MAIN / UI_FONT_PX_SMALL，
 *           其它字号会返回对应的内嵌回退字体（不会运行时新建，避免在
 *           绘制/事件回调里分配字体对象）
 * @return 字体指针，永不为 NULL
 */
const lv_font_t *font_ttf_get(uint16_t px);

/** @brief TTF 是否已就绪（false = 正在用内嵌回退字体） */
bool font_ttf_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* FONT_TTF_H */
