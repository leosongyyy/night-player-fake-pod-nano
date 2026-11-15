# 自定义 bootloader 加 I²C 读取：体积实验（2026-11-16）

> **实验性质**：本文记录的是**只加 hook** 的体积基线（只构建、不烧录）。
> hook 版实验代码已在 RTC-only 方案落地后移除（2026-11-16；需要时从 git 历史
> `c07abf7` 取回），本文只保留测量记录。最终落地的方案是
> **整份 override + RTC RAM_byte 选分区**，见
> [`RTC_BOOT_PARTITION.md`](RTC_BOOT_PARTITION.md)（bootloader 16,640 B，余 7,936 B）。
> 环境：ESP-IDF 6.1（`esp32s31`，GCC 15.2.0），`player.debug` / `inner.debug` 两个构建目录各测一遍。

---

## 0. 结论（TL;DR）

| 项目 | 数值 |
|---|---|
| bootloader 分区上限 | `0x8000 - 0x2000` = **24,576 B**（0x6000） |
| 基线 bootloader | **15,376 B**（0x3c10），余 **9,200 B（37%）** |
| 加"bit-bang 读 AXP2101 4 字节" | **16,208 B**（0x3f50），余 **8,368 B（34%）** |
| **体积增量** | **+832 B（+5.4%）**，占分区 3.4% |

**装得下，而且余量很大。** 再叠一层日志或"按这个字节选启动分区"的判断也够；
真正装不下的不是这点代码，而是 **IDF 的 `i2c_master` 驱动链**（见 §4）。

---

## 1. 测了什么

在 bootloader 的 `bootloader_after_init()` 钩子里：

1. 把 **GPIO47(SCL) / GPIO48(SDA)** 配成开漏模拟（外部上拉，板载已有）；
2. 用 bit-bang 发起一次标准 I²C 读：`START → 0x34<<1|W → 0x04 → RESTART → 0x34<<1|R → 读 4 字节 → STOP`；
3. 读到的 4 字节存进 `volatile` 全局（保证不被优化掉），失败置错误码；
4. 带 9 时钟总线恢复和 SCL 等待上限，避免总线卡死时 bootloader 挂住。

选 `0x04~0x07` 是因为它就是 AXP2101 的 4 字节 **DATA BUFFER**（见
[`reference_docs/pmu_axp2101.md`](reference_docs/pmu_axp2101.md) 的寄存器表），
和上一轮"PMU 里跨固件记录一个字节"的讨论对得上。

代码：hook 版已移除；需要时用
`git show c07abf7:ideas_and_exps/experiments/bootloader_i2c/boot_i2c.c` 取回
（同目录的 `CMakeLists.txt` / `README.md` 同理）。

---

## 2. 体积明细

`build/player.debug/bootloader/bootloader.map` 里本组件的 `.text`：

| 符号 | 字节 |
|---|---:|
| `bootloader_after_init`（含被内联的 read/restart 路径） | 538 |
| `i2c_write_byte` | 130 |
| `scl_wait_high` | 44 |
| `i2c_stop` | 40 |
| `scl_low` / `sda_low` | 36 + 36 |
| `i2c_half_delay` | 10 |
| `bootloader_hooks_include`（链接锚点） | 2 |
| **合计（map）** | **≈836** |
| **bin 实际增量** | **832** |

另外 `.sbss` 多 8 B（`g_boot_pmu_data[4]` + `g_boot_pmu_status`），不进 bin。

> 两个构建目录（`player.debug` / `inner.debug`）结果完全一致：
> 都是 `0x3f50`、余 `0x20b0`。bootloader 不受 role 影响。

---

## 3. 怎么加进去的（IDF 机制）

IDF 6.1 的 2nd stage bootloader 支持两种方式：

1. **hooks**（本例）：在项目根建 `bootloader_components/<name>/`，组件里定义
   `bootloader_before_init()` / `bootloader_after_init()`，再定义一个空的
   `bootloader_hooks_include()` 当链接锚点。bootloader 子工程会自动把
   `bootloader_components` 加进 `EXTRA_COMPONENT_DIRS`。
   - 优点：不动 IDF 本体，升级 IDF 不冲突；
   - 缺点：**只能在这两个时机插代码，不能改启动分区选择**（见 §5）。
2. **整份 override**：把 `components/bootloader` 复制进项目（参考
   `examples/custom_bootloader/bootloader_override`），想改哪改哪。
   I²C 代码本身的体积增量还是这 ~0.8 KB，只是基线 bootloader 可能因配置差异
   有几十~几百字节浮动。

本例的组件清单：

```cmake
idf_component_register(SRCS "boot_i2c.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_rom esp_hal_gpio)
```

`esp_hal_gpio` 在 bootloader 构建里本来就有（`bootloader_support` 的 strapping/
factory-reset 会用到），所以没有额外拉入新组件；`esp_rom` 提供 `esp_rom_delay_us`
和 `rom/gpio.h`。

---

## 4. 为什么不用现成的 `i2c_master` / `axp2101.c`

bootloader 是 **NON_OS 构建**（`BOOTLOADER_BUILD=1`、`NON_OS_BUILD=1`），
`esp_driver_i2c` 的 `i2c_master.c` 直接依赖：

