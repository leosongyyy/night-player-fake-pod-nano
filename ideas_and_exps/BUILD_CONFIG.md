# BUILD_CONFIG —— 多配置构建与烧录指南

本文档说明 FakePodNanoNightPlayer 的 **debug / releasecandidate / release 三套构建配置**体系：配置文件、构建/烧录/监视命令、配置差异与工作原理。

## 1. 概述

项目支持三套独立构建配置，互不干扰，各自有独立的 `sdkconfig` 和构建产物目录：

| 配置 | 定位 | 固件大小 | 烧录耗时 |
|---|---|---|---|
| **debug** | 日常开发：烧录快、调试功能全开 | 约 1.1 MB | ~11 s |
| **releasecandidate** | 发布前验证：release 内存占用（完整字体+极速版封面）+ 调试功能全开 | 约 4.2 MB | ~37 s |
| **release** | 正式使用：功能完整、调试功能全关 | 约 4.2 MB | ~37 s |

> **releasecandidate（RC）的用途**：完整字体经 SPIRAM_RODATA 拷贝进 PSRAM 约 2.6MB，使可用堆比 debug 版少约 2.9MB（PSRAM 空闲池从约 7104K 降到约 4160K）。RC 用与 release 相同的内存占用、同时保留全部调试手段，用来在"可用内存变小"的真实条件下定位内存相关问题。

## 2. 配置文件

```
sdkconfig.defaults            公共配置（esp32s3、QIO 16MB Flash、PSRAM、分区、
                              音频/屏幕/LVGL 等所有平台相关与功能配置）
sdkconfig.defaults.debug      debug 差异配置（叠加在公共配置之后）
sdkconfig.defaults.releasecandidate  RC 差异配置（叠加在公共配置之后）
sdkconfig.defaults.release    release 差异配置（叠加在公共配置之后）
```

合并规则：`sdkconfig.defaults` + 差异文件按顺序合并生成各配置的最终 `sdkconfig`，**后加载的文件覆盖先加载的**。最终 sdkconfig 生成在各自构建目录（`build/debug/sdkconfig`、`build/releasecandidate/sdkconfig`、`build/release/sdkconfig`），根目录 `sdkconfig` 已移出版本控制、不参与多配置构建。

### 三套配置的差异项

| 配置项 | 含义 | debug | releasecandidate | release |
|---|---|---|---|---|
| `COMPILER_OPTIMIZATION` | 编译优化级别 | Performance（-O2） | Performance（-O2） | Performance（-O2，运行性能优先） |
| `UI_SYSTEM_FONT_FULL` | 系统默认字体 | 精简版（固件小、烧录快） | **完整版** | 完整版（全量中文/日文/阿拉伯文/藏文） |
| `UI_COVER_FLASH` | 默认封面 | 2bit 索引色版（省 Flash） | **RGB565 极速版** | RGB565 极速版（渲染快） |
| `PERFORMANCE_MONITOR` | 双核 CPU/内存监视（串口每 2s） | 开 | 开 | 关 |
| `FREERTOS_GENERATE_RUN_TIME_STATS` | FreeRTOS 运行时统计（performance_monitor 依赖） | 开 | 开 | 关 |
| `LV_USE_PERF_MONITOR` | LVGL FPS 计数（屏幕角落） | 开 | 开 | 关 |

> ⚠ 触摸区域测试（白色圆点）与电池电压采样日志**已改为运行时开关**，不再是构建差异：
> 设置菜单 → 关于 → 开发者选项（config_store/NVS，**默认关**，开机检查、修改需重启生效），
> 详见 `main/core/storage/config_store`、`main/core/ui/touch_test`、`main/core/utils/battery_logger`。

## 3. 构建命令

所有命令通过项目自带的 `idfc.exe`（客户端-服务器架构）执行，在项目根目录运行。

### 3.1 从零开始（首次配置 + 构建）

首次必须同时带 `-B`（构建目录）、`SDKCONFIG_DEFAULTS`（默认配置列表）、`SDKCONFIG`（sdkconfig 生成位置）：

```powershell
# debug
.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" build

# releasecandidate（RC）
.\idfc.exe -B build/releasecandidate -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.releasecandidate" -D SDKCONFIG="build/releasecandidate/sdkconfig" build

# release
.\idfc.exe -B build/release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" -D SDKCONFIG="build/release/sdkconfig" build
```

### 3.2 后续增量构建（配置已缓存）

首次配置后，`SDKCONFIG_DEFAULTS`/`SDKCONFIG` 已写入构建目录的 CMake 缓存，后续只需带 `-B`：

```powershell
.\idfc.exe -B build/debug build              # debug 增量构建
.\idfc.exe -B build/releasecandidate build   # RC 增量构建
.\idfc.exe -B build/release build            # release 增量构建
```

### 3.3 仅重新配置（不编译，快速验证配置）

```powershell
.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" reconfigure
.\idfc.exe -B build/releasecandidate -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.releasecandidate" -D SDKCONFIG="build/releasecandidate/sdkconfig" reconfigure
.\idfc.exe -B build/release -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.release" -D SDKCONFIG="build/release/sdkconfig" reconfigure
```

