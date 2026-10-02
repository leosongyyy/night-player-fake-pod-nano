# idfc 使用说明（ESP-IDF 命令行客户端 / 服务器）

`idfc.exe` 是 ESP-IDF 的命令行客户端/服务器工具，用于便捷地执行 `idf.py` 构建、烧录、监视等命令。它采用 **服务器 + 客户端** 架构：服务器一次性激活 ESP-IDF 环境，然后通过 HTTP + SSE（Server-Sent Events）把 `idf.py` 命令的输出流式回传给客户端。

> 📥 **下载地址：<https://gitee.com/ZYFDroid/idf-client-cli/releases/>**。下载后把 `idfc.exe` 放到 **PATH 中的某个目录**（例如 `C:\Users\<你>\ProgramFiles\bin\`），然后在**项目目录**里直接用 `idfc <子命令>` 调用即可（见 §3）。

> 🆕 **2026-10-02 起：本项目启用了「配置矩阵」**（`idfc_buildconfig.json`，维度 = `variant` × `dac_chip`）。**构建目录从 `build/<variant>` 变成 `build/<cell>`（如 `build/cs43131.debug`）**，并新增 `-o <维度>=<类型>` 参数与 `matrix` / `set-overlay` 命令。**本文档中凡出现 `-B build/<variant>`、`build/<cfg>` 的示例都属于矩阵之前的行为**（没有 `idfc_buildconfig.json` 的项目仍然如此，因此这些说明并未失效，只是不再是本项目的路径）。矩阵的定义、格式与实测记录见 `BUILD_MATRIX.md`；新版 `idfc help --all` 与 idfc 仓库 README 是权威说明。

> ⚠ **本文档对应当前的「子命令版」idfc。**
> 旧版语法（`--server` / `-B build/<cfg> build` / `-B ... reconfigure` / `raw-idf`）**已全部失效**，现在一律报 `没有这样的命令`（退出码 1）。历史对话、旧文档、旧笔记里的 idfc 命令**不要照抄**——先看 §1 的迁移对照。
> 全部命令与参数请用 `idfc help --all` 查询（它是唯一的事实源）。

---

## 1. 从旧版迁移（必读）

| 旧写法（已失效） | 新写法 |
|---|---|
| `idfc --server --idfenv <ps1>` | `idfc start-server --idfenv <ps1>`（或 `set-idfenv` 后直接 `idfc start-server`） |
| 关控制台窗口 / taskkill | `idfc stop-server` |
| `idfc -B build/debug build` | `idfc build`（或 `idfc build -t debug`） |
| `idfc -B build/debug -D SDKCONFIG_DEFAULTS="…" -D SDKCONFIG="…" reconfigure` | `idfc build --reconfigure` |
| `idfc -B build/debug -p COM5 flash` | `idfc flash -p COM5`（或 `set-port COM5` 后 `idfc flash`） |
| `idfc -B build/debug -p COM5 monitor` | `idfc monitor -p COM5` |
| `idfc raw-idf <参数>` | `idfc idfpy <参数>` |
| `idfc menuconfig` | `idfc idfpy menuconfig` |

**迁移的实质**：旧版是「什么都透传给 idf.py」，多配置的 `-B` 与 `-D` 全靠使用者每次敲对；新版把「本项目是三套配置构建」这件事**内建进了工具**，`build`/`flash`/`monitor`/`run`/`size` 会自动拼好参数并做过期检测；需要原样透传时走 `idfpy`。

---

## 2. 整体架构

```
┌──────────────────────┐        HTTP + SSE         ┌────────────────────────────────────┐
│  客户端（idfc.exe）   │ ────────────────────────▶ │  服务器（idfc.exe __server_child）  │
│  仅需 workspace-write │ ◀──────────────────────── │  由 powershell 承载，常驻           │
│                      │     逐行流式回传输出       │  · 已激活 ESP-IDF 环境              │
│  build / flash /     │                           │  · 监听 localhost:19781            │
│  monitor / run / …   │                           │  · 每条请求 spawn 一个 idf.py.exe   │
└──────────────────────┘                           └────────────────────────────────────┘
```