- `freertos/FreeRTOS.h`、`freertos/semphr.h`（总线锁/事件组）
- `esp_heap_caps.h`、`esp_intr_alloc.h`、`esp_pm`（电源管理锁）
- `driver/gpio.h`

这些在 bootloader 里要么不存在、要么是 config-only 组件。现有
`main/core/hardware/axp2101.c` 还额外用了 `freertos/queue.h`、`task_psram`、
`esp_log`、`i2c_master_*`，整条链都进不来。所以没有"把驱动加进 bootloader 就完事"
这条路；可行的只有：

- **bit-bang**（本例，+832 B）；
- 或者用 `i2c_ll.h` 自己写硬件 I²C（要自己开时钟、配 GPIO matrix、算 timing、
  建 command list；估计 1~2 KB，本次没做）。

如果以后想在 bootloader 和 App 之间复用 AXP2101 的寄存器语义，可以把
`axp2101.c` 的"寄存器 ↔ 语义"与 I²C 传输层拆开，bootloader 只带一个极小的
read/write 回调；寄存器逻辑本身只有几百字节。

---

## 5. 关键限制：hooks 不能改启动分区

`bootloader_after_init()` 在 `bootloader_init()` 之后、`select_partition_number()`
之前执行，能读 PMU，但**不能影响选哪个 app 分区**——分区选择在
`bootloader_utility_get_selected_boot_partition()` 里，是 bootloader 内部逻辑，
hooks 没有回调点。

所以：

| 目的 | 可行做法 |
|---|---|
| 只是"开机读一个 PMU 字节，给 App 用" | hook 够了；读完写 RTC 保留内存，或干脆让 App 自己用现成 `axp2101` 驱动再读一次（更简单） |
| 根据 PMU 字节决定进 `factory` 还是 `ota_0` | **必须整份 override bootloader**，改 `selected_boot_partition()`；I²C 增量仍是 ~0.8 KB |
| 用 PMU 字节替代 otadata | 同上，但会推翻现有"里世界开机先擦 otadata"的纪律，需单独设计，不建议顺手做 |

把结果交给 App 的"正规"通道是 `bootloader_common_get_rtc_retain_mem()->custom`
（需要 `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE` > 0）；但注意
`bootloader_common_update_rtc_retain_mem()` 会重算 CRC，写入时机/顺序要跟它配合，
否则可能被当成无效结构体重置。更省事的做法是 App 启动后自己读 PMU（I²C0 已经由
`touch_init()` 建好，`battery_init()` 里就是现成的）。

---

## 6. 风险 / 未验证项

- **没有烧录**：时序、ACK、GPIO47/48 在 bootloader 阶段的实际电平都没上真机验证。
  代码是"能编译、能链接、体积可测"，不等于"能跑"。
- **启动时间**：4 字节 @100 kHz 约 0.4 ms，加总线恢复和超时上限，最坏几 ms；
  对开机体验可忽略，但它是 boot 路径上的新增失败点。
- **总线卡死**：代码有 9 时钟恢复 + SCL 等待上限（100×5 µs），失败就放弃读取、
  继续启动；量产版建议保留这个"读取失败不阻塞启动"的语义。
- **GPIO 复用**：bootloader 重配 GPIO47/48 后，App 的 `touch_init()` 会重新建
  I²C0 总线并重配引脚；理论上不冲突，但需要真机确认（尤其触摸初始化前的那段）。
- **日志**：当前 `CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y`，上面 832 B 不含任何日志
  字符串。若为了调试打开 INFO，体积会再涨（日志基础设施 + 字符串），但仍远小于
  8.3 KB 余量。
- **分区表偏移**：bootloader 变大不影响 `CONFIG_PARTITION_TABLE_OFFSET=0x8000`；
  只要不越过 0x8000 就不用动分区表。当前余 8.3 KB，很安全。

---

## 7. 复现（代码已移除）

hook 版代码已不在工作区，不能直接按原步骤复现。需要时从 git 历史取回：

```powershell
git show c07abf7:ideas_and_exps/experiments/bootloader_i2c/boot_i2c.c
git show c07abf7:ideas_and_exps/experiments/bootloader_i2c/CMakeLists.txt
git show c07abf7:ideas_and_exps/experiments/bootloader_i2c/README.md
```

把它们放到 `bootloader_components/boot_i2c/`，再删掉
`build\player.debug\bootloader-prefix\src\bootloader-stamp\bootloader-configure`，
`idfc idfpy -B build\player.debug bootloader` 即可重现 `0x3f50 / 0x20b0 (34% free)`。

> ⚠ 实测踩坑：只 `idfc idfpy ... bootloader` 不会发现新加的
> `bootloader_components/`，必须删掉 `bootloader-configure` stamp（或做一次
> 顶层 `reconfigure`）才会重新扫描组件。

---

## 8. 后续可做

1. 硬件 I²C（`i2c_ll`）版本的体积实测，和 bit-bang 对比；
2. 只读 1 字节 vs 读 4 字节的差异（预计 <100 B）；
3. ~~按 PMU 字节选分区~~：已落地为整份 override + RTC RAM_byte，见
   [`RTC_BOOT_PARTITION.md`](RTC_BOOT_PARTITION.md)；
4. 如果要"传给 App"：RTC retain mem `custom` 字段的写入时机验证。
