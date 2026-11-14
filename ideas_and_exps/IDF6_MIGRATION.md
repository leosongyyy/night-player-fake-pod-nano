# 从 ESP-IDF 5.5.5 迁移到 6.1（2026-11）

> 📅 本文记录 **S3 时代（1.x / 2026 年）**的迁移实测与结论，其中的芯片参数、构建矩阵、
> 回归清单不代表 S31（2.x）现状；S31 现状见 AGENTS.md。⚠ §9.1 回归清单里的
> 「低电压拒启」在 S31 上已不存在（仅适用于 `legacy_s3`），「伪关机」也已改为 PMU 真断电。

> 本文记录 FakePodNanoNightPlayer 从 **ESP-IDF 5.5.5** 迁移到 **ESP-IDF 6.1**（本机
> `C:\esp\v6.1\esp-idf`，GCC 15.2.0 工具链）的完整过程：读了哪些迁移指南、实际踩到
> 了哪些坑（含**两次真机崩溃**）、改了什么、怎么验证的，以及后续要盯的点。
>
> 范围约定：**本次不加入任何 ESP32-S31 相关内容**（用户明确要求先把现有 S3 工程在新
> 框架下跑通）。S31 是后续独立任务。

---

## 1. 结论速览

| 项目 | 结果 |
|---|---|
| 迁移后 IDF | ESP-IDF **v6.1**（`C:\esp\v6.1\esp-idf`，GCC 15.2.0） |
| idfc 环境 | `.idfc/idfc_config.json` 的 `IdfActivationScript` 已指向 `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |
| 编译期阻断点 | **3 个**：① LVGL 的 `IRAM_ATTR` 段号冲突（GCC 15 把警告升级成错误）；② `esp_lcd_new_panel_io_i2c()` 在 6.0 起只接受新 I2C 驱动的总线句柄；③ `CONFIG_COMPILER_ORPHAN_SECTIONS` 默认值收紧（本项目早已设为 `PLACE`，未爆） |
| **运行期阻断点（真机崩溃）** | **1 个**：迁移后新 I2C 驱动要求 `esp_lcd_panel_io_i2c_config_t.scl_speed_hz > 0`，而本地触摸驱动的 `ESP_LCD_TOUCH_IO_I2C_*_CONFIG()` 宏是 5.x 时代写的、没这个字段 → 值为 0 → `ESP_ERROR_CHECK` abort → **开机 panic 重启循环**（见 §5） |
| 主动做的迁移 | 旧 I2C 驱动（`driver/i2c.h`，v6.0 起 EOL、v7.0 移除）→ 新驱动 `driver/i2c_master.h`（触摸 + CS43131 两条链路） |
| 全项目 Kconfig 改名 | **只有 1 处**（`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` → `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`） |
| `main/Kconfig.projbuild` 写法 | **零改动**（kconfig v3 语法变更一条都没踩到） |
| 三套配置构建 | debug / releasecandidate / release **全部通过**（§8.1） |
| 真机功能验证 | ✅ 完整跑通：I2C 扫描 → 触摸 → CS43131 → SD(exFAT) 挂载 → **8990 首**曲库扫描 → 写播放列表分区 → 进正常主界面 → 音频/封面任务运行，全程无 abort/重启（§8.2） |

⚠ 一个容易误判的点：**迁移后设备"崩溃重启"时，第一嫌疑不一定是 LVGL 那个警告**。
本次实测的崩溃根因是 I2C，与 LVGL 无关；LVGL 那个警告经 map/反汇编核对**不改布局**
（详见 §4）。排查时请以串口日志的 `ESP_ERROR_CHECK failed` / `abort()` 调用栈为准。

---

## 2. 读过的迁移指南与实际相关性

本机文档在 `C:\esp\v6.1\esp-idf\docs\zh_CN\migration-guides\release-6.x\`：

- `6.0\`：13 份（`build-system` / `networking` / `peripherals` / `protocols` /
  `provisioning` / `security` / `storage` / `system` / `toolchain` / `tools` /
  `wifi` / `bluetooth-classic` / `index`）
- `6.1\`：**只有 3 份**（`index` / `peripherals` / `tools`）—— 6.1 没有网络、协议、
  安全、Wi-Fi、配网、蓝牙章节，**这些领域的破坏性变更全在 6.0 里**，不会漏。

因为项目是**纯本地 MP3 播放器**（不用 WiFi/蓝牙/网络协议栈/TLS/MQTT/OTA），实际相关的
只有下面几块：

| 指南 | 对本项目的相关性 |
|---|---|
| `6.0\peripherals.rst` | ⭐ 相关：`driver` 组件不再公共依赖 18 个 `esp_driver_*`；旧 I2C 驱动 EOL；**`esp_lcd_new_panel_io_i2c` 只认新 I2C**；驱动头文件不再隐式包含 FreeRTOS 头 |
| `6.0\system.rst` | ⭐ 相关：默认 LibC 由 Newlib 改 **PicolibC**；FreeRTOS/ringbuf 函数默认位置由 IRAM 改 **flash**；`esp_sleep_get_wakeup_cause()` 弃用（改 bitmap 版） |
| `6.0\build-system.rst` | ⭐ 相关：孤立段从"容忍"变**报错**（默认值改了）；`CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` 默认改为 **n**（警告变错误）；全局构造函数顺序由降序改升序；kconfig v3 语法变更 |
| `6.0\toolchain.rst` | ⭐ 相关：GCC 14.2 → **15.1+**（本机 15.2.0），新警告 `-Wunterminated-string-initialization` / `-Wheader-guard` 等 |
| `6.0\storage.rst` | 相关：`esp_vfs_fat_sdmmc_unmount` 移除（本项目未用）；`esp_vfs_console` → `esp_stdio`；FATFS 动态缓冲默认开 |
| `6.0\tools.rst` | 相关：CMake 最低 3.22.1；`idf.py size --legacy` 移除；Python 3.9 弃用 |
| `6.1\tools.rst` | ⭐ 相关：`idf.py flash` 默认变成**快速重烧**（有 `*_flashed.bin` 时只写变化区域，实测生效） |
| `6.1\peripherals.rst` | 不相关（只涉及 MIPI DPI 面板回调与 UART 唤醒新 API） |
| `6.0\networking / protocols / security / wifi / provisioning / bluetooth-classic` | **零强制改动**：涉及的组件本项目一个都没显式依赖，且 esp_netif / lwip / esp_wifi / mbedtls / bt / nvs_flash / esp_event / esp_http_client 这 8 个组件**在 6.1 里名字都没变** |

---

## 3. 编译期坑 ①：`esp_lcd_new_panel_io_i2c()` 只认新 I2C 驱动

### 现象（必然编译失败的一处）

```c
// 旧写法（IDF 5.5 可用，靠 _Generic 分发到 _v1/_v2 两个实现）
ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST,
                                         &tp_io_config, &tp_io_handle));