- **端口**：`19781`，路径前缀 `http://localhost:19781/idf-client-cli/`
- **一次激活，多次复用**：服务器启动时只激活一次 ESP-IDF 环境，后续每条命令都复用。
- **实际执行者**：服务器调用 `idf.py.exe`（`%IDF_TOOLS_PATH%\idf-exe\<ver>\idf.py.exe` 启动器）

> ⚠ **重要推论**：服务器是**常驻进程**，在启动时**快照了环境变量**，之后每条命令都把自己那份环境整份复制给 `idf.py` 子进程。
> 因此 **在客户端 shell 里临时 `set` 的环境变量不会影响命令**。需要 `ESPBAUD` 之类的环境变量时，必须在**启动服务器之前**设置好，然后重启服务器。
>
> ⚠ 另一条推论：**服务器的控制台就是启动它的那个窗口**，关掉窗口服务器就会一起停止（它挂在那个控制台上）。要长期挂着就别关窗口；要停就 `idfc stop-server`。

---

## 3. 前置条件

- 已安装 ESP-IDF（含工具链、Python 虚拟环境）。
- 已具备一份 **ESP-IDF 激活脚本**（`.ps1`），例如 `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`（本项目中可能是 5.5.4 或 5.5.5）。
- **`idfc.exe` 必须放在 PATH 中的目录**（从上面的发布页下载）。这样在项目目录里直接 `idfc build` / `idfc start-server` 就能用。
- ⚠ **不要把 `idfc.exe` 放在 DeepSeek Harness 的工作区里**：DSH v0.1.7-rc.1 起会给工作区打上可继承的 **Low 完整性标签**（`icacls` 可见 `Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`），而 Windows 派生进程的完整性取 `min(创建者令牌, 映像文件)`。工作区内的 exe 因此只能以 Low 启动，写不了 `%LOCALAPPDATA%`，服务器的权限自检会直接拒绝启动；**`danger-full-access` 也解决不了**（它只解除 DSH 沙箱，不解除完整性标签）。放到工作区外的 PATH 目录即可，**不需要管理员权限**。

---

## 4. 快速开始

### 4.0 安装（每台开发机做一次）

从 <https://gitee.com/ZYFDroid/idf-client-cli/releases/> 下载 `idfc.exe`，放到 PATH 里的某个目录，然后**新开一个终端**验证：

```powershell
idfc help --all          # 有输出即安装成功（注意：不要写 .\idfc.exe，它不在项目里）
```

> 💡 之后所有命令都在**项目目录**里执行，直接写 `idfc …`。客户端靠当前工作目录找到 `.idfc/idfc_config.json`，所以同一份 exe 可以服务多个项目。

### 4.1 首次配置项目（每个项目目录做一次）

```powershell
idfc set-idfenv "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1"
idfc set-variant debug          # 需存在 sdkconfig.defaults.debug
idfc ports                      # 列出串口（需服务器已启动）
idfc set-port COM12
idfc status                     # 复核当前生效的构建类型与串口
```

配置写入项目目录的 `.idfc/idfc_config.json`：

```json
{
  "IdfActivationScript": "C:\\Espressif\\tools\\Microsoft.v5.5.5.PowerShell_profile.ps1",
  "LastComPort": "COM12",
  "ActiveBuildVariant": "debug"
}
```

> ⚠ 这三个字段是**隐式状态**：不带 `-t`/`-p` 时命令的行为由它们决定。**不确定时先跑 `idfc status`**。

### 4.2 启动 / 停止服务器

```powershell
idfc start-server                          # 已 set-idfenv 时可省略参数
idfc start-server --idfenv "<激活脚本路径>"  # 或显式指定本次使用的脚本
idfc stop-server                           # 停止（会连带终止正在运行的 build/monitor）
```

要点：

