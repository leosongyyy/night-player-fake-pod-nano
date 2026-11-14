# ESP32-S31 偶发开机 Cache error：BOYA flash 与 IDF 分支预测竞态调查

> 2026-11-14。起因：接入 WiFi / 网络对时后固件从 1.49MB 涨到 2.14MB，
> 开始偶发"开机第一次读 NVS 就 panic（Cache error）"。
> 本文记录排查过程、真实根因、修复方式，以及"自己换 flash 颗粒"的兼容性结论。

---

## 1. 结论先行

1. **不是 BOYA flash 颗粒的问题，也不是大范围擦写本身的问题。**
2. 根因是 **ESP32-S31 双核 + IDF 6.1 的分支预测器 / cache 竞态**
   （espressif/esp-idf issue **#18948**，**IDFGH-18107**）：
   一颗核执行 flash 操作并 suspend cache 时，另一颗核停在
   `spi_flash_op_block_func` 的 IRAM 自旋里，它的分支预测器仍在投机取指，
   一旦投机到已不可访问的 flash 地址就触发 `Cache error` panic。
3. 本项目用的 IDF 6.1.0 快照（HEAD `fff9895c`，2026-08-25）**不含**上游修复。
4. 修复：把整个 `spi_flash` 组件复制到 `components/spi_flash/`，在
   `cache_utils.c` 里对 flash 操作窗口关闭/恢复分支预测（见 §5）。
   打补丁后 **8 次 esptool `--force` 全量重写 + 每次启动、以及连续重启均未复现**。
5. BOYA 专用驱动（`CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP`）与通用驱动**实现完全相同**，
   开它不会修任何东西；之前"开了 BOYA 就好了"是概率巧合，已回退。
6. **换其它品牌同容量 flash 不会规避这个 panic**（它是 SoC/IDF 侧问题）；
   兼容性取决于颗粒的电压/ID/命令集/QE 位与 IDF 驱动，见 §7。

---

## 2. 现象与现场

### 2.1 panic

```
I (445) app_main: boot: cold start (S31, no firmware deep-sleep shutdown)
I (453) app_main: Initialize config store (NVS)
Guru Meditation Error: Core 1 panic'ed (Cache error).
Cache access error
MEPC : 0x2f01040a   # spimem_flash_ll_set_cs_pin (IRAM)
 #0 spimem_flash_ll_set_cs_pin
 #1 spi_flash_hal_device_config
 #2 spiflash_start_core
 #3 spiflash_start_read
 #4 esp_flash_read (address=37248 = 0x9180, NVS 分区)
 #5 nvs::Page::readEntry
 ...
 #15 config_store_init
 #16 app_main
```

- 崩溃发生在 `app_main` 的**第一行** NVS 初始化，新增的 WiFi/时间代码一行都还没执行；
- `esp_flash_read` 会 suspend cache，`spi_flash_hal_device_config` 在 IRAM 中，
  panic 却是 cache access error → 典型的"cache 关闭期间仍访问了外部存储"；
- 偶发：同一份固件有时第一次开机崩、panic 重启后又能正常启动；
- 只在 flash 操作窗口内发生，和 NVS 读、OTA、擦写都有关系，不只"大范围写"。

### 2.2 与"大范围改写 flash"的关系

`esptool write_flash` 的写入范围大 → 擦除/编程时间更长，但这**不是根因**：

- panic 在**读** NVS 时发生，不在写 flash 时；
- 同一颗芯片、同样的擦写，换一份代码布局（WiFi 加入后）就开始出现；
- 上游 issue 的触发条件是"flash 操作 + 另一核自旋"，与写多少字节无关。

更准确地说：**新固件改变了代码布局/分支历史**，让停在 IPC 自旋里的那颗核
更容易投机到 flash；擦写只是把"重新上电/重新启动"这个动作放大了。
所以此前"从未出现"并不代表以前没有隐患，只是没有踩中分支预测器的状态。

---

## 3. 芯片识别

```
esptool --chip esp32s31 -p COM3 flash_id
  Manufacturer: 68
  Device: 4018
  Detected flash size: 16MB
```

