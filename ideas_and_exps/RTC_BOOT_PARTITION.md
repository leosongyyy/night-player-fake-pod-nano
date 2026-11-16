# 用 PCF85063A 的 RAM_byte 选启动分区（自定义 bootloader，RTC-only，otadata 已弃用）

> **状态（2026-11-16 第二次改版）：启动分区 100% 由 RTC 字节决定，otadata 彻底退出启动路径。**
> 切换只写 PCF85063A 的 `RAM_byte(0x03)`（一个位域）；RTC 读不到时**直接启动 factory
> （表世界）**，不再回退 otadata。所以「回表世界」是 0 次 flash 擦写。
>
> 目标：不再靠 otadata 的 flash 擦写来切「表世界 / 里世界」，改用 RTC 芯片里那 1 字节；
> 且这一字节现在同时承载"启动进充电界面"的一次性标志（bit2）。
>
> 相关：只做 hook 的体积实验见 [`BOOTLOADER_I2C_SIZE.md`](BOOTLOADER_I2C_SIZE.md)；
> 本文是「整份 override」版本。代码在 `bootloader_components/main/`。

---

## 0. 结论（TL;DR）

| 项目 | 结果 |
|---|---|
| bootloader 体积 | **16,720 B（0x4150）**，上限 24,576 B，余 **7,856 B（32%）** |
| 相比默认 bootloader | +1,344 B（默认 15,376 B） |
| 切换方式 | RTC `0x03` 的**位域**：`bit1:0` = 分区（`0b01` → `ota_0` 里世界，其它 → `factory` 表世界），`bit2` = 本次启动进充电界面，`bit7:3` 保留 |
| 读完处理 | bootloader **只清分区位**（`bit1:0`），`bit2` 与保留位留给 App 消费 |
| 失败降级 | NACK / 超时 / 读回不一致 / 目标分区不存在 → **选 factory**；镜像坏了 → IDF 自带的 factory 回退 |
| 一次往返的 otadata 擦除 | **0 次**（otadata 已不参与启动；旧路径 3 次 × 4 KB） |
| `fw_switch_to_sync()` 耗时 | **~220 ms，与旧路径相当**（仍做 1.5 MB 全量镜像校验；省的是擦除，不是时间） |
| 仍存在的 flash 写 | NVS 播放现场（`last_song`/`last_pos` 等，约 2 个 entry，页擦除摊销） |

**能减少，而且减少的正是切换路径上最“硬”的那几次扇区擦除**；NVS 的播放现场保存
仍在（那是恢复播放所必需的），所以不是“整个切换 0 flash 写”。

> **⚠ 自定义 bootloader 现在是必备件**：`FW_SWITCH_USE_RTC_BOOT` 开关已删除。换回 IDF
> 默认 bootloader 会让 App 的切换静默失效（它只认 otadata），那时必须把
> `fw_switch.c` 改回 `esp_ota_set_boot_partition()`。

---

## 1. 协议

PCF85063A 的 `0x03` 是 NXP 定义的 free RAM byte（复位 `0x00`，SR/掉电才清）。
电池常驻时它跨 SoC 复位、跨固件保持。**2026-11-16 起整个字节改成一个位域**
（原来用 `0xA5`/`0x5A` 两个魔法值把整字节占满，再加"启动进充电界面"就没地方了）：

| 位 | 含义 |
|---|---|
| `bit1:0` | 分区选择：`0b00` = `factory`（表世界，默认）、`0b01` = `ota_0`（里世界）；`0b10/0b11` 保留未用（读到按无效处理 → factory） |
| `bit2` | `1` = 本次启动进充电界面（表世界"插着充电器关机"时置位，App 消费后立刻清 0） |
| `bit7:3` | 保留（当前恒 0，留给以后的一次性短消息） |

bootloader 行为（2026-11-16 起**不再看 otadata**）：

| 读到的值 | bootloader 行为 |
|---|---|
| `bit1:0 == 0b01` 且 `ota_0` 存在 | 选 `ota_0`（里世界）；成功后写回 `值 & ~0b11`（**保留 bit2/保留位**） |
| `bit1:0 == 0b01` 但槽不存在 | 选 `factory`；清分区位 |
| `bit1:0 == 0b00` / `0b10` / `0b11` | 选 `factory`（`bit2` 不动，留给 App） |
| 读失败 / 超时 | 选 `factory`（表世界是主固件；这是比"按陈旧 otadata 启动"更安全的降级） |