- 该命令会派生一个 PowerShell，**dot-source 激活脚本**，再运行常驻工作进程 `__server_child` 监听端口。
- **`start-server` 会轮询服务器状态，确认就绪后才返回**（不会出现"报了成功但服务器其实没起来"）。
- **普通权限即可，无需管理员、无需 `danger-full-access`**——前提是 exe 在 PATH 目录而不是工作区内（见 §3）。
- **权限自检**：启动前服务器会探测能否在 `%LOCALAPPDATA%\IdfClientCLI` 写入文件。若写不了（典型原因是 exe 位于带 Low 完整性标签的工作区），服务器会**拒绝启动**（退出码 2）——这是有意为之，见 §8。跳过检查用 `--skip-permission-check`（不推荐）。
- **服务器是常驻进程，但它挂在启动它的控制台上**：**关掉那个窗口服务器就停了**。要长期挂着就别关窗口。
- 想换环境变量（如 `ESPBAUD`）：先设好变量再 `stop-server` → `start-server`（见 §2 的快照机制）。

### 4.3 日常命令

```powershell
idfc build                                 # 构建（自动检测配置是否过期）
idfc size                                  # 固件体积分析
idfc flash                                 # 烧录到已保存的串口
idfc monitor --wait-for "App started"      # 串口监视，命中后退出
idfc run --monitor-wait-for "App started"  # 构建+体积+烧录+监视一条龙
idfc menuconfig search "IRAM SPIRAM"        # 查配置项（标题/说明/当前值），纯客户端
idfc menuconfig desc CONFIG_BUTTON_LONG_PRESS_MS   # 单项详情（含 help 全文）
idfc idfpy <任意 idf.py 参数...>             # 逃生舱：原样透传给 idf.py
```

---

## 5. 命令速查

| 命令 | 作用 |
|---|---|
| `start-server [--idfenv <ps1>]` | 启动服务器（确认就绪后返回） |
| `stop-server` | 停止服务器（连带终止正在运行的 build/monitor） |
| `help [命令名\|--all]` | 查看帮助 |
| `idfpy <idf.py 参数...>` | 原样透传（`menuconfig`、`erase-flash`、`efuse-*` 等走这里） |
| `build [-t T] [--reconfigure\|--no-reconfigure\|--fullclean] [-v] [--dry-run]` | 构建（过期检测 + 输出折叠） |
| `flash [-t T] [-p COMx] [-v]` | 烧录（**会检测构建是否过期并打印警告**，但不会自动重建；建议先 `build`） |
| `monitor [-t T] [-p COMx] [--no-reset] [--timeout ms] [--grep RE] [--wait-for RE]` | 串口监视（有界） |
| `size [-t T] [--detailed]` | 固件体积分析（`--detailed` 输出极长，谨慎） |
| `menuconfig search "<关键词…>" [--page N]` / `menuconfig desc <名称>` | 查构建产物里的 menuconfig 配置项（标题 / 说明全文 / 当前值）——**纯客户端**，只读 `build/<T>/config/` 的两个 json，不触发构建 |
| `run [-t T] [-p COMx] [--clean] [--no-size] [--verbose-build] [--verbose-flash] [--monitor-*]` | 构建+体积+烧录+监视一条龙 |
| `clean [-t T] [-o …]` | 删除**当前单元格**的 `sdkconfig` 与 `config`（启用矩阵时是 `build/<cell>/…`） |
| `fullclean [-t T] [-o …]` | 删除**当前单元格**整个目录（启用矩阵时是 `build/<cell>/`） |
| `matrix [--json] [-t T] [-o …]` | 列出配置矩阵的全部单元格、状态与当前选择（纯客户端） |
| `set-overlay <维度> <类型\|--default>` | 记住矩阵里某个 overlay 维度的类型（`--default` 清除） |
| `status` | 查看项目配置与服务器状态 |
| `ports` | 列出系统串口（含设备名与实例路径） |
| `set-port COMx` / `set-idfenv <ps1>` / `set-variant <T>` | 记住项目默认配置 |
| `powershell` / `vscode` | 由服务器（以启动它那个进程的权限）在当前目录打开 PowerShell / VS Code |

