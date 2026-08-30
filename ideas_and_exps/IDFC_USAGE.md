# idfc.exe 使用说明（ESP-IDF 命令行客户端）

`idfc.exe` 是 ESP-IDF 的命令行客户端/服务器工具，用于在 DeepSeek Harness 环境下便捷地执行 `idf.py` 构建、烧录等命令。它采用 **服务器 + 客户端** 架构：服务器一次性激活 ESP-IDF 环境，然后通过 HTTP + SSE（Server-Sent Events）把 `idf.py` 命令的输出流式回传给客户端。

---

## 1. 整体架构

```
┌──────────────┐   HTTP/SSE    ┌──────────────────────────────────┐
│  客户端进程   │ ────────────▶ │  服务器（后台 PowerShell 子进程） │
│  idfc build  │ ◀──────────── │  · 加载激活脚本（激活 IDF 环境） │
└──────────────┘   流式输出     │  · 监听 http://localhost:19781   │
                               │  · 执行 idf.py.exe 并转发输出    │
                               └──────────────────────────────────┘
```

- **端口**：`19781`，路径前缀 `http://localhost:19781/idf/`
- **一次激活，多次复用**：服务器启动时只激活一次 ESP-IDF 环境，后续每条命令都复用该环境。

---

## 2. 前置条件

- 已安装 ESP-IDF（含工具链、Python 虚拟环境等）。
- 已具备一份 **ESP-IDF 激活脚本**（`.ps1`），例如：
  `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`
  （该脚本会设置 `IDF_PATH`、`IDF_TOOLS_PATH` 并把工具链加入 `PATH`。）
- 本工具面向 **DeepSeek Harness 沙箱** 设计：服务器绑定 HTTP.sys 监听端口通常需要 `danger-full-access` 权限。

---

## 3. 命令用法

```
idfc <command...>                 # 通过运行中的服务器执行一条 ESP-IDF 命令
idfc --server --idfenv <脚本路径>  # 启动 IDF 服务器（后台 PowerShell）
idfc --help                       # 显示帮助
```

### 3.1 启动服务器

```powershell
idfc --server --idfenv "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1"
```

要点：

- 该命令会启动一个 PowerShell，**dot-source 激活脚本**，再运行工作进程 `--server-child` 监听端口。
- 子进程启动后，当前 CLI 会话即退出——因此建议用 `pwsh` 前台启动，而不要放到会回收子进程的“后台任务”里（后台任务结束时可能连带杀掉服务器子进程）。
- **沙箱权限检查**：启动前服务器会探测能否在 `%LOCALAPPDATA%\IdfClientCLI` 写入文件。若当前沙箱禁止写入，服务器会拒绝启动（退出码 2），除非跳过检查：
  ```powershell
  idfc --server --idfenv <脚本路径> --skip-permission-check
  ```
- **DeepSeek Harness 实测**：要让监听端口正常绑定，需在 **`danger-full-access`** 权限下运行服务器（否则 `HttpListener` 会因无法绑定 HTTP.sys 而崩溃，.NET 异常 `0xe0434352`）。推荐让后台任务本身直接承载 `--server-child`，从而既保证服务器常驻、又具备完整权限：
  ```powershell
  powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . '<激活脚本>' } ; & '<idfc.exe路径>' --server-child"
  ```

### 3.2 执行命令（客户端）

服务器运行后，在**工作区写权限（workspace-write）**环境下执行任意 ESP-IDF 命令：

```powershell
idfc build              # 构建
idfc -p COM3 flash      # 烧录到指定串口
idfc flash              # 烧录（使用默认端口）
idfc menuconfig         # 配置菜单
```

- 多条参数自动按词合并、含空格的参数会加引号后传给服务器。
- 标准输出/标准错误逐行通过 SSE 流式打印到本进程，退出码原样透传。

---

## 4. 参数说明

| 参数 | 适用 | 说明 |
|---|---|---|
| `--idfenv <path>` | 服务器 | ESP-IDF 激活 PowerShell 脚本路径 |
| `--skip-permission-check` | 服务器 | 跳过文件写入探测（沙箱禁写时使用） |
| `--help` | 通用 | 显示帮助 |
| `--server` | 服务器 | 启动服务器并派生子进程 `--server-child` |
| `--server-child` | 内部 | 实际监听并执行命令的工作进程（不建议直接调用） |

---

## 5. 退出码

| 退出码 | 含义 |
|---|---|
| `0` | 成功 |
| `1` | 参数无效 / 激活脚本未找到 |
| `2` | 沙箱权限探测失败（服务器未启动） |
| `-1` | 服务器未运行 / 连接错误 |
| `<n>` | 透传的 ESP-IDF 命令退出码 |

---

## 6. 常见问题排查

| 现象 | 原因 | 解决办法 |
|---|---|---|
| `IDF server is not running at http://localhost:19781/idf.` | 服务器未启动或被回收 | 先启动服务器；用常驻后台任务承载 `--server-child` |
| `.NET 异常 0xe0434352` / `HttpListener` 崩溃 | 沙箱阻止 HTTP.sys 端口绑定 | 用 `danger-full-access` 运行服务器 |
| `Cannot write to ...\IdfClientCLI`（退出码 2） | 沙箱禁止写入探测文件 | 加 `--skip-permission-check` |
| 激活脚本无法加载（ExecutionPolicy 报错） | 执行策略限制 | 用 `-ExecutionPolicy Bypass` 运行 PowerShell |
| 端口被占用 | 已有服务器实例 | 关闭旧实例或更换端口（需改源码 `port` 常量） |

---

## 7. 构建产物参考（ESP32-S3 AMOLED LVGL v8 示例）

```powershell
idfc build
# Project build complete.
# 产物：
#   build\FakePodNanoNightPlayer.bin         应用固件
#   build\bootloader\bootloader.bin          引导程序
#   build\partition_table\partition-table.bin 分区表
# 烧录：idfc flash  或  idfc -p COM端口 flash
```
