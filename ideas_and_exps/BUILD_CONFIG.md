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
| `HEAP_POISONING` | 堆毒化（越界写在释放时立即报 CORRUPT HEAP + 调用栈） | 关（`DISABLED`） | 关（`DISABLED`） | 关 |
| `MEM_TRACE` | 内存快照诊断日志（各阶段内部 RAM/DMA/PSRAM 占用） | 开 | 开 | **关** |

> ⚠ 触摸区域测试（白色圆点）与电池电压采样日志**已改为运行时开关**，不再是构建差异：
> 设置菜单 → 关于 → 开发者选项（config_store/NVS，**默认关**，开机检查、修改需重启生效），
> 详见 `main/core/storage/config_store`、`main/core/ui/touch_test`、`main/core/utils/battery_logger`。

## 3. 构建命令

所有命令通过项目自带的 `idfc.exe`（客户端-服务器架构）执行，在项目根目录运行。

### 3.1 构建：参数由 idfc 自动拼装

**不需要**再手工带 `-B` / `SDKCONFIG_DEFAULTS` / `SDKCONFIG`——`idfc build` 会按构建类型自动拼好
（`-B build/<variant>`、`-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<variant>"`、
`-D SDKCONFIG="build/<variant>/sdkconfig"`）。首建与增量构建是同一条命令：

```powershell
.\idfc.exe build                      # 当前构建类型（.idfc 里保存的，默认 debug）
.\idfc.exe build -t releasecandidate  # 指定类型
.\idfc.exe build -t release
```

> ⚠ 旧写法（`.\idfc.exe -B build/debug -D SDKCONFIG_DEFAULTS="…" build`）**已全部失效**，
> 现在报 `没有这样的命令`（退出码 1）。历史文档/旧笔记里的这类命令不要照抄，完整说明见 `IDFC_USAGE.md`。

### 3.2 增量构建

```powershell
.\idfc.exe build -t debug                     # debug 增量构建（默认类型，可省 -t）
.\idfc.exe build -t releasecandidate          # RC 增量构建
.\idfc.exe build -t release                   # release 增量构建
```

### 3.3 重新配置 / 强制全量 / 只看会做什么

```powershell
.\idfc.exe build --reconfigure        # 强制 clean + reconfigure 后再构建（改了 defaults 之后）
.\idfc.exe build --no-reconfigure     # 跳过过期检测，纯增量
.\idfc.exe build --fullclean          # 删掉整个 build/<variant> 再从零构建
.\idfc.exe build --dry-run            # 不构建，只报告"是否要清理"与将要执行的完整命令
.\idfc.exe clean -t debug             # 只删 build/debug/sdkconfig 与 build/debug/config
```

这些命令实际执行的完整 `idf.py` 命令行，会在开头以 ``# Executing `idfc idfpy …` `` 回显，可直接照抄复用。

### 3.4 关于"改了 defaults 却不生效"（历史事故）

已有 sdkconfig 的优先级**高于** defaults（kconfgen 用 `replace=False` 合并，已有值不会被 defaults 覆盖），
所以"改了 `sdkconfig.defaults.*` → 只跑普通增量构建"时新配置**不会生效、也不会报错**。

- **现在不用手工删文件**：`build` 会自动比较 `build/<variant>/sdkconfig` 与 `sdkconfig.defaults*`、
  所有 `Kconfig`/`*.projbuild` 的时间戳，过期即自动 clean + reconfigure（并打印原因）。
- 想强制重配用 `build --reconfigure`，想从最干净状态开始用 `build --fullclean`。
- 需要"只 reconfigure、不编译"时没有专门子命令，走逃生舱——**路径参数必须自己写全**：
  ```powershell
  .\idfc.exe idfpy -B build/debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig" reconfigure
  ```
- 裸 `idfpy` 绕过 `build` 时，`-B`/`-D` 的正确性由你自己负责（历史上"堆毒化仍开"的事故就是这么来的）。

## 4. 烧录命令

烧录前先确认设备串口号（如 `COM12`）。构建类型与串口都由 idfc 拼装，`-p` 只在你没 `set-port` 时才需要：

```powershell
.\idfc.exe flash                      # 烧录 .idfc 里保存的类型 + 串口（默认 debug / COM12）
.\idfc.exe flash -t releasecandidate -p COM12
.\idfc.exe flash -t release -p COM12
```

⚠ **`flash` 不重建**：若 `build/<variant>/sdkconfig` 早于 defaults 或任何 `Kconfig`/`*.projbuild`，
它会打印"当前构建类型 … 已经过时。当前即将刷入的是旧版的固件"，**然后照样把旧产物烧进去**。
要烧新配置就得先 `build`，或直接用 `run`（构建 → 体积 → 烧录 → 监视一条龙）：