- `0x68` = **Boya / BoHong Microelectronics**；
- `0x4018` = **BY25Q128AS**（128Mbit = 16MB），flashrom 的
  `BOYA_BY25Q128AS 0x4018` 与 IDF 的 BOYA probe 掩码
  （`flash_id >> 16 == 0x68`、`(flash_id & 0xFF00) == 0x4000`）都吻合。
- 该颗粒支持标准 SPI / Dual / Quad I/O，4KB 扇区、64KB 块擦除、256B 页；
  具体电压/频率要以料号后缀的数据手册为准（BY25Q128 家族有 1.8V 与 3.3V 版本，
  见 §7.2）。

> 参考：[flashrom flashchips.h（BOYA_ID 0x68 / BY25Q128AS 0x4018）](https://chromium.googlesource.com/chromiumos/third_party/flashrom/+/e223dcf74bc4de10ec1c4335865202b4be1e4b38/flashchips.h#2)、
> [Boya BY25Q128 数据手册（128Mbit SPI NOR）](http://www.yxd163.com/uploadfile/2021/1218/20211218032703116.pdf)。
> 注意：这些是外部资料，仅用于核对 ID/命令集，不当作项目依赖。

---

## 4. 为什么 BOYA 驱动不是原因（关键排除）

对比 IDF 6.1.0 的 `spi_flash_chip_boya.c` 与 `spi_flash_chip_generic.c`：

| 项 | BOYA 驱动 | 通用驱动 | 差异 |
|---|---|---|---|
| probe | 识别 `0x68` / `0x40xx` | 接受所有 ID | 仅识别范围 |
| read / write / erase | `spi_flash_chip_generic_*` | 同名函数 | **无** |
| set_io_mode / get_io_mode | `spi_flash_chip_generic_*` | 同名函数 | **无** |
| read_reg / wait_idle / reset | generic | generic | **无** |
| caps | `UNIQUE_ID`（**无 SUSPEND**） | 对 0x68 也只给 `UNIQUE_ID` | **无** |
| name | `"boya"` | `"generic"` | 仅日志 |

即：**BOYA 驱动只是把 probe 收窄、把日志名字改掉**，对这颗芯片的
读/写/擦/QE 设置行为与通用驱动逐字节相同。因此：

- 打开 `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP` 不可能修复 cache 竞态；
- 之前"开 BOYA 后连续 15 次启动正常"是**概率巧合**（故障本来就是偶发）；
- 回退后开机日志会重新出现
  `Detected boya flash chip but using generic driver`，这是**提示性警告**，
  不是本次 panic 的根因；本项目已按你的要求回退该选项。

---

## 5. 真实根因与修复

### 5.1 根因（IDFGH-18107 / espressif/esp-idf#18948）

IDF 的双核 flash 操作流程（`components/spi_flash/cache_utils.c`）：

1. 发起核通过 IPC 让另一核进入 `spi_flash_op_block_func()`；
2. 另一核在 IRAM 里自旋，等 `s_flash_op_complete`；
3. 发起核 disable 非 IRAM 中断、suspend flash/PSRAM cache，开始读/写/擦；
4. 结束后恢复 cache，通知另一核退出。

问题在步骤 2：**另一核的分支预测器没有关闭**。RISC-V 的分支预测器会继续
投机取指；当步骤 3 把 cache suspend 后，投机取指落到 flash 地址空间就会
触发 cache access-fail，表现为 `Cache error` panic。是否踩中取决于该核
之前执行过什么代码（分支历史）、代码布局、时序——所以是偶发的。

上游 issue 标题即：
"spi_flash: parked hart's branch predictor stays enabled during flash operations;
speculative flash fetch raises 'Cache error' (ESP32-S31) (IDFGH-18107)"。
相关上游修复提交（本项目 IDF 快照中没有）：

- `0f0068f` fix(esp_hw_support): fix branch predictor access flash after cache disabled
- `a3c9d8d` fix/dualcore_branch_predictor_cache_race_v5.5
- `87e3dda` fix(spi_flash): integrate branch prediction control during cache operations

本项目本地 IDF：

```
HEAD fff9895c 2026-08-25 change(version): Update version to 6.1.0
```

`components/spi_flash/cache_utils.c` 的自旋循环里**没有**任何
`esp_cpu_branch_prediction_disable()` 调用；只有 `CONFIG_SPI_FLASH_ROM_IMPL=y`
时 `esp_rom_spiflash.c` 的包装函数才会顺手关分支预测。我们用的是非 ROM
实现（`CONFIG_SPI_FLASH_ROM_IMPL` 未设置），所以中招。

### 5.2 修复：本地 fork `spi_flash` 组件

按你的要求，没有改 `C:\esp`，而是把组件复制进项目：

```
components/spi_flash/          # 从 IDF 6.1.0 复制（去掉了 test_apps/linux/mock/sim）
  cache_utils.c                # 唯一改动：flash 操作窗口内关闭/恢复分支预测
  ...                          # 其余文件与 IDF 一致
```

改动点（`cache_utils.c`）：

1. `spi_flash_op_block_func()`（另一核的自旋）：
   进入时 `esp_cpu_branch_prediction_disable()`，
   `spi_flash_restore_cache()` 之后 `esp_cpu_branch_prediction_enable()`；
2. `spi_flash_disable_interrupts_caches_and_other_cpu()`（发起核）：
   disable cache 前关闭分支预测；
3. `spi_flash_enable_interrupts_caches_and_other_cpu()`：
   restore cache 后恢复分支预测。

用 `#if SOC_BRANCH_PREDICTOR_SUPPORTED` 包住，非 RISC-V/无分支预测的芯片
编译为空操作。文件头有完整中文注释（原因、上游 issue、升级 IDF 时的处理）。

构建验证：`build/debug/compile_commands.json` 已确认编译的是
`<项目>/components/spi_flash/cache_utils.c`（项目组件覆盖了 IDF 同名组件）。

### 5.3 验证

- 打补丁前：4 次"全量重写后首次启动"里 2 次 panic；
- 打补丁后：**8 次 `esptool write_flash --force` 全量重写 + 每次启动全部通过**，
  另加连续重启 10+ 次无 panic；
- 启动内存/功能无回归（`int 111KB / 47KB 最大连续块`，与打补丁前一致）。

> ⚠ 需要长期观察：偶发问题的"未复现"不等于数学证明。建议后续每次
> 大版本烧录后保留串口日志，若再见到 `Cache error`，先确认：
> ① 本地 `components/spi_flash` 是否被正确覆盖（看 `compile_commands.json`）；
> ② IDF 是否已升级到含上游修复的版本（届时可删掉本地 fork）。

---

## 6. 为什么"这次才出现"

- 接入 WiFi/SNTP 后固件 +约 650KB，代码布局、链接顺序、分支历史全变了；
- 分支预测器是**有状态**的：停在 IPC 自旋前的最后一段代码决定了它会投机到哪；
- 新布局让另一核在自旋时更容易把 PC 投机到 flash 侧；
- 这和"BOYA 颗粒""擦除范围"没有因果关系。换一份代码可能又几年不出现，
  但隐患一直在。

---

## 7. 自己焊接播放器：换 flash 颗粒的兼容性

### 7.1 结论

- 换 **同容量（16MB）、同电压、标准 SPI/QIO 的 NOR**，电气上通常可以工作；
- 但**不能解决本次 panic**（SoC/IDF 侧问题），所以不要为了这个 panic 换颗粒；
- 真正要注意的是：**电压、JEDEC ID、QE 位、命令集、封装/引脚、IDF 驱动支持**。

### 7.2 选型检查表

| 项 | 要求 | 说明 |
|---|---|---|
| 容量 | 16MB（128Mbit） | 分区表按 16MB 固定；换大容量要改 `partitions.csv`/`CONFIG_ESPTOOLPY_FLASHSIZE` |
| 电压 | 与板子 flash 供电一致 | BY25Q128 有 1.8V（BL 等）与 3.3V 版本；**焊错电压会不识别/损坏**，以原理图为准 |
| 接口 | 标准 SPI / Dual / Quad I/O | 本项目 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`，需要颗粒支持 QIO 且 QE 位可设 |
| 命令集 | 0x03/0x0B/0xEB 读、0x02 页写、0x20 扇区擦、0x52/0xD8 块擦、0x05/0x35 读状态、0x01 写状态 | IDF 通用驱动按这套命令工作 |
| JEDEC ID | 能被 IDF 识别或落到 generic | 见 7.3 |
| 频率 | ≥80MHz（或按手册降频） | 本项目 flash 80MHz QIO；颗粒手册标称不够时把 `CONFIG_ESPTOOLPY_FLASHFREQ_40M` 打开 |
| 封装/引脚 | SOP8 208mil / WSON 8x6 等与焊盘一致 | 注意 /WP、/HOLD（或 /RESET）引脚处理 |
| 擦除寿命 | ≥10 万次 | 与 BOYA 同级即可 |

### 7.3 IDF 驱动支持（`CONFIG_SPI_FLASH_SUPPORT_*`）

IDF 6.1 的芯片驱动表（`spi_flash_chip_drivers.c`）：

```
ISSI / GD / MXIC / Winbond / BOYA / TH / MXIC_OPI → generic（兜底）
```

- 已支持的厂商：打开对应 `CONFIG_SPI_FLASH_SUPPORT_<VENDOR>_CHIP=y`，
  驱动会处理该厂商的 QE 位、状态寄存器、suspend 等差异；
- 未列出的厂商：落到 **generic**。generic 假设 QE 在
  **SR2 bit1 / SR bit9**（`spi_flash_chip_generic_set_io_mode`），
  读/写/擦命令也是标准集。满足这两点的颗粒通常能用；QE 位不同的颗粒
  可能无法进入 QIO（表现为启动/读取异常），此时应换用有专用驱动的型号。
- **建议**：DIY 时优先选 IDF 明确支持的型号（Winbond W25Q128JV、
  GD25Q128E、MX25L128、ISSI IS25LP128 等），并在 `sdkconfig.defaults` 中
  打开对应驱动；不要只依赖 generic 的"碰巧能用"。
- 不要为了省事同时打开所有驱动：会延长探测时间、增加 IRAM/Flash 占用，
  按实际颗粒二选一即可。

### 7.4 换上非 BOYA 颗粒后还要做什么

1. `idfc build`（Kconfig 改了会自动重配）；
2. 烧录后看串口：`spi_flash: detected chip: <vendor>` 是否与实物一致；
   若是 `generic`，确认 QIO 读写正常（跑一次全片读校验 / OTA 回归）；
3. `esptool flash_id` 核对 JEDEC ID 与容量；
4. 若出现 `Detected ... but using generic driver` 且 QIO 异常，补开对应
   `CONFIG_SPI_FLASH_SUPPORT_*`；
5. 本次的 S31 分支预测补丁与颗粒无关，换颗粒后仍应保留
   `components/spi_flash`。

---

## 8. 复现/验证命令

```powershell
# 芯片 ID / 容量
python -m esptool --chip esp32s31 -p COM3 flash_id

# 强制全量重写（验证"大范围写 + 首次启动"，绕过 esptool 的差异写入）
python -m esptool --chip esp32s31 -p COM3 -b 1500000 `
  --before default-reset --after hard-reset write_flash --force `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x10000 build\debug\FakePodNanoNightPlayer.bin
```

> `--force` 会强制擦写整个 app 区域；NVS 分区不动，用户配置/播放列表不受影响。

---

## 9. 变更清单

- `components/spi_flash/`：IDF 组件 fork，唯一补丁在 `cache_utils.c`；
- `sdkconfig.defaults`：回退 `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP`，加注释说明；
- `ideas_and_exps/WIFI_TIME_SYNC.md` §6.4：改为指向本文；
- `AGENTS.md` / `CHANGELOG.md`：同步纠正"BOYA 修复"的错误结论。

> ⚠ **升级 IDF 时**：先检查上游 `cache_utils.c` 是否已有
> `esp_cpu_branch_prediction_disable()`（issue #18948 的修复）。
> 已合入 → 删除 `components/spi_flash`，恢复用 IDF 组件；
> 未合入 → 保留本地 fork，并按新版本重新对比/移植补丁（不要直接覆盖，
> 上游可能有其它改动）。