**共用参数**：`-t <build-type>` 指定构建类型；`-p COMx` 指定串口。**未知命令与未知参数会被明确拒绝**（退出码 1）。

**执行回显（2026-09-23 起）**：`build` / `flash` / `monitor` / `size`（以及 `run` 的各个阶段）在真正执行前会打印一行

```
# Executing `idfc idfpy -B build\debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build\debug\sdkconfig" <子命令>`
```

这行就是本次**实际使用**的完整命令——想手工复现、贴进文档、或确认"到底在构建哪一套"，抄它即可，
不必再去猜隐式状态。⚠ 回显行末尾可能带一个多余空格，复制时无碍。

**关于 monitor 的三个有界参数**（对自动化尤其重要）：

| 参数 | 行为 | 退出码 |
|---|---|---|
| `--wait-for <regex>` | 监视到匹配内容后立即退出 | `0` |
| `--timeout <ms>` | 等待指定毫秒后退出 | `1` |
| `--grep <regex>` | 只输出匹配内容（仅过滤 stdout，stderr 与错误信息始终透出） | — |

两者可同时使用，先满足者生效。客户端退出时（超时、命中或被强杀），服务端都会自动终止进程树并释放串口。

**关于 `menuconfig`（查配置项，2026-09-23 起）**：

| 用法 | 行为 |
|---|---|
| `menuconfig search "关键词1 关键词2" [--page N]` | 多关键词 **AND** 检索：关键词按**字面**匹配（不按正则）、不区分大小写，且必须落在**同一个字段**（`id` / 符号名 / 标题 / 说明）里。每页 20 条，超出时打印匹配总数与翻页提示 |
| `menuconfig desc <名称>` | 单项详情：名称 / 类型 / 取值范围 / 当前值 / 依赖 / 所在菜单路径 / **说明全文**。名称可带可不带 `CONFIG_` 前缀、大小写不限，也接受菜单节点的小写 id（如 `build-type-application-build-type`） |

- **纯客户端命令**：只读 `build/<构建类型>/config/` 下的 `kconfig_menus.json`（全部选项 + 标题 + 说明全文 + 依赖）与 `sdkconfig.json`（当前生效值），**不经过 IDF 服务器、也不触发构建**。
- ⚠ **必须先成功构建过一次**：这两个 json 由 ESP-IDF 在**配置阶段**生成（实测 debug 版 `kconfig_menus.json` 2.6MB / 4079 个节点、`sdkconfig.json` 1730 个键）。缺失时命令会提示先 `build` 并以退出码 1 退出。
- 退出码：`0` 成功；`1` = 参数错误 / 缺少构建产物 / **没有匹配项** / 页码越界。
- 典型用处：改 `main/Kconfig.projbuild` 或调 `sdkconfig` 项之前，先确认"这项是干什么的、现在是多少"，不必翻 IDF 源码或整份 `sdkconfig`。例如 `idfc menuconfig desc CONFIG_BUTTON_LONG_PRESS_MS` 会连说明全文一起打出来。

---

## 6. 本项目特有的坑（重要，含历史记录）

以下几条是**用真实事故换来的**，即使新版已经自动化处理，原理仍值得记住——因为一旦绕过高层命令（直接用 `idfpy`），这些坑会原样回来。

### 6.1 多配置构建：为什么必须带对 `-B`（历史事故）

本项目是 **debug / releasecandidate / release 三套配置**构建。旧版工具要求每条命令手工带 `-B build/<cfg>`：不带时 idfc 会走**根目录 `build/` + 根 `sdkconfig`**（已被 `.gitignore` 忽略、内容陈旧），后果有两种：

- **`build` / `flash`：会拿旧配置去构建**，产物与 `sdkconfig.defaults` 链不一致。**曾经因此产出过"堆毒化仍开"的固件**——`sdkconfig.defaults.debug` 里明明写着关闭，实际固件里 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`，导致内部 RAM 紧张、任务建不起来、SD 读 DMA 失败。这类问题的症状（内存不足、外设偶发失败）会把排查方向带得很偏。
- **`monitor`：日志开头会出现 `Running cmake in directory ...\build`**（白跑一轮 cmake 配置），而且 idf_monitor 取不到该配置的 `.elf`，**调试符号加载不正确**（栈回溯的函数名/行号失真甚至解不出来）。