```powershell
.\idfc.exe run --monitor-wait-for "App started"
```

⚠ 不确定端口时先列串口（`ports` 会过滤掉蓝牙串口，并给出设备名与实例路径，便于认板子）：

```powershell
.\idfc.exe ports
.\idfc.exe set-port COM12      # 记住本项目的默认串口，之后 flash/monitor/run 都不用带 -p
```

## 5. 串口监视

```powershell
.\idfc.exe monitor                              # .idfc 里保存的类型与串口（默认 debug / COM12）
.\idfc.exe monitor -t releasecandidate -p COM12
.\idfc.exe monitor --wait-for "App started"     # 命中即退出（退出码 0），适合自动化
.\idfc.exe monitor --timeout 15000              # 到点退出（退出码 1）
.\idfc.exe monitor --grep "MEM:"                # 只显示匹配行
```

退出：`Ctrl+]`（`--wait-for` / `--timeout` 会自行退出）。观察要点：

- **debug / releasecandidate 版**应看到每 2s 一条 `MEM: heap=... cpu0=..% cpu1=..%`（performance_monitor），屏幕角落有 LVGL FPS 计数；其中 **releasecandidate 版**的 `MEM: heap=` 数值会比 debug 版低约 2.9MB（完整字体占用的 PSRAM），正好用于观察低内存环境下的运行状况
- **release 版**无上述日志（性能监视等编译级调试功能关闭）
- 触摸测试白点 / 电池电压日志为运行时开关，**默认关闭**：开启需在设置菜单 → 关于 → 开发者选项中打开并重启（开启后可看到 `touch_test: Touch test created` 或 `batt_log: battery logger ready` 日志）

## 6. 工作原理（为什么可以多配置共存）

`build`/`flash`/`monitor`/`run`/`size` 会自动把下面三项拼进命令行（想核对就用 `build --dry-run`，或看执行时回显的 ``# Executing `idfc idfpy …` ``）：

| 机制 | 作用 |
|---|---|
| `-B <dir>` | 指定独立构建目录，隔离编译产物（`build/debug`、`build/releasecandidate`、`build/release`） |
| `SDKCONFIG_DEFAULTS` | 指定默认配置**文件列表**（分号分隔，后者覆盖前者） |
| `SDKCONFIG` | 指定 sdkconfig 的**生成路径**，避免多配置共用根目录 sdkconfig 互相覆盖 |

设置 `SDKCONFIG_DEFAULTS` 后会**整体替换**默认行为（`sdkconfig.defaults` 不再自动加载），所以必须把公共文件显式写进列表，如 `"sdkconfig.defaults;sdkconfig.defaults.debug"`——这也是**手工走 `idfpy` 时最容易写错、后果最严重的一处**。

## 7. 常见问题

- **改了 `sdkconfig.defaults.*` 后不生效**：已有 sdkconfig 优先级高于 defaults，仅 reconfigure 不会覆盖旧值。**现在只需 `.\idfc.exe build`**——它会自动检测过期并 clean + reconfigure（见 §3.4）；要手工处理才需要 `build --reconfigure` 或删掉 `build/<v>/sdkconfig`。
- **换了新电脑/删了 build 目录**：直接 `.\idfc.exe build -t <类型>` 即可，参数会重新生成（见 §3.1）。
- **想调整某配置项**：编辑对应的 `sdkconfig.defaults.debug` / `sdkconfig.defaults.releasecandidate` / `sdkconfig.defaults.release`（只写差异项），或公共项编辑 `sdkconfig.defaults`。
- **`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 与 performance_monitor 的关系**：release 关闭 performance_monitor 时也一并关闭了 runtime stats（`ulTaskGetIdleRunTimeCounter` 仅 performance_monitor 使用），二者必须同开同关。

## 8. 相关文件

- 配置：`sdkconfig.defaults` / `sdkconfig.defaults.debug` / `sdkconfig.defaults.releasecandidate` / `sdkconfig.defaults.release`
- 应用配置项定义：`main/Kconfig.projbuild`（`PERFORMANCE_MONITOR`、`UI_SYSTEM_FONT_FULL`、`UI_COVER_FLASH` 等；触摸测试/电池日志已移除 Kconfig，改运行时开关，见上方说明）
- 构建产物：`build/debug/`、`build/releasecandidate/`、`build/release/`
- 固件：`build/debug/FakePodNanoNightPlayer.bin`、`build/releasecandidate/FakePodNanoNightPlayer.bin`、`build/release/FakePodNanoNightPlayer.bin`
