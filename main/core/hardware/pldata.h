#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表数据结构分区（pldata）驱动
 *
 * 负责访问 Flash 中一个独立的 4MB 数据分区，用于持久化播放列表等
 * 结构化的播放器数据（见 partitions.csv 的 `pldata` 分区）。
 *
 * 设计要点（省 RAM）：
 *  - 读取通过 esp_partition_mmap() 把整个分区**内存映射**到地址空间，
 *    直接以指针访问，数据仍留在 Flash，**不占用堆 RAM**；
 *    mmap 只占用少量 MMU 表项 / Flash cache。
 *  - Flash 只支持"整扇区擦除 + 按字节/页写"，因此：
 *      * 写入前目标区域必须是**已擦除**状态（全 0xFF）；
 *      * 推荐"整体保存"模型：pldata_erase_all() 擦除整个分区后，
 *        再把整份播放列表数据从 offset 0 顺序写入。
 *      * 对同一扇区重复写入（未先擦除）会得到错误结果，调用方需自行管理布局。
 *  - mmap 期间写 Flash 会造成缓存不一致，因此 write/erase 前会自动解除映射，
 *    写入完成后如需读取需重新 pldata_mmap()。
 *
 * 典型用法：
 * @code
 *   pldata_init();
 *   pldata_erase_all();                 // 整体重写前擦除
 *   pldata_write(0, &pl_header, sizeof(pl_header));   // 写入需已擦除
 *   const void *p;
 *   pldata_mmap(&p, &size);             // 内存映射只读访问（省 RAM）
 *   const my_header_t *h = p;           // 直接当指针用
 *   pldata_munmap();                    // 用完解除映射
 * @endcode
 */

/** pldata 分区大小（4MB） */
#define PLDATA_PART_SIZE (4U * 1024U * 1024U)

/** Flash 扇区大小（擦除最小单位） */
#define PLDATA_SECTOR_SIZE 0x1000

/**
 * @brief 查找 pldata 分区并缓存句柄（可重复调用）
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 分区不存在（检查 partitions.csv）
 */
esp_err_t pldata_init(void);

/**
 * @brief 获取 pldata 分区大小（字节）
 *
 * @return 分区大小；未 init 时为 0
 */
size_t pldata_get_size(void);

/**
 * @brief 擦除整个 pldata 分区（写新数据前先整体擦除，比较耗时，一次性）
 *
 * @return ESP_OK 成功
 */
esp_err_t pldata_erase_all(void);

/**
 * @brief 擦除分区的指定扇区范围
 *
 * @param offset 起始偏移，需按 PLDATA_SECTOR_SIZE 对齐
 * @param size   擦除长度，需按 PLDATA_SECTOR_SIZE 对齐
 * @return ESP_OK 成功；参数未对齐返回 ESP_ERR_INVALID_ARG
 */
esp_err_t pldata_erase_range(uint32_t offset, size_t size);

/**
 * @brief 向分区写入数据（⚠ 目标区域必须是已擦除状态，通常先 erase_all 再整体写）
 *
 * @param offset 起始偏移
 * @param data   数据源
 * @param len    数据长度
 * @return ESP_OK 成功；越界返回 ESP_ERR_INVALID_SIZE
 */
esp_err_t pldata_write(uint32_t offset, const void *data, size_t len);

/**
 * @brief 从分区读取数据
 *
 * 若当前已映射则直接从映射地址 memcpy（省一次 Flash 事务）；否则走普通读。
 *
 * @param offset 起始偏移
 * @param dst    目标缓冲
 * @param len    读取长度
 * @return ESP_OK 成功
 */
esp_err_t pldata_read(uint32_t offset, void *dst, size_t len);

/**
 * @brief 把整个分区内存映射到地址空间，只读访问（省 RAM）
 *
 * 返回的指针直接指向 Flash（只读），播放列表可整体当作在 RAM 中访问，
 * 但不占用堆 RAM。同一时刻只允许一份映射，重复调用返回已有映射。
 *
 * @param[out] out_ptr  返回映射基址
 * @param[out] out_size 返回映射大小（= 分区大小）
 * @return ESP_OK 成功
 */
esp_err_t pldata_mmap(const void **out_ptr, size_t *out_size);

/**
 * @brief 解除 pldata 内存映射（写入前会自动调用本函数）
 */
void pldata_munmap(void);

#ifdef __cplusplus
}
#endif
