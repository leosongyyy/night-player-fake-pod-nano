#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放列表数据（pldata）后端驱动：SD 卡上的缓存文件 + PSRAM 常驻缓冲
 *
 * 2026-11-16 起，播放列表数据**不再放在 flash 的 pldata 分区**，而是卡上一个
 * 普通文件（`/sdcard/.ecov/pldata.bin`），开机整块读进 PSRAM，运行时按指针访问。
 * 设计动机与收益见 ideas_and_exps/PLDATA_ON_SDCARD.md。
 *
 * 文件内容与旧的 flash 分区镜像**逐字节相同**（4KB 头扇区 + PLST 数据本体），
 * 所以 playlist_store / playlist_format 的格式定义一行都不用改，也能直接拿 PC
 * 工具或 `dump` 出来的旧分区镜像做比对。
 *
 * 访问模型：
 *  - **读**：pldata_init() 一次性 fopen + 整块 fread 到 PSRAM（9042 首 ≈ 620KB），
 *    之后 pldata_read / pldata_mmap 都是纯内存操作；
 *  - **写**：pldata_write / pldata_erase_range **只改内存缓冲**，不碰 flash；
 *    一次扫描结束时由 playlist_store 调一次 pldata_commit() 落盘，
 *    走 "写同级 tmp → fsync → rename" 的原子替换；
 *  - pldata_mmap() 的名字沿用（上层语义是"拿到整份镜像的只读基址"），
 *    现在返回的是 PSRAM 地址，pldata_munmap() 是空操作。
 *
 * 为什么值得换（对比 flash 后端）：
 *  - 写盘不再关闭外部内存缓存（只有 flash 擦写才关）→ 扫描期间不必冻结 LVGL，
 *    也不会让 PSRAM 栈任务命中 `esp_cache_freeze_caches_disable_interrupts` 断言；
 *  - 620KB 写卡约 0.15~0.5s（flash 擦写实测 6.14s）；
 *  - 代价：常驻约 620KB PSRAM（16MB 富余），以及"固件会写用户的音乐卡"这类新风险。
 *
 * ⚠ 写卡纪律（必须遵守，见 PLDATA_ON_SDCARD.md §6.1）：
 *   1. **只写自己这一个文件**（+ 同级 tmp），绝不碰其它路径、绝不 format/mkfs；
 *   2. 只在扫描末尾整份写一次，不做增量/高频写（会顶到音频读卡）；
 *   3. 写失败保留旧文件（tmp + rename），最坏情况只是"卡上没缓存 → 下次重扫"。
 */

/**
 * @brief pldata 镜像上限（字节）= 4KB 头 + 4MB 数据本体
 *
 * 4MB 数据本体 = 构建缓冲上限（playlist_format.h 的 `PLS_STR_MAX_OFFSET`）。
 * 之所以不再是 4MB（旧的 pldata 分区大小）：数据搬到卡上以后，容量不再受
 * flash 分区限制，取"构建器能产出的最大镜像"即可。
 */
#define PLDATA_MAX_IMAGE (0x1000U + 4U * 1024U * 1024U)

/** 镜像内部分块粒度（与 FatFs 扇区 CONFIG_FATFS_SECTOR_4096 一致；擦除/对齐单位） */
#define PLDATA_SECTOR_SIZE 0x1000

/**
 * @brief 打开卡上的缓存文件并整块载入 PSRAM（可重复调用，只生效一次）
 *
 * ⚠ 必须在 sd_card_init() **之后**、`lvgl_port_lock()` **之前**调用
 * （整块读约 30~150ms，持 LVGL 锁调用会冻住 UI 任务）。
 *
 * @return ESP_OK 载入成功；ESP_ERR_INVALID_STATE SD 未挂载；
 *         ESP_ERR_NOT_FOUND 没有缓存文件（正常，上层会扫描后 commit 生成）；
 *         ESP_ERR_NO_MEM PSRAM 分配失败
 */
esp_err_t pldata_init(void);

/**
 * @brief 缓冲是否已载入有效镜像（false = 还没有缓存数据，需要扫描）
 */
bool pldata_has_data(void);

/**
 * @brief 获取当前缓冲容量（字节；0 = 尚未分配）
 *
 * 注意语义已变：过去返回 flash 分区大小（固定 4MB），现在是"按需增长到的
 * 缓冲容量"。当前无调用者，保留是为兼容旧接口。
 */
size_t pldata_get_size(void);

/**
 * @brief 擦除整份镜像覆盖的范围（无数据时为空操作）
 *
 * ⚠ 只改内存缓冲，需要在扫描末尾由 pldata_commit() 才落盘。
 */
esp_err_t pldata_erase_all(void);

/**
 * @brief 擦除缓冲内的指定范围（按 0xFF 填充，语义等价于 flash 扇区擦除）
 *
 * 入参契约与 flash 时代一致：offset / size 必须按 PLDATA_SECTOR_SIZE 对齐
 * （playlist_store 一直这么传）。容量不足时按需增长缓冲。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 未对齐；ESP_ERR_INVALID_SIZE 越界
 */
esp_err_t pldata_erase_range(uint32_t offset, size_t size);

/**
 * @brief 向缓冲写入数据（纯 memcpy，不需要"先擦除"）
 *
 * 写入会延长镜像长度（s_len = max(s_len, offset + len)），并在 pldata_commit()
 * 时整份落盘。
 *
 * @param offset 起始偏移
 * @param data   数据源
 * @param len    数据长度
 * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 超出镜像上限
 */
esp_err_t pldata_write(uint32_t offset, const void *data, size_t len);

/**
 * @brief 从缓冲读取数据
 *
 * @param offset 起始偏移
 * @param dst    目标缓冲
 * @param len    读取长度
 * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 超出已载入的镜像长度（含"无数据"）
 */
esp_err_t pldata_read(uint32_t offset, void *dst, size_t len);

/**
 * @brief 拿到整份镜像的只读基址（名字沿用 flash 时代的 mmap，实际是 PSRAM 指针）
 *
 * @param[out] out_ptr  镜像基址（offset 0 = 头部扇区起点）
 * @param[out] out_size 镜像有效长度（= 文件大小）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 无数据
 */
esp_err_t pldata_mmap(const void **out_ptr, size_t *out_size);

/**
 * @brief 空操作（数据在 PSRAM，无需解除映射；保留是为了上层代码零改动）
 */
void pldata_munmap(void);

/**
 * @brief 把缓冲整份写回卡上文件（tmp → fsync → rename 原子替换）
 *
 * 只在扫描末尾调用一次。可在任意任务上下文执行（**不会关闭外部缓存**，
 * 因此 PSRAM 栈任务也安全）。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 无数据；其余为文件系统错误
 */
esp_err_t pldata_commit(void);

/**
 * @brief 删除卡上的缓存文件 `/sdcard/.ecov/pldata.bin`（并让缓冲失效）
 *
 * 这是"立刻失效"的唯一实现：设置菜单「重新扫描」就是**删这个文件再重启**
 * （重启后 boot_flow 发现没有缓存 → 自动重扫）。旧 flash 后端靠"擦头部扇区"
 * 实现同样语义。
 *
 * ⚠ 只删自己那一个文件（+ 同级残留 tmp），绝不碰卡上其它内容。
 * ⚠ 缓冲不释放（马上重启/重扫，留着可复用），只把有效长度清零。
 *
 * @return ESP_OK 成功（文件本来就不存在也算成功）；ESP_FAIL 删除失败
 *         （写保护卡/文件系统异常，调用方应提示用户而不是静默重启）
 */
esp_err_t pldata_delete_cache_file(void);

#ifdef __cplusplus
}
#endif
