#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_font.h
 * @brief 袖珍磁带机 UI 的字体统一入口
 *
 * ⚠ F5：`UI_SYSTEM_FONT` 是运行期函数（system_font_get()），不是常量，
 *   不能写进静态初始化器。因此这里把两个字体都包在 getter 后面，模块
 *   一律通过 `CAS_FONT_CN` / `CAS_FONT_MONO` 宏取值，宏展开同样是一次
 *   函数调用，可在任意需要 `const lv_font_t *` 的位置使用（含样式设置）。
 *
 * 两条路径：
 *   - CONFIG_CASSETTE_FONT_FALLBACK=y（默认，字体源缺失时）：
 *       CAS_FONT_CN   -> UI_SYSTEM_FONT（NPF1 全字库 / 内嵌兜底）
 *       CAS_FONT_MONO -> &lv_font_montserrat_24
 *   - CONFIG_CASSETTE_FONT_FALLBACK=n 且 bake_font.sh 已产出 baked 文件：
 *       CAS_FONT_CN   -> &cas_font_cn（站酷庆科黄油体子集）
 *       CAS_FONT_MONO -> &cas_font_mono（若烘焙成功），否则 Montserrat 24
 *
 * 切换只改 Kconfig 与烘焙产物，7 个模块的调用点无需改动。
 */

/** 取复古中文正文/丝印字体（永不返回 NULL） */
const lv_font_t *cas_font_cn_get(void);

/** 取等宽数字字体（翻页钟/时长/容量，永不返回 NULL） */
const lv_font_t *cas_font_mono_get(void);

#define CAS_FONT_CN    (cas_font_cn_get())
#define CAS_FONT_MONO  (cas_font_mono_get())

#ifdef __cplusplus
}
#endif
