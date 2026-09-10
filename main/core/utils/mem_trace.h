#pragma once

/*
 * 内存快照诊断（FUTURE_PLAN 第 34 点调查用，Kconfig CONFIG_MEM_TRACE 控制）
 *
 * 用途：在启动各阶段、开机 UI 创建、封面加载等关键路径上打印
 * 内部 RAM（INTERNAL|8BIT）、DMA 池、PSRAM 的空闲量 / 最大连续块 /
 * 已分配块数，用于定位"谁吃掉了内部 RAM 与 DMA 池"。
 *
 * 判读要点（ESP32-S3）：
 *   - 内部 DRAM 同时是 DMA 池的来源，因此"内部 RAM 空闲"与"DMA 池空闲"高度相关；
 *   - `blocks=已分配/总块数` 的**增量**是关键：
 *       少量块数增量 + 大字节增量 → 少数大分配（缓冲类）；
 *       大量块数增量 + 中等字节增量 → 许多小对象（LVGL 控件/样式类）；
 *   - `largest`（最大连续块）比 free 更能反映"还能不能分配出 512B/几 KB"。
 *
 * ⚠ 本模块只做只读统计，不做任何分配。
 * ⚠ CONFIG_MEM_TRACE 关闭时（release 构建）两个函数退化为空宏，
 *   调用点无需 #if 包裹，字符串参数会被编译器直接丢弃。
 *
 * 2026-09-10 实测结论见 ideas_and_exps/IRAM_DMA_AUDIT.md：
 *   封面链路的 .npd / JPEG / 像素缓冲全部落 PSRAM，真凶是 LVGL 常驻 UI 对象。
 */

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MEM_TRACE

/* 打印一次内存快照。stage 为阶段标签（建议简短，如 "10-lvgl_port"、"cover:req"） */
void mem_trace_log(const char *stage);

/* 打印一笔大块分配的实际落点（PSRAM / INTERNAL），
   what 为用途名（如 "npd"、"jpeg data"、"pixel"），tag 为日志 TAG */
void mem_trace_ptr(const char *tag, const char *what, size_t bytes, const void *ptr);

/* 单独打印一组 caps（名称 + caps 掩码），用于自定义组合 */
void mem_trace_caps(const char *name, unsigned int caps);

#else /* !CONFIG_MEM_TRACE */

#define mem_trace_log(stage)                    ((void)0)
#define mem_trace_ptr(tag, what, bytes, ptr)    ((void)0)
#define mem_trace_caps(name, caps)              ((void)0)

#endif /* CONFIG_MEM_TRACE */

#ifdef __cplusplus
}
#endif