“消费”语义很重要：里世界崩溃/复位时，不能因为分区位还在又进里世界。
App 侧还有兜底（见 §3），但主保证在 bootloader。

> ⚠ **兼容性**：旧固件写的 `0xA5 = 0b1010_0101` 在新协议里 = 分区 `0b01`（里世界）
> + `bit2=1`（充电界面）。实际不会同时生效——bootloader 先按分区进 `ota_0`，
> bit2 由里世界开机时 `fw_switch_clear_boot_flags()` 清掉。
> `0x5A`（旧"显式回 factory"值）在新协议里 = `bit1:0 = 0b10`（保留值）→ 同样选
> `factory`，语义没变。

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
- `selected_boot_partition()` 里，factory-reset / test 调试检查之后**直接按 RTC 字节
  决定**（不再先算 otadata 的 `boot_index`）；RTC 读失败才用 factory 兜底。
- 目标分区不存在（`bs->ota[0].offset == 0` / `bs->factory.offset == 0`）时
  清掉无效请求并降级到 factory。
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
  2. 写 RTC `0x03` 的分区位（读-改-写：`(旧值 & ~0b11) | 0b01`，保留 bit2/保留位）
     并**读回确认**该字段；
  3. 成功 → 返回，**完全不碰 otadata**（0 次擦除）；
  4. 写失败/读回不一致 → 返回错误（**没有 otadata 兜底了**）：调用方提示
     "里世界固件不可用"，不再出现"RTC 与 otadata 两套状态打架"。
- `fw_switch_to_player()`（里世界开机调用）：
  - 只把分区位清 0（读-改-写，保留 bit2/保留位），**不读也不擦 otadata**；
  - RTC 读不到 → 直接返回 ESP_OK（bootloader 同样会回 factory，降级方向一致）。
- 启动标志（同一字节的 bit2，见 `fw_switch.h`）：
  - `fw_switch_request_charge_ui()`：表世界"插着充电器关机"时置位，随后 `esp_restart()`；
  - 表世界开机在 `rtc_init()` 之后**立刻** `fw_switch_charge_ui_requested()` +
    `fw_switch_clear_charge_ui_request()` 消费（进充电界面），末尾再
    `fw_switch_clear_boot_flags()` 兜底清分区位/旧值；
  - 里世界开机只 `fw_switch_clear_boot_flags()`（不实现充电界面）。
- 编译开关 `FW_SWITCH_USE_RTC_BOOT` **已删除**：自定义 bootloader 是必备件。

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
| RTC NACK（芯片不在/地址错） | bootloader 读失败 → **选 factory**（表世界） |
| SCL 被拉死/时钟拉伸 | 500 µs 上限 + 9 时钟恢复 + 1 次重试 → 仍失败则 factory |
| App 写 RTC 失败/读回不一致 | 返回错误，"里世界固件不可用"提示；**不再写 otadata** |
| 分区位 `0b01` 但 `ota_0` 分区不存在 | 清分区位 → factory |
| `ota_0` 镜像损坏 | App 的 `esp_image_verify()` 在写请求前拦截，不切换；若损坏发生在“校验后→重启”之间，bootloader 因 skip-validate 不校验，可能加载失败（与旧路径同样的已知风险） |
| 自定义 bootloader 被换回默认 | App 的切换会静默失效（默认 bootloader 只认 otadata）——必须同时把 `fw_switch.c` 改回 `esp_ota_set_boot_partition()` |
| 里世界崩溃/复位 | bootloader 已消费分区位；里世界启动也清一次 → 回 factory |
| otadata 里残留旧值 | **无影响**：bootloader 不再读 otadata |

---

## 6. 未验证 / 风险

- **显式要求 factory 的 RTC 路径已删除**（原 `0x5A`）：分区位只留
  `0b00/0b01`，且 `0b00` 本身就是 factory——"回表世界"只需清分区位，**不擦 otadata**。
  `0x5A` 在新协议里落在保留值 `0b10`，同样落到 factory。
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
