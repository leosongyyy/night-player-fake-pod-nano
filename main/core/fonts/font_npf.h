#pragma once

/*******************************************************************************
 * font_npf.h
 * NPF1（NightPlayer Font v1）外置全字库加载器。
 *
 * 背景：内嵌位图字库（*.c）受固件体积限制只能收录常用字，用户歌名/目录名里的
 * 生僻字会显示为方框。本模块把整份 24px 全字库（Droid Sans Fallback，34,461 个
 * 码位）预烘焙成 NPF1 镜像放进 `font` 数据分区（6MB @0x600000），运行时只做一次
 * `esp_partition_mmap` + 填几个指针（**零解析**），字形位图与 glyph_dsc
 * 始终留在 Flash（占 MMU 页，不占 RAM）。
 *
 * 格式与 LVGL 9.4 的对应关系、生成方法见 ideas_and_exps/NPF_FONT.md。
 * （NPF1 镜像本身与 LVGL 版本无关：glyph_dsc 的 LARGE 布局在 8.4 / 9.4 上一致，
 *   S3 分支 legacy_s3 与本分支共用同一份 spiffs/system.npf。）
 *
 * 典型用法（开机一次）：
 * @code
 *   esp_err_t err = font_npf_init();   // 失败不影响启动，UI 回退内嵌精简字库
 *   const lv_font_t *f = font_npf_get();
 * @endcode
 ******************************************************************************/

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NPF1 镜像版本（与 tools/font/bin_font_prebake.py 的 NPF_VERSION 一致） */
#define FONT_NPF_VERSION 1

/**
 * @brief 查找 `font` 分区、校验并挂载 NPF1 字库（重复调用直接返回缓存结果）
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 分区不存在；
 *         ESP_ERR_INVALID_STATE 镜像 magic/版本/长度非法（多半是没刷字体）；
 *         其它为 mmap 或内存分配失败
 */
esp_err_t font_npf_init(void);

/**
 * @brief 取已挂载的字体描述符
 *
 * @return 成功时返回指向静态 `lv_font_t` 的指针；未挂载/加载失败返回 NULL
 */
const lv_font_t *font_npf_get(void);

/**
 * @brief 挂载的字体覆盖的字形数（含 gid0），未挂载时为 0
 */
uint32_t font_npf_glyph_count(void);

#ifdef __cplusplus
}
#endif
