#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file task_psram.h
 * @brief PSRAM 栈任务创建助手（内部 RAM 紧张时的标准做法）
 *
 * 背景：开机末期内部 RAM 已接近耗尽（实测内部空闲约 4~5KB、堆内 DMA 空闲约
 * 2KB，最大连续块仅 ~8KB），新建任务时 3~8KB 的内部栈会分配失败——
 * 曾出现 `create power service task failed`（自动息屏/低电关机全失效）、
 * 重新扫描任务无法启动（提示无法分配栈内存）、performance_monitor 的
 * mem_monitor/cpu_coll 任务起不来。
 *
 * 方案：把**对实时性不敏感**的后台任务栈放到 PSRAM（8MB 富余），
 * 内部 RAM 留给真正需要的对象（LVGL 堆、驱动 DMA、音频/UI/按键任务栈）。
 * 本机 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`（见 sdkconfig.defaults），
 * 且 PSRAM 已开启 XIP（指令/只读数据都在 PSRAM），栈上取指命中缓存即可。
 *
 * ⚠ 适用判据（放 PSRAM 前先确认）：
 *   1. 任务不做微秒级硬实时（栈访问偶发缓存未命中会增加几十~几百 ns）；
 *   2. **任务绝不触发"关缓存"流程**（关键！）：SPI Flash 擦写、分区 mmap
 *      映射/解除映射都会先关外部内存缓存，此时 PSRAM 不可访问，栈在 PSRAM
 *      会命中 esp_mm 的断言
 *      `assert failed: esp_cache_freeze_caches_disable_interrupts ...
 *       (s_task_stack_is_sane_when_cache_frozen())` 并直接 panic。
 *      具体触发点：`esp_partition_munmap` / `esp_partition_mmap`（本项目
 *      pldata 层的 `pldata_munmap`、`pldata_unmap_if_mapped`——后者在
 *      pldata_write / pldata_erase_range 里都会调用）、任何 `esp_flash_*`
 *      擦写。**凡是要写/擦播放列表分区的任务（扫描/重新扫描/清数据）栈
 *      必须留在内部 RAM**；
 *   3. 栈需求大、执行频率低的任务优先（监视/日志类）。
 *
 * ⚠ 不适用：LVGL（每帧刷屏）、button_svc（按键中断触发、要求低延迟）、
 *   player_ctrl/deep_worker（交互与深链主路径）、boot_scan/rescan
 *   （分区写擦 + munmap 关缓存）。
 * ⚠ 例外：`mp3_play` 因多格式解码需要 24KB 栈，现也使用本接口创建；
 *   其运行期不触发 flash 关缓存路径。
 */

/**
 * @brief 创建任务，栈优先从 PSRAM 分配（失败自动回退内部 RAM）
 *
 * 与 xTaskCreatePinnedToCore 参数完全一致，仅内存来源不同：
 * 先尝试 MALLOC_CAP_SPIRAM，失败（PSRAM 未启用/耗尽）再退回默认内部 RAM，
 * 因此调用方无需处理"PSRAM 不可用"的分支。
 *
 * @return pdPASS 成功；否则失败（内部 RAM 也不够）
 */
BaseType_t task_create_psram_stack(TaskFunction_t task_func,
                                   const char *name,
                                   uint32_t stack_bytes,
                                   void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_handle,
                                   BaseType_t core_id);

#ifdef __cplusplus
}
#endif
