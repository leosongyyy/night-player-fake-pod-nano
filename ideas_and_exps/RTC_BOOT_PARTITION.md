# 用 PCF85063A 的 RAM_byte 选启动分区（自定义 bootloader，默认 RTC-only）

> **状态：已升级为默认切换路径（RTC-only，2026-11-16 真机验证）**。
> 正常切换只写 PCF85063A 的 `RAM_byte(0x03)`，不再写/擦 otadata；otadata 仅在
> RTC 不可用时作为兜底。实验代码、失败降级和验证结果全部保留。
>
> 目标：不再靠 otadata 的 flash 擦写来切「表世界 / 里世界」，改用 RTC 芯片里那 1 字节
> RAM_byte；读不到/超时就降级回 otadata，绝不因为 I²C 失败而变砖。
>
> 相关：只做 hook 的体积实验见 [`BOOTLOADER_I2C_SIZE.md`](BOOTLOADER_I2C_SIZE.md)；
> 本文是「整份 override」版本。代码在 `bootloader_components/main/`。

---

## 0. 结论（TL;DR）

| 项目 | 结果 |
|---|---|
| bootloader 体积 | **16,640 B（0x4100）**，上限 24,576 B，余 **7,936 B（32%）** |
| 相比默认 bootloader | +1,264 B（默认 15,376 B） |
| 切换方式 | RTC `0x03`：`0xA5` → `ota_0`（里世界），`0x5A` → `factory`，其它 → otadata |
| 读完处理 | bootloader 读成功后**清 0**（一次性请求） |
| 失败降级 | NACK / 超时 / 读回不一致 / 目标分区不存在 → 走 otadata；镜像坏了 → IDF 自带的 factory 回退 |
| 一次往返的 otadata 擦除 | **0 次**（旧路径 3 次 × 4 KB） |
| `fw_switch_to_sync()` 耗时 | **~220 ms，与旧路径相当**（仍做 1.5 MB 全量镜像校验；省的是擦除，不是时间） |
| 仍存在的 flash 写 | NVS 播放现场（`last_song`/`last_pos` 等，约 2 个 entry，页擦除摊销） |

**能减少，而且减少的正是切换路径上最“硬”的那几次扇区擦除**；NVS 的播放现场保存
仍在（那是恢复播放所必需的），所以不是“整个切换 0 flash 写”。

> **默认值**：`main/core/system/fw_switch.c` 的 `FW_SWITCH_USE_RTC_BOOT` 已置 **1**。
> 只有把自定义 bootloader 换回 IDF 默认版时，才需要改成 0（改用纯 otadata 路径）。

---

## 1. 协议

PCF85063A 的 `0x03` 是 NXP 定义的 free RAM byte（复位 `0x00`，SR/掉电才清）。
电池常驻时它跨 SoC 复位保持：

| `0x03` 值 | bootloader 行为 |
|---|---|
| `0xA5` | 选 `ota_0`（里世界）；成功后清 `0x00` |
| `0x5A` | 选 `factory`（表世界）；成功后清 `0x00` |
| 其它（含 `0x00`） | 不干预，走标准 otadata 选择 |
| 读失败 / 超时 | 不写、不清，走标准 otadata 选择（降级） |

“消费”语义很重要：里世界崩溃/复位时，不能因为字节还是 `0xA5` 又进里世界。
App 侧还有两道兜底（见 §3），但主保证在 bootloader。

---

## 2. 实现

### 2.1 bootloader（`bootloader_components/main/`）

- 用 IDF 的 **component override**：`bootloader_components/main` 取代
  `components/bootloader/subproject/main`（只复制 `bootloader_start.c`，其余用 IDF 的）。
- `bootloader_after_init` 钩子被去掉（本 override 不依赖 hooks；IDF 默认 main 的
  hook 机制在本项目没用到）。
- I²C 是 **bit-bang**（GPIO47=SCL / GPIO48=SDA，开漏模拟，外部上拉），
  没有链入 `i2c_master`（bootloader 是 NON_OS 构建，FreeRTOS/heap 链不进来）。
- 读/写函数带：
  - SCL 等待上限 100×5 µs = **500 µs**；
  - 9 时钟总线恢复；
  - 失败重试 1 次；
  - 全部失败 → 返回 `RTC_BOOT_NO_REQUEST`，不影响启动。
- `selected_boot_partition()` 里，先算标准 otadata 的 `boot_index`，再在
  factory-reset / test 检查之后读 RTC 请求；有请求就覆盖 `boot_index`。
- 目标分区不存在（`bs->ota[0].offset == 0` / `bs->factory.offset == 0`）时
  清掉无效请求并降级。