生成后可检查 `build/<name>/sdkconfig` 中的关键配置项确认差异生效。

⚠ **修改 defaults 文件后的重要规则**：已有 sdkconfig 的优先级**高于** defaults（kconfgen 用 `replace=False` 合并，已有值不会被 defaults 覆盖）。所以修改了 `sdkconfig.defaults.*` 后，仅 reconfigure 改动**不会生效**——必须先删除对应构建目录的 sdkconfig 再重新配置：

```powershell
Remove-Item build/debug/sdkconfig            # 删除目标构建目录的 sdkconfig
.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" reconfigure
```

### 3.4 默认构建（不带任何参数）

不带 `-B`/`-D` 的 `.\idfc.exe build` 走根目录 `sdkconfig`（旧的单配置流程），**不推荐**继续使用，统一走 debug/releasecandidate/release 三配置。

## 4. 烧录命令

烧录前先确认设备串口号（如 `COM12`），并带上与构建一致的 `-B` 目录，**必须对应烧录的固件**：

```powershell
# 烧录 debug 版到 COM12
.\idfc.exe -B build/debug -p COM12 flash

# 烧录 releasecandidate 版到 COM12
.\idfc.exe -B build/releasecandidate -p COM12 flash

# 烧录 release 版到 COM12
.\idfc.exe -B build/release -p COM12 flash
```

⚠ 若不确定端口，先列出串口确认：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

## 5. 串口监视

```powershell
# 监视 debug 固件运行日志
.\idfc.exe -B build/debug -p COM12 monitor

# 监视 releasecandidate 固件运行日志
.\idfc.exe -B build/releasecandidate -p COM12 monitor

# 监视 release 固件运行日志
.\idfc.exe -B build/release -p COM12 monitor
```

退出：`Ctrl+]`。观察要点：

- **debug / releasecandidate 版**应看到每 2s 一条 `MEM: heap=... cpu0=..% cpu1=..%`（performance_monitor），屏幕角落有 LVGL FPS 计数；其中 **releasecandidate 版**的 `MEM: heap=` 数值会比 debug 版低约 2.9MB（完整字体占用的 PSRAM），正好用于观察低内存环境下的运行状况
- **release 版**无上述日志（性能监视等编译级调试功能关闭）
- 触摸测试白点 / 电池电压日志为运行时开关，**默认关闭**：开启需在设置菜单 → 关于 → 开发者选项中打开并重启（开启后可看到 `touch_test: Touch test created` 或 `batt_log: battery logger ready` 日志）

## 6. 工作原理（为什么可以多配置共存）

| 机制 | 作用 |
|---|---|
| `-B <dir>` | 指定独立构建目录，隔离编译产物（`build/debug`、`build/releasecandidate`、`build/release`） |
| `SDKCONFIG_DEFAULTS` | 指定默认配置**文件列表**（分号分隔，后者覆盖前者） |
| `SDKCONFIG` | 指定 sdkconfig 的**生成路径**，避免多配置共用根目录 sdkconfig 互相覆盖 |

设置 `SDKCONFIG_DEFAULTS` 后会**整体替换**默认行为（`sdkconfig.defaults` 不再自动加载），所以必须把公共文件显式写进列表，如 `"sdkconfig.defaults;sdkconfig.defaults.debug"`。

## 7. 常见问题

- **改了 `sdkconfig.defaults.*` 后不生效**：已有 sdkconfig 优先级高于 defaults，仅 reconfigure 不会覆盖旧值。**必须先删除对应构建目录的 `sdkconfig` 再重新配置**（见第 3.3 节末尾的示例）。只跑增量 build 更不会重新加载 defaults。
- **换了新电脑/删了 build 目录**：回到第 3.1 节的完整命令重新配置即可。
- **想调整某配置项**：编辑对应的 `sdkconfig.defaults.debug` / `sdkconfig.defaults.releasecandidate` / `sdkconfig.defaults.release`（只写差异项），或公共项编辑 `sdkconfig.defaults`。
- **`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 与 performance_monitor 的关系**：release 关闭 performance_monitor 时也一并关闭了 runtime stats（`ulTaskGetIdleRunTimeCounter` 仅 performance_monitor 使用），二者必须同开同关。

## 8. 相关文件

- 配置：`sdkconfig.defaults` / `sdkconfig.defaults.debug` / `sdkconfig.defaults.releasecandidate` / `sdkconfig.defaults.release`
- 应用配置项定义：`main/Kconfig.projbuild`（`PERFORMANCE_MONITOR`、`UI_SYSTEM_FONT_FULL`、`UI_COVER_FLASH` 等；触摸测试/电池日志已移除 Kconfig，改运行时开关，见上方说明）
- 构建产物：`build/debug/`、`build/releasecandidate/`、`build/release/`
- 固件：`build/debug/FakePodNanoNightPlayer.bin`、`build/releasecandidate/FakePodNanoNightPlayer.bin`、`build/release/FakePodNanoNightPlayer.bin`