```

IDF 6.1 的 `esp_lcd_io_i2c.h` **只剩一个签名**
`esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus, ...)`，而
`esp_lcd_i2c_bus_handle_t` / `esp_lcd_new_panel_io_i2c_v1` / `_v2` 在 6.1 里
**已完全不存在**。

### 顺带把整条 I2C 链路迁到新驱动

既然必须动，就把旧驱动彻底换掉（`driver/i2c.h` 在 v6.0 已 **EOL**，只打
`#pragma message` 严重警告，**v7.0 会移除**）。新旧驱动模型差异：

| | 旧驱动 `driver/i2c.h` | 新驱动 `driver/i2c_master.h` |
|---|---|---|
| 寻址 | 端口号 `I2C_NUM_0`，每次传输现填从设备地址 | **总线句柄** + **每设备句柄**（地址绑定在设备句柄上） |
| 建总线 | `i2c_param_config()` + `i2c_driver_install()` | `i2c_new_master_bus()` |
| 加设备 | 无此概念 | `i2c_master_bus_add_device()` |
| 读寄存器 | `i2c_master_write_read_device(port, addr, …)` | `i2c_master_transmit_receive(dev, …)` |
| 写寄存器 | `i2c_master_write_to_device(port, addr, …)` | `i2c_master_transmit(dev, …)` |
| 探测 | 手搓 `i2c_cmd_link_create` + start/write_byte/stop + `i2c_master_cmd_begin` | `i2c_master_probe(bus, addr, timeout)` |
| 超时 | `i2c_set_timeout()` 配硬件超时 | 每次传输传 `xfer_timeout_ms` |
| 释放 | `i2c_driver_delete(port)` | `i2c_del_master_bus(bus)`（连带销毁总线上所有设备句柄） |

改动落点（保持原有分层与"触摸建总线、DAC 挂上去"的既有架构不变）：

- `main/core/hardware/touch.c/.h`
  - `i2c_new_master_bus()` 建 `I2C_NUM_0` 总线（200kHz、内部上拉、`glitch_ignore_cnt=7`）；
  - 扫描改用 `i2c_master_probe()`；
  - 新增 **`touch_get_i2c_bus()`** 把总线句柄公开出去（CS43131 要用）；
  - `touch_power_off()` 改用 `i2c_del_master_bus()`；
  - 删除 `i2c_set_timeout(TOUCH_HOST, 0x02)`（新驱动按传输传超时，无对应物）。
- `main/core/hardware/audio/cs43131.c/.h`
  - 用 `touch_get_i2c_bus()` + `i2c_master_bus_add_device()` 把自己的设备句柄挂上总线
    （`cs43131_bus_attach()`，幂等）；
  - `cs43131_write_reg` / `cs43131_read_reg` 改用 `i2c_master_transmit` /
    `i2c_master_transmit_receive`（超时 `CS43131_I2C_TIMEOUT_MS = 200`，
    对应旧代码的 `pdMS_TO_TICKS(200)`）；
  - 探测改为 `i2c_master_probe()` + 逐候选地址 attach/读 ID/失败就 `rm_device`；
  - 新增 **`cs43131_bus_detach()`**：把设备句柄从总线上摘掉。
- `main/core/utils/fake_shutdown.c`
  - 关机序列里 `cs43131_bus_detach()` **必须排在 `touch_power_off()` 之前** ——
    否则 `i2c_del_master_bus()` 销毁总线后 `s_i2c_dev` 变成**悬垂句柄**，唤醒重启时
    `cs43131_bus_attach()` 会误判"已挂载"而复用失效句柄。
- **删除 `components/i2c_bsp/`**：全工程零引用（只有它自己 include 自己）的死代码，
  而且它那段 `driver/i2c.h` 用法本来就被 `#ifndef EXAMPLE_USE_TOUCH` 整段编掉。
  用 `git rm -r components/i2c_bsp` 删除，保留可追溯性。

⚠ 新旧 I2C 驱动**混用同一端口会触发 IDF 的 legacy 冲突检查**（有
`CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK` 可绕过，明确不推荐）。本次是彻底切换，
不存在混用。

---

## 4. 编译期坑 ②：LVGL 的 `IRAM_ATTR` 段号冲突

### 现象

