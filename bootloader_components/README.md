# bootloader_components/main —— 自定义 2nd stage bootloader（RTC-only 切换）

用 IDF 的 **component override** 机制覆盖 `components/bootloader/subproject/main`
（组件名必须是 `main`）。作用：在选启动分区之前，用 bit-bang I²C 读
PCF85063A 的 `RAM_byte(0x03)`，按协议决定进 `factory`（表世界）还是 `ota_0`（里世界）。

完整设计、实测日志、体积/flash 擦写账见
[`../ideas_and_exps/RTC_BOOT_PARTITION.md`](../ideas_and_exps/RTC_BOOT_PARTITION.md)。

## 协议

`RAM_byte(0x03)` 是一个**位域**（2026-11-16 起；bootloader 与 App 必须一致）：

| 位 | 含义 |
|---|---|
| `bit1:0` | 分区：`0b01` → `ota_0`（里世界）；其它（`0b00`/保留值）→ `factory` |
| `bit2` | `1` = 本次启动进充电界面（App 消费，bootloader **不碰**） |
| `bit7:3` | 保留 |

| 情况 | 行为 |
|---|---|
| `bit1:0 == 0b01` 且 `ota_0` 存在 | 选 `ota_0`；成功后只清 `bit1:0`（保留 bit2/保留位） |
| `bit1:0 == 0b01` 但槽不存在 | 选 `factory` + 清分区位 |
| `bit1:0` 其它值 | 选 `factory`（`0b00` 就是表世界） |
| 读失败/超时/NACK | 选 `factory`（**不再**回退 otadata：otadata 已不参与启动） |

- **otadata 已完全不参与启动决策**（2026-11-16 全面迁移到 RTC 字节）：
  "回表世界"只需把分区位清 0，**0 次 flash 擦写**；otadata 分区保留但不再读写。

- I²C：SCL=GPIO47 / SDA=GPIO48，开漏模拟，依赖板载外部上拉；地址 `0x51`。
- SCL 等待上限 500 µs、9 时钟总线恢复、失败重试 1 次。
- 目标分区不存在时清请求并降级。
- 镜像校验**不在 bootloader**（`CONFIG_BOOTLOADER_SKIP_VALIDATE_*`），由 App 在写
  请求前用 `esp_image_verify()` 做。

## 体积

`idfc idfpy -B build\player.debug bootloader`：

```
Bootloader binary size 0x4140 bytes. 0x1ec0 bytes (32%) free.
```

上限 `0x8000-0x2000` = 24,576 B；当前 16,704 B。

## 怎么禁用 / 恢复默认 bootloader

1. 删除本目录（`bootloader_components/`）；
2. **同时**把 `main/core/system/fw_switch.c` 的切换实现改回
   `esp_ota_set_boot_partition()` —— 2026-11-16 起本 bootloader 是**必备件**
   （App 只写 RTC 字节，IDF 默认 bootloader 不认，切换会静默失败）；
3. `idfc build`（必要时先删 `build\<cell>\bootloader-prefix\src\bootloader-stamp\bootloader-configure`，
   新增/删除 `bootloader_components` 不会自动触发 reconfigure）；
4. `idfc flash`（会写回 IDF 默认 bootloader 到 0x2000）。

## 注意

- 本 override 去掉了 IDF main 的 `bootloader_before_init/after_init` hooks 调用；
  本项目不用 hooks。若以后要加 hook 组件，需要把 `bootloader_hooks.h` 和调用补回。
- 新增/修改本目录后，`idfc idfpy -B build\<cell> bootloader` 不会自动重新扫描
  组件；删掉 `build\<cell>\bootloader-prefix\src\bootloader-stamp\bootloader-configure`
  再 build。
- `esp_rom_printf` 打印 `[boot] rtc03=...`，不受 `CONFIG_BOOTLOADER_LOG_LEVEL_NONE`
  影响；不需要日志时可删掉以省几十字节。