**新版的处理**：`build`/`flash`/`monitor`/`run`/`size` 都会自动拼 `-B build/<variant>` 与 `-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<variant>"`，**不再需要手工带 `-B`**。若确实要用 `idfpy` 绕过，务必自己带上正确的 `-B`。

### 6.2 sdkconfig 的合并优先级：为什么"改了 defaults 但不生效"（历史事故）

**已存在的 `build/<cfg>/sdkconfig` 优先级高于 `sdkconfig.defaults*`**（`replace=False` 合并）。因此：

> 改了 `sdkconfig.defaults*` → 只跑普通增量构建 → **新配置不生效，而且不报任何错**。
> 唯一可靠的做法是**先删掉 `build/<cfg>/sdkconfig`**，让 defaults 重新生成。

**新版的处理**：`build` 会自动比较 `build/<variant>/sdkconfig` 与 `sdkconfig.defaults`、`sdkconfig.defaults.<variant>`、项目中所有 `Kconfig` / `*.projbuild` 的时间戳，**过期就自动清理并重新配置**（会打印"检测到构建文件已过时，因此进行清理并重新配置"）。想强制重配用 `build --reconfigure`；想跳过检测走纯增量用 `build --no-reconfigure`。

⚠ **`flash` 只警告、不重建**：`idf.py flash` 会隐式构建，所以"改了 defaults 直接 flash"会烧出**旧配置的固件**。现在 `flash` 会先用与 `build` 相同的判定做过期检查，过期时打印：

```
> 警告：当前构建类型 debug 已经过时。当前即将刷入的是旧版的固件。建议重新进行构建，会自动更新配置。
```

但它**不会**替你 clean/reconfigure，也会照常把旧产物烧进去。正确姿势始终是**先 `build` 再 `flash`**（或直接用 `run`，它内部就是这个顺序）。

> 旧文档里"必须先删 `build/<cfg>/sdkconfig` 再带 `-D` reconfigure"这句操作指导已被上面两条命令取代；但**它背后的原因（合并优先级）依然成立**，手工绕过时仍然适用。

### 6.3 串口、波特率与启动器

- **波特率**：`idf.py` 的 `-b/--baud` 是一个声明了 `envvar: ESPBAUD` 的全局选项，所以 `ESPBAUD` 确实有效——但受限于 §2 的环境变量快照机制，**必须在启动服务器之前设置**，否则无效。⚠ idfc **不提供** `flash -b <baud>` 参数：**用户已决定不做**——日常不会改波特率（刷过很多板子都没动过），不必为这条不常走的路线单独加参数；真要用就自己走 `idfpy`。
- **`idf.py.exe` 启动器会按空格重新拆分参数**：这是 `idf-exe` 启动器自身的行为（不是 idfc 的问题）。实测 `idf.py.exe "a b"` 会变成两个参数，而 `python idf.py "a b"` 正常。因此**含空格的参数无法可靠透传**；monitor/flash 的参数（构建类型、COM 口）都经过校验不含空格，不受影响。
- **串口释放有约 1 秒延迟**：客户端退出后服务端会自动清理 `idf.py` 进程树并释放串口（实测约 1.1 秒）。若紧接着执行下一个串口命令遇到"串口被占用"，稍等即可。

### 6.4 安全提示

服务器以**启动它的那个进程的完整权限**执行命令，且 `exec` 端点接受任意 `idf.py` 参数与工作目录。它只监听 `localhost`，但这仍意味着：**任何能在本机发起 HTTP 请求的进程，都等价于获得了同样的执行能力**。因此不要在不可信的环境中常驻服务器（用完 `stop-server`），不要把监听地址改到 `0.0.0.0`，`powershell` / `vscode` 这类"打开满权限交互式进程"的命令只在明确需要时使用。