```
lvgl/src/misc/lv_color.c:317:1: error: ignoring attribute 'section (".iram1.4")'
because it conflicts with previous 'section (".iram1.1")' [-Werror=attributes]
```

`lv_color_op.c` / `lv_math.c` 同样报，特征都是**头文件里声明、`.c` 里定义**的函数。

### 根因（两处变更叠加，而 LVGL 那边是"漏改"而非设计）

1. **IDF 6.1 把 `IRAM_ATTR` 改成带 `__COUNTER__` 的唯一子段**
   （`esp_common/include/esp_attr.h`）：
   ```c
   #define _SECTION_ATTR_IMPL(SECTION, COUNTER) \
       __attribute__((section(SECTION "." _COUNTER_STRINGIFY(COUNTER))))
   #define IRAM_ATTR _SECTION_ATTR_IMPL(".iram1", __COUNTER__)
   ```
   目的是"多个函数各自落在独立子段里，`--gc-sections` 才能单独回收"。
   于是同一个函数在**声明**处展开成 `.iram1.1`、在**定义**处展开成 `.iram1.4`。
2. **IDF 6.1 起 `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` 默认 n**，GCC 15 的
   `-Wattributes`（*later declaration with section attribute must match previous*）
   从警告升级为错误。

LVGL 侧的问题本质：`LV_ATTRIBUTE_FAST_MEM`（= `IRAM_ATTR`）**同时出现在声明和定义上**。
这本就是冗余——[lvgl#4391](https://github.com/lvgl/lvgl/issues/4391) 早已定下约定
"属性只留在**定义**与头文件里的 `static inline` 定义上，**不留在声明上**"，绝大多数地方
都遵守了，只有几个函数漏改。铁证：同一个头文件里

```c
int32_t /* LV_ATTRIBUTE_FAST_MEM */ lv_trigo_sin(int16_t angle);   // lv_math.h:53  已按约定
int32_t LV_ATTRIBUTE_FAST_MEM lv_trigo_cos(int16_t angle);         // lv_math.h:55  漏改
```

### 中间插曲：先误判，再靠证据纠正

我最初的处置是**只把 `-Wattributes` 降级为警告**（`-Wno-error=attributes`），
理由是"段号不一致在链接上无害"。用户随即反馈：**"我记得之前遇到过这个问题，忽略警告
的结果就是烧录的固件会一直崩溃重启"**。

于是做了两项客观核查：

1. **map / nm / objdump**：`lv_color_16_16_mix`、`lv_color_mix`、`lv_trigo_cos`
   都落在 `.iram0.text`（IRAM），地址 `0x403793e4` / `0x40380588` / `0x40378120`；
   在目标文件里它们属于 `.iram1.1`——即 **GCC 丢弃的是"定义处那个冲突的段名"，
   保留了"第一个声明处的段名"，函数仍进 IRAM**。
2. **上游查证**（本机访问不了 github.com，由用户代抓）：LVGL
   [PR #10737](https://github.com/lvgl/lvgl/pull/10737) *"fix(attributes): remove
   LV_ATTRIBUTE_FAST_MEM from declarations"* 的原文确认了机制：
   > On ESP-IDF the attribute expands to `IRAM_ATTR`, which is built on `__COUNTER__`,
   > so the two occurrences request different sections for the same symbol and the
   > compiler rejects it. … This restores the convention agreed in #4391 …
   > **No definition is changed, so the functions are still placed in fast memory
   > where the option is enabled.**

结论：**这是个该修的上游疏漏，正规修法是"去掉声明上的属性"，零性能损失**。于是放弃
警告抑制，改为构建期应用该补丁（§4.1）。同时确认：用户记忆里的"崩溃重启"在本次迁移中
**确实发生了，但根因是 I2C（§5），不是这个警告**。

### 4.1 补丁的落地方式（构建期自动应用）

LVGL 是 `managed_components/` 下的**托管组件**，直接改源码会在下次组件管理器更新时被
覆盖；而本机 idf-component-manager **3.1.2 不支持**原生的 `patches/` 机制（已实测搜索，
包内无任何 patch 相关代码）。故采用"**补丁文件入库 + CMake 配置期幂等回写**"：

```
patches/lvgl_fast_mem_decl/src/misc/lv_color.h       ← 修好的整份文件（与组件目录同构）
patches/lvgl_fast_mem_decl/src/misc/lv_color_op.h
patches/lvgl_fast_mem_decl/src/misc/lv_math.h
```

`main/CMakeLists.txt` 在配置阶段逐文件比对内容，**不一致才回写**（幂等，不会造成无谓重编）：

```cmake
set(_lvgl_patch_files
    "src/misc/lv_color.h" "src/misc/lv_color_op.h" "src/misc/lv_math.h")
set(_lvgl_patch_root  "${CMAKE_CURRENT_SOURCE_DIR}/../patches/lvgl_fast_mem_decl")
set(_lvgl_component   "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl")
foreach(_rel ${_lvgl_patch_files})
    ... file(READ) 比对 → file(WRITE) ...
endforeach()
```

**鲁棒性已实测**：手动把 3 个文件还原成未打补丁状态（模拟组件管理器重新解包），再跑
`idfc build --reconfigure` → 日志出现
`已应用 LVGL 补丁（PR #10737，修复 IRAM_ATTR 段号冲突）：回写 3 个文件`，构建通过，
固件大小与补丁版**完全一致**（`0x1c7a80`）。

**补丁实际覆盖 3 处**（PR 列了 6 处，另 3 处对本项目不适用）：

| PR 里的函数 | 文件 | 本项目情况 |
|---|---|---|
| `lv_color_16_16_mix` | `src/misc/lv_color.h` | ✅ 已改 |
| `lv_color_mix` | `src/misc/lv_color_op.h` | ✅ 已改 |
| `lv_trigo_cos` | `src/misc/lv_math.h` | ✅ 已改 |
| `mask_only_image_blend` | `src/draw/sw/blend/lv_draw_sw_blend_to_a8.c` | ❌ 该文件/符号在 LVGL 9.4.0 里**已不存在** |
| `drawbuf_next_row` ×2 | `src/draw/sw/blend/neon/*.c` | ❌ ARM NEON 专用，xtensa 构建不编译 |

**验证补丁不损失性能**：打完补丁后 3 个函数地址与补丁**前完全一致**
（`0x403793e4` / `0x40380588` / `0x40378120`，均在 `.iram0.text`），
`.iram0.text` 大小 `0x1833f`（≈96KB）。即 `--gc-sections` 粒度与 IRAM 布局都没变，
而警告真正消失了（不是被抑制）。

> 上游 **9.6 仍未合入该 PR**（2026-11 查证），故暂时无法靠升级解决。日后 LVGL 版本
> 合入该修复后，本段会自动变成空操作（比对后内容相同），届时可整段删除。

---

## 5. 运行期坑（真机崩溃）：新 I2C 驱动要求 `scl_speed_hz > 0`

### 现象

I2C 迁移完成后三套配置都能编译通过，但**烧录后开机即 panic 重启循环**：

```
I (776) touch: Scanning I2C bus...
I (783) touch: Found I2C device at address 0x15 / 0x30 / 0x6b / 0x7e   ← 总线是好的
E (805) i2c.master: i2c_master_bus_add_device(1181): invalid scl frequency
E (813) lcd_panel.io.i2c: esp_lcd_new_panel_io_i2c(72): i2c add device fail
ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG) at 0x42046381
  --- touch_init at main/core/hardware/touch.c:134
  expression: esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle)
abort() was called at PC 0x40386aaf on core 0
Rebooting...
```

### 根因

IDF 6.1 给 `esp_lcd_panel_io_i2c_config_t` **新增了 `scl_speed_hz` 字段**，并且
`esp_lcd_new_panel_io_i2c()` 会把它**原样转交**给新 I2C 驱动的
`i2c_master_bus_add_device()`，而后者有硬校验：

```c
// components/esp_driver_i2c/i2c_master.c:1181
ESP_RETURN_ON_FALSE(dev_config->scl_speed_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid scl frequency");
```

本地触摸驱动的 `ESP_LCD_TOUCH_IO_I2C_*_CONFIG()` 宏是 5.x 时代手写的、**没有这个字段**
（5.5 的结构体也没有），宏展开后该成员 = 0 → 校验失败 → `ESP_ERROR_CHECK` abort。
**这不是可省略的默认值**：新驱动把它当成"该设备每次传输时要切换到的 SCL 频率"，
所以必须与总线同速。

### 处置

在调用点覆盖（而不是改 3 个驱动头文件里的宏，位置更集中、不动上游宏文件）：

```c
esp_lcd_panel_io_i2c_config_t tp_io_config = TOUCH_IO_I2C_CONFIG();
tp_io_config.scl_speed_hz = 200 * 1000;   // 与 i2c_new_master_bus 的总线速率一致
ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle));
```

改完重新烧录，设备完整启动（见 §8.2）。

### 教训

**"三套配置都编译通过"完全不能替代真机验证。** 本次两个坑里，纯编译只能发现一个
（§4 的段号冲突），另一个（§5）只有烧到板子上才暴露。迁移类工作必须编译 + 烧录 + 看日志。

---

## 6. 其他改动（都是小项）

### 6.1 Kconfig 改名（全项目仅 1 处）

```diff
-CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
+CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
```

依据：`components/freertos/sdkconfig.rename`。旧名在 6.1 会被自动映射（不会报错），
但生成 sdkconfig 时会打警告，故直接改用新名。

> 核对方法（值得复用）：把 IDF 6.1 里**全部** `sdkconfig.rename*` 的映射对抽出来，
> 与项目所有 `sdkconfig.defaults*` / `sdkconfig.<维度>.<类型>` 里的 `CONFIG_` 名求交集。
> 本项目命中 **1** 处（其余 178 处命中全落在**生成的** `sdkconfig` 的"废弃项兼容块"里，
> 那份文件是产物、会被重新生成，无需理会）。

### 6.2 孤立段处理：`PLACE` → `WARNING`

IDF 6.0 起该 choice 的默认值从"容忍"收紧为 **ERROR**。本项目原来设的是
`CONFIG_COMPILER_ORPHAN_SECTIONS_PLACE=y`（静默放置）。迁移时改成
`CONFIG_COMPILER_ORPHAN_SECTIONS_WARNING=y`：**行为相同（仍然放置、不会链接失败），
但会把是哪些段打印出来**，便于发现真正的内存映射问题（这正是 IDF 文档不推荐 `PLACE`
的原因）。实测链接无任何孤立段告警；若嫌吵可换回 `PLACE`。

### 6.3 `cmake_minimum_required` 3.5 → 3.16

根 `CMakeLists.txt`。IDF 6.1 要求 ≥3.16，旧值在 CMake 4.x 下会打
*"Compatibility with CMake < 3.10 will be removed"* 弃用警告。

### 6.4 唤醒原因改用 bitmap 版 API

`main/main.c`：`esp_sleep_get_wakeup_cause()` 在 6.0 起弃用（多唤醒源同时命中时只返回
其中一个），改为：

```c
uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
if (wakeup_causes == 0) { /* 冷启动 */ }
else if (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) { /* B 键唤醒 */ }
```

本项目伪关机只注册了 EXT1，语义与旧写法完全等价。
⚠ 顺带发现：`esp_sleep_get_wakeup_causes()` 对**冷启动**返回的是 `BIT(0)`（=1）而不是
0，所以日志里冷启动会打印 `W (361) boot: woken by unexpected source (causes=0x00000001)`。
这不影响功能（只是日志措辞），但如果想让日志更准确，可把判据改成
`if (causes == BIT(ESP_SLEEP_WAKEUP_UNDEFINED))` 之类 —— 属可选的打磨项，本次未改。

### 6.5 `esp_lcd_touch_get_coordinates()` → `esp_lcd_touch_get_data()`

`main/core/ui/lvgl_port.c`。`esp_lcd_touch` 1.2.1 已把前者标记为弃用（2.0.0 移除），
新 API 的错误码从 `bool` 变成 `esp_err_t`，坐标放在 `esp_lcd_touch_point_data_t` 数组里：

```c
esp_lcd_touch_point_data_t tp_data[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
uint8_t touchpad_cnt = 0;
bool touched = false;
if (esp_lcd_touch_get_data(tp, tp_data, &touchpad_cnt,
                           CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK &&
    touchpad_cnt > 0) {
    touchpad_x = tp_data[0].x;
    touchpad_y = tp_data[0].y;
    touched = true;
}
```

### 6.6 `.gitignore` 补 `.idfc/`

`.idfc/idfc_config.json`（激活脚本路径、默认串口、当前构建类型/overlay 选择）是**每台
机器/每次本地操作**的状态，不入库。

### 6.7 ⚠ 一个**不在 git 里**的必要前置改动：IDF 本体的 exFAT 支持

项目支持 exFAT 格式的 SD 卡，而 ESP-IDF 的 FatFs 默认**关掉 exFAT**。这个改动在
**IDF 安装目录**（`C:\esp\v6.1\esp-idf`，本身是个 git 仓库），**不在本工程仓库里**，
所以换电脑/重装 IDF 时**必须重新打**：

```diff
# C:\esp\v6.1\esp-idf\components\fatfs\src\ffconf.h
-#define FF_FS_EXFAT		0
+#define FF_FS_EXFAT		1
```

（该文件原注释亦说明：启用 exFAT 要求 `FF_USE_LFN >= 1`，本项目已满足。）
未打该补丁的症状：`vfs_fat_sdmmc: failed to mount card (13)` → 进"无卡"界面。
本次实测该补丁生效后挂载正常（§8.2）。

---

## 7. 核查过但**确认不需要改**的项（避免下次重复排查）

### 代码层

- **所有驱动头文件在 6.1 仍可原样 include**（路径变了但 include 名不变）：
  `driver/gpio.h`、`driver/spi_master.h`、`driver/i2s_std.h`、`driver/sdmmc_host.h`
  （6.1 在 `esp_driver_sdmmc/legacy/include`，仍是 public include dir）、
  `esp_adc/adc_oneshot.h` + `adc_cali*`、`esp_lcd/*`、`sd_protocol_defs.h`、
  `sdmmc_cmd.h`、`esp_vfs_fat.h`、`esp_memory_utils.h`、`esp_rom_crc.h`、
  `freertos/idf_additions.h`。
- **`main/CMakeLists.txt` 的 3 处链接 hack 在 6.1 下依旧成立**（已由三套构建 + 真机实测）：
  `-Wl,-u,lv_malloc_core`、FreeType 接线（`target_link_libraries(lvgl PUBLIC
  idf::espressif__freetype)` / `FT2_BUILD_LIBRARY` /
  `FT_CONFIG_OPTIONS_H=<…ftoption_lvgl.h>`）、以及
  `-Wl,-u,FT_New_Memory/-u,FT_Done_Memory/-u,FT_Stream_Open`。
- `main` 组件**不写 `REQUIRES`** 仍可用全部组件：IDF 对 `main` 的"自动依赖所有组件"
  特殊处理在 6.1 仍在。
- 未命中的高危 token（无需处理）：`driver/adc.h`、`adc1_get_raw`、`esp_adc_cal_*`、
  `driver/i2s.h`（旧）、`driver/dac.h`、`ledc_*`、`mcpwm_*`、`pcnt_*`、`rmt_*`、
  `timer_group_*`、`uart_*`、`spi_flash_*`、`esp_spiram.h`、`MALLOC_CAP_EXEC`、
  `EXT_RAM_ATTR`、`xTaskGetAffinity`、`vTaskDelayUntil`、`xQueueGenericReceive`、
  `esp_log_buffer_hex`、`esp_deep_sleep_enable_gpio_wakeup`、FATFS 卸载函数、
  `ESP_IDF_VERSION*`（工程内**没有任何 IDF 版本条件编译**）。
- `esp_private/esp_clk.h` 的 `esp_clk_cpu_freq()`（`performance_monitor.c`）在 6.1
  仍存在 —— 属私有 API、不保证未来，但当前可用。

### Kconfig 层

- `main/Kconfig.projbuild` 的 11 个 config 全部合规，**一条 kconfig v3 废弃写法都没踩**
  （实测用 v3.14.0 的 kconfiglib 以 parser 1 与 parser 2 分别解析：0 warning、
  0 条 `Deprecation notice`）。全文件不含 `tristate` / `def_*` / `---help---` /
  `optional` / `option` / `choice` / `select` / `imply` / 宏赋值 / 环境变量引用 /
  无类型 config / 小写或带点的配置名。
- 项目**没有**也需要 `sdkconfig.rename`（所有 config 名都是合法大写标识符）。
- 项目实际写入的 IDF 侧 Kconfig 项（SPIRAM / FreeRTOS / FATFS / PM / BOOTLOADER /
  ESPTOOLPY / PARTITION / COMPILER / LVGL 等）在 6.1 中**除 §6.1 那一条外全部仍然
  有效**，无一条被移除。

### 行为变更（已评估，对本项目影响可控）

| 变更 | 评估 |
|---|---|
| **默认 LibC 由 Newlib 改 PicolibC** | 本项目无"按任务重定义 stdin/stdout/stderr"的用法；FreeType 的 `ftsystem.c`（stdio 路径）已被链接 hack 排除。**真机实测 FATFS/VFS 读写正常**（§8.2 扫描 8990 首 + 写播放列表分区成功） |
| **FreeRTOS 函数默认从 IRAM 移到 flash**（5.5 里 `CONFIG_FREERTOS_IN_IRAM` 是隐藏且 default y，6.1 变可见且 default n） | IRAM 更宽裕、性能略降；新增 `CONFIG_FREERTOS_IN_IRAM=y` 可复刻 5.5 行为。真机实测 `iram` 空闲 48~144KB（比 5.5 时代的 38.8KB 宽裕很多），**当前未开**。⚠ 注意本项目有大量"关缓存"路径（`esp_partition_mmap/munmap`、擦写 pldata/font），仍需留意 |
| **ringbuf 函数默认从 IRAM 移到 flash**（`CONFIG_RINGBUF_IN_IRAM` 新增，default n） | 影响 `mp3_player.c` 的 Core1 解码↔I2S 环形缓冲；真机实测 `mp3_play`/`mp3_out` 任务正常创建运行。如音频路径日后出现异常，可显式开 `CONFIG_RINGBUF_IN_IRAM=y` |
| `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE` 默认 y→n | 三套配置均开断言、未定义 `NDEBUG`，**无实际影响** |
| `CONFIG_VFS_SUPPORT_TERMIOS` 默认关闭 | 本项目未用 termios |
| `CONFIG_FATFS_USE_DYN_BUFFERS` 默认开启、LFN 默认堆缓冲 | 与项目原设置一致（`CONFIG_FATFS_LFN_HEAP=y` 变冗余但无害） |
| 全局构造函数执行顺序由降序改升序 | 本项目无依赖构造函数顺序的静态注册机制 |
| `esp_lcd` 的 `color_space`/`rgb_endian` → `rgb_ele_order`、`esp_lcd_panel_disp_off` → `esp_lcd_panel_disp_on_off` | 项目代码**早已**用新写法（`display.c`） |
| `driver` 组件不再传递 18 个 `esp_driver_*` | `main` 不受影响；本地组件 `esp_lcd_qspi_amoled` 的 `PRIV_REQUIRES "driver"` 靠 `esp_lcd` 仍公共依赖 gpio/i2c/spi 而可用（**脆弱点，见 §9.2**） |

---

## 8. 实测验证记录

环境：ESP-IDF **v6.1**（`C:\esp\v6.1\esp-idf`），GCC **15.2.0**（`esp-15.2.0_20251204`），
CMake 4.0.3，Ninja 1.12.1，Python 3.13.12，esptool 5.6.0，ccache 启用。
设备：FakePodNano（ESP32-S3 rev v0.2，8MB Octal PSRAM，16MB Flash），COM12。

### 8.1 构建（三套配置全通过）

| 配置 | 命令 | 结果 | 固件大小 | app 分区占用 |
|---|---|---|---|---|
| debug（`cs43131.ttffont.debug`） | `idfc build`（从零全量） | ✅ 147.9 s | `0x1c7a80` = **1.86 MiB** | 22%（余 78%） |
| releasecandidate | `idfc build -t releasecandidate` | ✅ 193.4 s | `0x216600` = **2.09 MiB** | 26%（余 74%） |
| release | `idfc build -t release` | ✅ 191.9 s | `0x2150a0` = **2.08 MiB** | 26%（余 74%） |

启动引导程序三套一致：`0x3cb0` 字节，余量 53%。
分区表与 5.5 时代**完全一致**（`factory` 8128K / `font` 4M @0x800000 / `pldata` 4M
@0xc00000），**没有因迁移而改动**，因此 **font 分区不需要重刷**（字体数据偏移没变）。

### 8.2 真机功能验证（debug 配置，含 exFAT 补丁）

一次从头到尾的完整流程，全程无 `abort`/`panic`/重启：

```
I (34)  octal_psram: vendor id : 0x0d (AP) … Found 8MB PSRAM device
I (250) app_init: ESP-IDF:          v6.1                          ← 确认跑的是 6.1
I (276) heap_init: At 3FCAD3F0 len 0003C320 (240 KiB): RAM
I (301) esp_psram: Adding pool of 6400K of PSRAM memory
I (776) touch: Scanning I2C bus...
I (783) touch: Found I2C device at address 0x15 / 0x30 / 0x6b / 0x7e
I (875) cs43131: CS43131 found at I2C addr 0x30, ID = 43 13 10    ← 与触摸共用总线 OK
I (915) cs43131: CS43131 init OK (48kHz, direct XTAL 24.576MHz)
I (1109) sd_card: SD card mounted at /sdcard, listing root:        ← exFAT 生效
I (5225) playlist_builder: scan done: dirs=172 songs=8990 str_len=356478 total=618622
I (11382) playlist_store: playlist saved: len=618622 crc=0x9b0ea409
I (11732) pldata: pldata mapped to 0x3cc00000
I (12104) playlist_page: playlist page show dir=0 (116 songs)
I (12247) boot_flow: enter normal UI                              ← 进正常主界面
I (22560) MEM: stk: deep_worker=11592 button_svc=3376 power_svc=2120 LVGL=8860
                   player_ctrl=1492 sleep_timer=3488 mem_monitor=2836
                   cpu_coll0=416 cpu_coll1=460 mp3_play=5996 mp3_out=3276 cover_load=2412
I (25958) eq: scan /sdcard/.musicfx: 3 eq file(s)
```

覆盖到的路径：PSRAM 初始化 → NVS 配置读取 → AMOLED(QSPI) → LVGL 移植层 →
I2C 总线 + 触摸控制器（CST820）→ 电池 ADC → I2S + CS43131 → pldata 分区 →
按键服务 → **SD 卡 exFAT 挂载** → 172 目录 / **8990 首歌**递归扫描 →
写 618KB 播放列表分区 → mmap 读回 → 进入主界面 → 音频/封面/EQ 任务运行。

内存与栈水位（开机完成后稳态）：

| 指标 | 值 | 与 5.5 时代对比 |
|---|---|---|
| PSRAM 堆 | ~5.7 MB 空闲 | 正常 |
| 内部 RAM 空闲 | 开机后约 54.6 KB（稳态），`int free now` 读数 53~73 KB | 5.5 时代 §IRAM 审计的稳态是 38.8 KB，**更宽裕** |
| `iram`（性能监视读数） | 48.7~144 KB | — |
| LVGL 任务栈 HWM | 8,860 B（预算 20 KB） | 充足 |
| deep_worker 栈 HWM | 11,592 B（预算 12 KB） | 偏紧但符合既有记录（TTF 迁移时同量级） |
| mp3_play / mp3_out 栈 HWM | 5,996 / 3,276 B | 充足 |

### 8.3 剩余的编译警告（无害，记录备查）

| 警告 | 来源 | 处置 |
|---|---|---|
| `'read_id' defined but not used` | 本地 `esp_lcd_touch_chsc6417.c` | 既有代码遗留，与迁移无关 |
| `variable 'gesture_id' set but not used` | 本地 `esp_lcd_touch_cst816/820.c` | 同上 |
| `Private include directory … belongs to esp_wifi but is being used by wpa_supplicant` × 5 | **IDF 6.1 自身的组件校验告警** | 与本项目无关，无法也不应处理 |
| `FATFS_PRINT_LLI: 'default 0' is not a valid bool value` 等 NOTE | **IDF 6.1 自身 Kconfig** | 同上 |
| kconfgen 把中文注释行报告为 "line was updated to # … 閰嶇疆 …" | **控制台编码问题**（见 §9.3） | 只影响日志显示，配置值正确 |

（LVGL 的 `iram1` 段号冲突警告已由 §4.1 的补丁**彻底消除**，不是抑制。）

---

## 9. 遗留事项与后续要盯的点

### 9.1 真机回归结果（2026-11，已全部通过）

迁移后在真机上逐项验证，**均正常**：

| 验证项 | 结果 |
|---|---|
| 播放 / 解码 | ✅ 正常 |
| 音量 | ✅ 正常 |
| 伪关机 | ✅ 正常 |
| 唤醒（B 键） | ✅ 正常 |
| 睡眠定时 | ✅ 正常 |
| 息屏 / 亮屏 | ✅ 正常 |
| CPU 频率（esp_pm 240/80MHz 切换） | ✅ 正常 |
| SD 扫描 | ✅ 正常（8990 首） |
| 无卡分支 | ✅ 正常 |
| **低电压拒启** | ✅ **正常**（见下） |

**低电压拒启的验证方法**（值得复用）：把 `core/hardware/battery.h` 的
`BATTERY_CRITICAL_LOW_MV` **临时抬高**到高于当前电池电压的值（本次用满电电压 4200mV），
烧录后开机即走拒启分支：

```
I (822)  battery: Initialize ADC for battery voltage (GPIO1)
I (837)  MEMTRACE: 07-battery  ...
W (1137) boot_flow: battery too low (4190 mV < 4200 mV), refuse to boot
```

得到该日志、并停在低电警告屏即验证通过；**测完必须把阈值改回 3450**
（否则设备会拒绝正常开机）。该阈值为开机拒启与运行中低电自动伪关机共用，
所以这一个测试同时覆盖了后者读数的正确性。

其余仍建议日常留意但本次未专门复测的：长时间运行的内存/栈是否有缓慢泄漏、
列表滚动帧率与 5.5 时代的对比（LVGL 的 IRAM 布局未变，理论上应一致）。

### 9.2 脆弱点（已在迁移收尾时全部处理）

- ✅ **`esp_driver_gpio` 依赖已补成显式**（2026-11 收尾）。IDF 6.0 起旧 `driver`
  组件不再公共依赖 18 个 `esp_driver_*`，而下面这些组件都**直接调用 GPIO**
  （`gpio_config` / `gpio_reset_pin` / `gpio_set_level` / `gpio_isr_handler_*`），
  此前只是靠 `esp_lcd` / `esp_lcd_touch` 转发依赖才编过：
  - 仓库内的 4 个组件**直接改在各自 `CMakeLists.txt`**（它们在版本控制里，无需补丁）：
    `esp_lcd_qspi_amoled`（`PRIV_REQUIRES driver esp_driver_gpio`）、
    `esp_lcd_touch_cst816` / `_cst820` / `_chsc6417`（`PRIV_REQUIRES esp_driver_gpio`）。
  - 托管组件 `espressif/esp_lcd_touch` 自己也是 `REQUIRES "driver" "esp_lcd"` 却
    大量调 GPIO —— 它会被组件管理器覆盖，故走**构建期补丁**
    `patches/esp_lcd_touch_deps/`（见 §4.1 的补丁机制说明）。
  - 另外注意：`esp_lcd_sh8601`（托管组件，同样 `PRIV_REQUIRES driver`）本项目
    **并未实际使用**（用的是自研 `esp_lcd_qspi_amoled`），只是早先经由
    `components/esp_lcd_qspi_amoled/idf_component.yml` 的依赖链被拉进构建；
    本项目**没有直接依赖它**，故未改动，仅此备注。
- **`patches/` 的补丁机制依赖 `main/CMakeLists.txt` 里那段回写逻辑**：已实测有效
  （把目标文件还原成上游原样后，`build --reconfigure` 会自动回写且构建产物一致）。
  若日后有人清理"看起来没用"的 CMake 代码，补丁会失效——症状就是 §4 的
  `iram1` 段号冲突错误、或 `esp_lcd_touch` 的 `driver/gpio.h` 找不到，回归。
- **§6.7 的 IDF 本体 exFAT 补丁不在本工程 git 里**：换机器/重装 IDF 必须重打。

### 9.3 控制台编码（建议，未做）

配置阶段会打印若干
`NOTE: … line was updated to # ==================== LVGL 9 閰嶇疆 ====================n`
—— 这是 **kconfgen 写临时文件/报日志时的编码问题**（中文注释被按 GBK 解读）。
**只影响日志显示**：`sdkconfig.defaults` 源文件里的中文注释完好，生成的 sdkconfig
配置值也正确（已核对）。若要消掉，可让服务器带 `PYTHONUTF8=1` 启动（需重启 idfc 服务器）。
本次未做。

### 9.4 本次明确**没有做**的事

- **没有加入任何 ESP32-S31 相关内容**（用户要求：先把现有 S3 工程在新框架下跑通）。
  后续做 S31 时值得先看的点：
  - 本机 v6.1 已有 `components/soc/esp32s31`（芯片支持已进 IDF）；
  - 本工程的芯片相关假设集中在少数几处：`sdkconfig.defaults` 的
    `CONFIG_IDF_TARGET="esp32s3"` / `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`、
    `partitions.csv`、以及各外设的 GPIO 分配（见 AGENTS.md 的 GPIO 表）——
    新芯片的 GPIO/外设差异需要实机核对；
  - `idfc_buildconfig.json` 现在只有 `dac_chip` / `font` 两个 overlay 维度，
    若要按芯片出不同固件，最自然的做法是**再加一个 `soc` 维度**
    （`OutputDir` 要补上占位符，所有 cell 目录名会再变一次）。
- 没有升级任何托管组件版本（LVGL 仍 9.4.0、esp_lcd_touch 仍 1.2.1 等）；
  `dependencies.lock` 只是被 component manager 改了锁格式（2.0.0 → 3.0.0）
  并记下 `idf 6.1.0`。
- 没有改 `partitions.csv`。
- 没有开 `CONFIG_FREERTOS_IN_IRAM` / `CONFIG_RINGBUF_IN_IRAM`（先按 6.1 默认跑，
  实测 IRAM 余量宽裕）。

---

## 10. 改动清单与回滚

本次改动的**全部文件**（可 `git diff` 对照）：

```
CMakeLists.txt                              cmake_minimum_required 3.5 → 3.16
main/CMakeLists.txt                         LVGL 构建期补丁应用逻辑（替代先前的警告抑制）
patches/lvgl_fast_mem_decl/src/misc/*.h     新增：LVGL PR #10737 的 3 个修好的文件
main/main.c                                 i2c 头文件 + 唤醒原因 API
main/core/hardware/touch.c / touch.h        I2C 总线迁到 i2c_master + scl_speed_hz 修复
                                            + 公开总线句柄 + 释放改 i2c_del_master_bus
main/core/hardware/audio/cs43131.c / .h     设备句柄挂总线 + 读写/探测改写 + bus_detach
main/core/utils/fake_shutdown.c             关机序列插 cs43131_bus_detach()
main/core/ui/lvgl_port.c                    esp_lcd_touch_get_data()
sdkconfig.defaults                          PSRAM 栈改名 + 孤立段 PLACE→WARNING
.gitignore                                  忽略 .idfc/
components/i2c_bsp/                         删除（git rm，死代码）
.idfc/idfc_config.json                      激活脚本指向 v6.1（本地状态，不入库）
C:\esp\v6.1\esp-idf\components\fatfs\src\ffconf.h   ⚠ 不在本仓库：FF_FS_EXFAT 0 → 1
```

要回到 5.5.5：`git revert` 上述提交（`components/i2c_bsp` 会被恢复），把
`idfc set-idfenv` 指回 `Microsoft.v5.5.5.PowerShell_profile.ps1`，重启服务器，
然后 `idfc build --fullclean`（**必须全量重配**，理由见下）。

⚠ **跨 IDF 版本切换必须清构建目录**：Kconfig v3 改了"默认值推断"机制——v3 会给
*推断出来的*默认值在 sdkconfig 里加 `# default:` 标记，只有带标记的值才随依赖变化重算；
把 5.5 生成的旧 `build/<cell>/sdkconfig` 直接喂给 6.1，里面的值仍被当作"用户显式设定"，
于是**新 default 会静默不生效也不报错**（与 AGENTS.md 第 6 节记的历史事故同源）。
本次迁移的第一步就是 `idfc build --fullclean`，所以不存在这个问题。
