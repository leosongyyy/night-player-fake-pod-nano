# idfc.exe 使用说明（ESP-IDF 命令行客户端 / 服务器）

`idfc.exe` 是 ESP-IDF 的命令行客户端/服务器工具，用于在 DeepSeek Harness 环境下便捷地执行 `idf.py` 构建、烧录、监视等命令。它采用 **服务器 + 客户端** 架构：服务器一次性激活 ESP-IDF 环境，然后通过 HTTP + SSE（Server-Sent Events）把 `idf.py` 命令的输出流式回传给客户端。

> ⚠ **本文档对应当前的「子命令版」idfc.exe。**
> 旧版语法（`--server` / `-B build/<cfg> build` / `-B ... reconfigure` / `raw-idf`）**已全部失效**，现在一律报 `没有这样的命令`（退出码 1）。历史对话、旧文档、旧笔记里的 idfc 命令**不要照抄**——先看 §1 的迁移对照。
> 全部命令与参数请用 `.\idfc.exe help --all` 查询（它是唯一的事实源）。

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
│  仅需 workspace-write │ ◀──────────────────────── │  由 powershell 承载，满权限常驻     │
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

---

## 3. 前置条件

- 已安装 ESP-IDF（含工具链、Python 虚拟环境）。
- 已具备一份 **ESP-IDF 激活脚本**（`.ps1`），例如 `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`（本项目中可能是 5.5.4 或 5.5.5）。
- 本工具面向 **DeepSeek Harness 沙箱** 设计：服务器绑定 HTTP.sys 监听端口通常需要 `danger-full-access` 权限。

---

## 4. 快速开始

### 4.1 首次配置项目（每个项目目录做一次）

```powershell
.\idfc.exe set-idfenv "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1"
.\idfc.exe set-variant debug          # 需存在 sdkconfig.defaults.debug
.\idfc.exe ports                      # 列出串口（需服务器已启动）
.\idfc.exe set-port COM12
.\idfc.exe status                     # 复核当前生效的构建类型与串口
```

配置写入项目目录的 `.idfc/idfc_config.json`：

```json
{
  "IdfActivationScript": "C:\\Espressif\\tools\\Microsoft.v5.5.5.PowerShell_profile.ps1",
  "LastComPort": "COM12",
  "ActiveBuildVariant": "debug"
}
```

> ⚠ 这三个字段是**隐式状态**：不带 `-t`/`-p` 时命令的行为由它们决定。**不确定时先跑 `.\idfc.exe status`**。

### 4.2 启动 / 停止服务器

```powershell
.\idfc.exe start-server                          # 已 set-idfenv 时可省略参数
.\idfc.exe start-server --idfenv "<激活脚本路径>"  # 或显式指定本次使用的脚本
.\idfc.exe stop-server                           # 停止（会连带终止正在运行的 build/monitor）
```

也可以让用户直接运行项目根目录的 `StartIDFCServer.vbs`。

要点：

- 该命令会派生一个 PowerShell，**dot-source 激活脚本**，再运行常驻工作进程 `__server_child` 监听端口。
- **`start-server` 会轮询服务器状态，确认就绪后才返回**（不再出现"报了成功但服务器其实没起来"）。
- **沙箱权限检查**：启动前服务器会探测能否在 `%LOCALAPPDATA%\IdfClientCLI` 写入文件。若当前沙箱禁止写入，服务器会**拒绝启动**（退出码 2）——这是有意为之，见 §8。跳过检查用 `--skip-permission-check`（不建议）。
- **DeepSeek Harness 实测**：要让监听端口正常绑定，需在 **`danger-full-access`** 权限下运行服务器，否则 `HttpListener` 会因无法绑定 HTTP.sys 而崩溃（.NET 异常 `0xe0434352`）。
- 服务器是**常驻后台进程**，会话结束后可能被回收；每次遇到连接错误都应先检查/提醒启动服务器。

### 4.3 日常命令