---

## 7. 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功（含 `monitor --wait-for` 命中） |
| `1` | 参数无效 / 校验失败 / `monitor --timeout` 超时 / 激活脚本未找到 / `menuconfig` 无匹配项或缺构建产物 |
| `2` | 权限探测失败：exe 位于带 Low 完整性标签的目录（如 DSH 工作区），服务器未启动；也会作为 `idf.py` 的退出码透传（如烧录失败） |
| `-1` | 服务器未运行 / 连接错误，或输出流结束但未收到退出码 |
| `<n>` | 透传的 ESP-IDF 命令退出码 |

---

## 8. 常见问题排查

| 现象 | 原因 | 解决办法 |
|---|---|---|
| `IDF 服务器可能尚未运行（… 请求失败）`（退出码 -1） | 服务器未启动，或启动它的窗口被关掉了 | `idfc start-server`（普通权限即可） |
| `[ERROR] 测试权限时写入到 … 失败`（退出码 2） | `idfc.exe` 放在带 **Low 完整性标签**的目录里（典型是 DSH 工作区），写不了 `%LOCALAPPDATA%`，服务器**主动拒绝**启动 | 把 `idfc.exe` 移到 **PATH 中的工作区外目录**再启动（见 §3）。`danger-full-access` 无效，因为拦路的是完整性标签而不是 DSH 沙箱；仅临时调试时可加 `--skip-permission-check`（**不推荐**） |
| `.NET 异常 0xe0434352` / `HttpListener` 崩溃 | 端口绑定失败（通常是被占用，或运行环境异常） | 先 `idfc stop-server` 再重启；确认没有别的进程占用 19781 |
| `没有这样的命令：\`-B\`` / `\`--server\`` | 用了旧版语法 | 见 §1 的迁移对照表 |
| 激活脚本无法加载（ExecutionPolicy 报错） | 执行策略限制 | `start-server` 内部已用 `-ExecutionPolicy Bypass`；手工启动时需自己带上 |
| 端口被占用 | 已有服务器实例 | `idfc stop-server` 后重启 |
| 改了 `sdkconfig.defaults*` 但固件行为没变 | 已有 sdkconfig 优先级更高（§6.2） | `idfc build`（自动检测）或 `build --reconfigure` |
| `flash` 打印"当前构建类型 … 已经过时" | 构建产物早于 defaults/Kconfig（§6.2），而 `flash` 只警告不重建 | 先 `idfc build` 再 `flash`（或直接用 `run`） |
| `menuconfig` 报"缺少 menuconfig 查询所需的数据文件" | 该构建类型还没成功构建过（两个 json 由配置阶段生成） | 先 `idfc build -t <构建类型>`，再重试 |
| `-t <V>` 报"配置文件尚不存在" | 缺少 `sdkconfig.defaults.<V>`，或构建类型拼错 | 确认该文件存在；构建类型只能由字母、数字、下划线组成 |
| 串口被占用 | 上一次 monitor 的进程尚未完全退出 | 约 1 秒内自动清理；仍占用则 `stop-server` 后重启 |
| 想临时改波特率 | 环境变量快照机制（§2） | 在**启动服务器之前**设置 `ESPBAUD`，然后重启服务器 |
| 需要跑 idfc 未封装的命令 | —— | `idfc idfpy <idf.py 参数…>` |

---

## 附：构建产物位置

新版按构建类型分目录（由 `-B build/<variant>` 决定）：

```
build/debug/FakePodNanoNightPlayer.bin          应用固件
build/debug/bootloader/bootloader.bin           引导程序
build/debug/partition_table/partition-table.bin 分区表
build/debug/size_analysis_report.txt            size 命令生成的分析报告
```

烧录：`idfc flash`（或 `idfc run` 一条龙）。