- ⚠ 镜像校验**不在 bootloader**：本机 `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON/
  ALWAYS=y`，bootloader 不校验 SHA；所以 App 在写 RTC 请求前必须自己
  `esp_image_verify()`（见 §2.2）。如果镜像在“App 校验完 → 重启”之间损坏，
  bootloader 仍可能加载失败——这与旧的 otadata 路径风险相同。

### 2.2 App（`main/core/system/fw_switch.c`）

- `fw_switch_to_sync()`：
  1. 用 `esp_image_verify(ESP_IMAGE_VERIFY_SILENT, ...)` **全量只读校验** `ota_0`
     镜像。⚠ 不能只查 4 字节头：本机 `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON/
     ALWAYS=y`，bootloader 自己**不做**镜像校验（见 `esp_image_format.c` 的
     `bootloader_load_image`），损坏镜像会被直接加载；
  2. 写 RTC `0x03 = 0xA5` 并**读回确认**；
  3. 成功 → 直接返回，**完全不碰 otadata**（0 次擦除）；
  4. 失败 → 走原来的 `esp_ota_set_boot_partition()`（擦 1 个 4 KB 扇区）。
- `fw_switch_to_player()`（里世界开机调用）：
  - 先读 otadata 两个 4 KB 扇区的前 32 字节；
  - 全是 `0xFF` → 说明上次走的是 RTC 主路径，**跳过 8 KB 擦除**；
  - 非空 → 才擦（上次是 RTC 失败后的兜底切换）。
- `main/main.c` / `inner_world_app.c` 在 `rtc_init()` 后清一次 `0x03` 作为兜底
  （正常情况下 bootloader 已经消费）。
- 编译开关 `FW_SWITCH_USE_RTC_BOOT`（`fw_switch.c` 顶部）：
  - `1`：RTC 主路径（当前，配套自定义 bootloader）；
  - `0`：纯 otadata 路径（移除自定义 bootloader 时必须改 0）。

---

## 3. 真机实测

### 3.1 正常切换（表 → 里）

```
I (5723) fw_switch: switch next boot -> ota_0 @0x310000 (size 0x280000)
I (5943) fw_switch: RTC boot request 0xA5 written; otadata untouched (no flash erase)
W (5944) app_main: SYNC_AUTOSTART: restarting into inner-world firmware
[boot] rtc03=0xa5 -> ota_0 (inner)
I (410) inner_world_app: === 里世界固件启动：role=sync, running partition=ota_0 ===
I (420) fw_switch: otadata already empty (RTC path), skip erase
```

- `fw_switch_to_sync()`：`5723 → 5943`，**约 220 ms**——这 220 ms 主要是全量镜像
  verify（必须做，见 §2.2），与旧路径相当；**省下来的是 3 次 otadata 扇区擦除**，
  不是切换时间。
- 切换后直接 `esptool read-flash 0x590000 0x20`：**32 字节全 `0xFF`**，
  证明这次切换没有写 otadata。
- 回到表世界：用户实测通过（RTC 字节已消费 + otadata 空 → factory）。

### 3.2 I²C 失败降级

把 bootloader 的 RTC 地址临时改成不存在的 `0x50`（模拟 NACK/超时）：

```
[boot] rtc03 read failed -> otadata fallback
I (607) app_main: running partition: factory (role=player)
```

- 启动照常，只是退化成原 otadata 路径；
- 恢复 `0x51` 后重新烧 bootloader，正常启动。

### 3.3 体积

`check_sizes.py`：

```
Bootloader binary size 0x4100 bytes. 0x1f00 bytes (32%) free.
```

- `0x4100` = 16,640 B；上限 `0x8000-0x2000` = 24,576 B；余 7,936 B。
- 新增代码主要是 bit-bang I²C（读 + 写 + 恢复 + 超时）和 3 条日志字符串；
  相比 hook 版（+832 B）多了写 RTC、清请求、分区判断和日志。

---

## 4. Flash 擦写账

### 4.1 旧路径（otadata）

| 动作 | 擦除 |
|---|---|
| 表世界 `esp_ota_set_boot_partition(ota_0)` | 1 × 4 KB 扇区（+32 B 写） |
| 里世界开机 `fw_switch_to_player()` | 2 × 4 KB 扇区（8 KB 整个 otadata） |
| **一次往返合计** | **3 × 4 KB = 12 KB** |

### 4.2 新路径（RTC 主路径）

| 动作 | 擦除 |
|---|---|
| 表世界写 RTC `0x03` + 读回 | **0**（RTC SRAM，I²C 2 字节） |
| bootloader 读 + 清 `0x03` | **0**（I²C 2 字节） |
| 里世界开机检查 otadata 为空 → 跳过 | **0** |
| **一次往返合计** | **0 次 otadata 擦除** |

### 4.3 还剩什么