```powershell
.\idfc.exe build                                 # 构建（自动检测配置是否过期）
.\idfc.exe size                                  # 固件体积分析
.\idfc.exe flash                                 # 烧录到已保存的串口
.\idfc.exe monitor --wait-for "App started"      # 串口监视，命中后退出
.\idfc.exe run --monitor-wait-for "App started"  # 构建+体积+烧录+监视一条龙
.\idfc.exe idfpy <任意 idf.py 参数...>             # 逃生舱：原样透传给 idf.py
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
| `flash [-t T] [-p COMx] [-v]` | 烧录（**不检测构建是否过期**，建议先 `build`） |
| `monitor [-t T] [-p COMx] [--no-reset] [--timeout ms] [--grep RE] [--wait-for RE]` | 串口监视（有界） |
| `size [-t T] [--detailed]` | 固件体积分析（`--detailed` 输出极长，谨慎） |
| `run [-t T] [-p COMx] [--clean] [--no-size] [--verbose-build] [--verbose-flash] [--monitor-*]` | 构建+体积+烧录+监视一条龙 |
| `clean [-t T]` | 删除 `build/<T>/sdkconfig` 与 `build/<T>/config` |
| `fullclean [-t T]` | 删除整个 `build/<T>/` |
| `status` | 查看项目配置与服务器状态 |
| `ports` | 列出系统串口（含设备名与实例路径） |
| `set-port COMx` / `set-idfenv <ps1>` / `set-variant <T>` | 记住项目默认配置 |
| `powershell` / `vscode` | 由服务器（满权限）在当前目录打开 PowerShell / VS Code |

**共用参数**：`-t <build-type>` 指定构建类型；`-p COMx` 指定串口。**未知命令与未知参数会被明确拒绝**（退出码 1）。

**关于 monitor 的三个有界参数**（对自动化尤其重要）：

| 参数 | 行为 | 退出码 |
|---|---|---|
| `--wait-for <regex>` | 监视到匹配内容后立即退出 | `0` |
| `--timeout <ms>` | 等待指定毫秒后退出 | `1` |
| `--grep <regex>` | 只输出匹配内容（仅过滤 stdout，stderr 与错误信息始终透出） | — |

两者可同时使用，先满足者生效。客户端退出时（超时、命中或被强杀），服务端都会自动终止进程树并释放串口。

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

> 旧文档里"必须先删 `build/<cfg>/sdkconfig` 再带 `-D` reconfigure"这句操作指导已被上面两条命令取代；但**它背后的原因（合并优先级）依然成立**，手工绕过时仍然适用。

### 6.3 串口、波特率与启动器

- **波特率**：`idf.py` 的 `-b/--baud` 是一个声明了 `envvar: ESPBAUD` 的全局选项，所以 `ESPBAUD` 确实有效——但受限于 §2 的环境变量快照机制，**必须在启动服务器之前设置**，否则无效。
- **`idf.py.exe` 启动器会按空格重新拆分参数**：这是 `idf-exe` 启动器自身的行为（不是 idfc 的问题）。实测 `idf.py.exe "a b"` 会变成两个参数，而 `python idf.py "a b"` 正常。因此**含空格的参数无法可靠透传**；monitor/flash 的参数（构建类型、COM 口）都经过校验不含空格，不受影响。
- **串口释放有约 1 秒延迟**：客户端退出后服务端会自动清理 `idf.py` 进程树并释放串口（实测约 1.1 秒）。若紧接着执行下一个串口命令遇到"串口被占用"，稍等即可。

### 6.4 安全提示

服务器以**启动它的那个进程的完整权限**执行命令，且 `exec` 端点接受任意 `idf.py` 参数与工作目录。它只监听 `localhost`，但这仍意味着：**任何能在本机发起 HTTP 请求的进程，都等价于获得了同样的执行能力**。因此不要在不可信的环境中常驻服务器（用完 `stop-server`），不要把监听地址改到 `0.0.0.0`，`powershell` / `vscode` 这类"打开满权限交互式进程"的命令只在明确需要时使用。

---

## 7. 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功（含 `monitor --wait-for` 命中） |
| `1` | 参数无效 / 校验失败 / `monitor --timeout` 超时 / 激活脚本未找到 |
| `2` | 沙箱权限探测失败（服务器未启动）；也会作为 `idf.py` 的退出码透传（如烧录失败） |
| `-1` | 服务器未运行 / 连接错误，或输出流结束但未收到退出码 |
| `<n>` | 透传的 ESP-IDF 命令退出码 |

---

## 8. 常见问题排查

| 现象 | 原因 | 解决办法 |
|---|---|---|
| `IDF 服务器可能尚未运行（… 请求失败）`（退出码 -1） | 服务器未启动或被回收 | `.\idfc.exe start-server`（或让用户运行 `StartIDFCServer.cmd`） |
| `.NET 异常 0xe0434352` / `HttpListener` 崩溃 | 沙箱阻止 HTTP.sys 端口绑定 | 用 `danger-full-access` 权限启动服务器 |
| `[ERROR] 测试权限时写入到 … 失败`（退出码 2） | 当前处于 `workspace-write` 沙箱，服务器**主动拒绝**启动 | 用满权限启动；仅临时调试时可加 `--skip-permission-check`（**不推荐**，受限沙箱里起的服务器会让后续构建出现难以归因的失败） |
| `没有这样的命令：\`-B\`` / `\`--server\`` | 用了旧版语法 | 见 §1 的迁移对照表 |
| 激活脚本无法加载（ExecutionPolicy 报错） | 执行策略限制 | `start-server` 内部已用 `-ExecutionPolicy Bypass`；手工启动时需自己带上 |
| 端口被占用 | 已有服务器实例 | `.\idfc.exe stop-server` 后重启 |
| 改了 `sdkconfig.defaults*` 但固件行为没变 | 已有 sdkconfig 优先级更高（§6.2） | `.\idfc.exe build`（自动检测）或 `build --reconfigure` |
| `-t <V>` 报"配置文件尚不存在" | 缺少 `sdkconfig.defaults.<V>`，或构建类型拼错 | 确认该文件存在；构建类型只能由字母、数字、下划线组成 |
| 串口被占用 | 上一次 monitor 的进程尚未完全退出 | 约 1 秒内自动清理；仍占用则 `stop-server` 后重启 |
| 想临时改波特率 | 环境变量快照机制（§2） | 在**启动服务器之前**设置 `ESPBAUD`，然后重启服务器 |
| 需要跑 idfc 未封装的命令 | —— | `.\idfc.exe idfpy <idf.py 参数…>` |

---

## 附：构建产物位置

新版按构建类型分目录（由 `-B build/<variant>` 决定）：

```
build/debug/FakePodNanoNightPlayer.bin          应用固件
build/debug/bootloader/bootloader.bin           引导程序
build/debug/partition_table/partition-table.bin 分区表
build/debug/size_analysis_report.txt            size 命令生成的分析报告
```

烧录：`.\idfc.exe flash`（或 `.\idfc.exe run` 一条龙）。
