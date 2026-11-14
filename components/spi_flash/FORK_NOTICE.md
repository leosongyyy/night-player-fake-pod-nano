# spi_flash 本地 fork 说明

本目录是 **ESP-IDF 6.1.0 `components/spi_flash` 的本地副本**（2026-11-14），
用于修复 ESP32-S31 的 flash cache 竞态（espressif/esp-idf#18948 / IDFGH-18107）：

> flash 操作 suspend cache 期间，另一核停在 `spi_flash_op_block_func` 的
> IRAM 自旋里，分支预测器仍会投机取指到 flash，触发 `Cache error` panic。

## 唯一改动

`cache_utils.c`：

1. `spi_flash_op_block_func()` 进入时关闭分支预测，`spi_flash_restore_cache()`
   后恢复；
2. `spi_flash_disable_interrupts_caches_and_other_cpu()` 在 disable cache 前
   关闭当前核分支预测；
3. `spi_flash_enable_interrupts_caches_and_other_cpu()` 在 restore cache 后恢复。

其余文件与 IDF 6.1.0 一致；为减小仓库体积，已删除 `test_apps/`、`linux/`、
`mock/`、`sim/`（S31 构建不使用）。

## 升级 IDF 时

先检查上游 `components/spi_flash/cache_utils.c` 是否已包含
`esp_cpu_branch_prediction_disable()`（issue #18948 的修复）：

- 已包含 → 删除本目录，恢复使用 IDF 组件；
- 未包含 → 保留本目录，并按新版本重新对比/移植补丁（不要直接覆盖，
  上游可能还有其它改动）。

完整调查与验证记录：`ideas_and_exps/S31_FLASH_CACHE_RACE.md`。