- **NVS**：`inner_confirm_ok_cb` 在切换前调用
  `player_controller_save_last_position()` + `config_store_flush_pending(true)`，
  约 2 个 NVS entry（32 B 级），页擦除按 4 KB / ~100 entry 摊销；这是恢复播放现场
  必需的，本次没有改。
- **镜像校验**：旧路径在 `esp_ota_set_boot_partition()` 里做一次 1.5 MB 全量
  SHA 校验（~220 ms）；新路径用 `esp_image_verify()` 做同样的只读校验（也是
  ~220 ms）。**这一步不能省**：本机 `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON/
  ALWAYS=y`，bootloader 不校验镜像。所以新路径的收益是**0 次 flash 擦除**，
  不是更快的切换。
- 结论：**切换机制本身从 3 次扇区擦除降到 0 次**；NVS 仍有少量写，所以不是
  “绝对零 flash 写”。按 10 万次擦除寿命算，旧路径约 3.3 万次往返，新路径的瓶颈
  变成 NVS（高一个数量级以上），日常“偶尔进里世界对时/传歌”完全不用担心磨损。

---

## 5. 失败/降级清单

| 场景 | 行为 |
|---|---|
| RTC NACK（芯片不在/地址错） | 读失败 → otadata 选择 |
| SCL 被拉死/时钟拉伸 | 500 µs 上限 + 9 时钟恢复 + 1 次重试 → otadata |
| App 写 RTC 失败/读回不一致 | 走 `esp_ota_set_boot_partition()`（1 次扇区擦除） |
| `0xA5` 但 `ota_0` 分区不存在 | 清请求 → otadata |
| `ota_0` 镜像损坏 | App 的 `esp_image_verify()` 在写请求前拦截，不切换；若损坏发生在“校验后→重启”之间，bootloader 因 skip-validate 不校验，可能加载失败（与旧路径同样的已知风险） |
| 自定义 bootloader 被换回默认 | **必须把 `FW_SWITCH_USE_RTC_BOOT` 改 0**，否则 App 只写 RTC，默认 bootloader 不认，切换会静默失败（重启回表世界） |
| 里世界崩溃/复位 | bootloader 已消费字节；里世界启动也清一次 → 回 factory |

---

## 6. 未验证 / 风险

- **0x5A（显式要求 factory）路径没有单独真机跑过**：代码与 0xA5 对称，且
  “回到表世界”走的是 otadata 空/消费路径；如果要严格验证，需要在 App 里加一个
  一次性写 `0x5A` 的测试入口。
- **切换瞬间掉电**：RTC 写是 SRAM，不存在“写坏”问题；但 App 在切换前还会
  `config_store_flush_pending` 写 NVS，掉电风险仍在 NVS 那边（与旧路径相同）。
- **自定义 bootloader 与 App 的耦合**：`FW_SWITCH_USE_RTC_BOOT` 是手动契约；
  换回默认 bootloader 时必须改 0。更好的做法是加一个运行时 capability 握手
  （例如 PMU DATA BUFFER 的一个字节），但会多一次 I²C 和“陈旧标志”问题，
  本次没做。
- **hook 支持被去掉**：override 的 `bootloader_start.c` 不再调用
  `bootloader_before_init/after_init`；本项目不用 hooks。若以后要加 hook 组件，
  需要把 hook 调用和 `bootloader_hooks.h` 一起补回。
- **安全启动/OTA 升级**：当前未启用 secure boot / OTA app 更新；若以后启用，
  otadata 与回滚逻辑要重新评估（本方案默认 otadata 永久为空）。
- **日志**：`[boot] rtc03=...` 用 `esp_rom_printf`，不受
  `CONFIG_BOOTLOADER_LOG_LEVEL_NONE` 影响；如果想再省几十字节可以删掉。

---

## 7. 文件与恢复

新增：

- `bootloader_components/main/bootloader_start.c`（IDF 6.1 版本 + RTC 逻辑）
- `bootloader_components/main/CMakeLists.txt`

修改：

- `main/core/system/fw_switch.c`（RTC 主路径 + otadata 跳过 + 开关）
- `main/main.c`（表世界启动清请求字节）
- `main/core/system/inner_world_app.c`（里世界启动清请求字节）

备份（本次实验前）：

- `tempdir/flashbackup/bootloader_override_20261116/boot_and_table_0x2000_0x9000.bin`
- `tempdir/flashbackup/bootloader_override_20261116/otadata_0x590000_0x2000.bin`

恢复默认 bootloader：

1. 删除 `bootloader_components/`；
2. 把 `FW_SWITCH_USE_RTC_BOOT` 改成 `0`；
3. `idfc build` / `idfc flash`（会写回 IDF 默认 bootloader）。
