# MP3InfoScanner（NightPlayerNanoInfoScanner）

> **FakePodNano × NightPlayer — 封面扫描工具（PC 端配套工具）**
>
> 为 **FakePodNanoNightPlayer**（ESP32-S3 开源播放器 FakePodNano 硬件的第三方固件）批量扫描 SD 卡中的 MP3，读取其标签信息与内嵌封面，生成 `.npd` 元数据文件，供固件端 `song_meta` 模块解析并展示「标题 / 副标题 / 回放增益 / 460×460 封面」。

这是一个 C# / .NET Framework 4.8 的 WinForms 桌面工具，运行在 **Windows** 上，以音乐目录（如 SD 卡）为扫描对象，不属于固件代码的一部分。

---

## 功能特性

- **批量扫描**：递归扫描指定目录下所有 `*.mp3`（自动跳过隐藏目录，如 `.ecov` 本身），按文件路径排序逐个处理。
- **标签读取**：基于 [Un4seen BASS](https://www.un4seen.com/) + BASS.NET + `tags` 插件读取 MP3 标签：
  - **标题**：取 ID3 `title`，缺失时回退为文件名。
  - **副标题**：艺术家与专辑都存在时输出 `艺术家 - 专辑`；只存在其一则输出存在的部分；两者皆缺输出 `Various Artist`。
  - **回放增益**：读取 `replaygain_track_gain`（dB 值），标签缺失时兜底为 `-6 dB`。
  - **封面**：取标签内第一张可用图片（最多向前尝试 32 张）。
- **封面归一化**：内嵌封面统一重编码为 **460×460 JPEG**（与播放器屏幕同分辨率）：
  - 正方形原图直接铺满 460×460；
  - 非正方形（宽高差 > 10px）按比例缩放后**黑底居中**；
  - 无封面时不写图段（长度记 0）。
- **增量扫描**：`.npd` 元数据文件已存在且比对应 MP3 新（5 秒容差）时跳过；生成后把 `.npd` 的最后修改时间对齐为 MP3 的修改时间，后续扫描只处理新增/更新的文件。
- **运行中反馈**：界面实时预览当前封面、输出日志、显示当前曲目信息与扫描进度。
- **扫描期间禁止关窗**：避免中途退出导致元数据不完整。

---

## 工作原理与产物

扫描完成后，程序在音乐目录根下建立隐藏目录 `.ecov/`，**镜像** MP3 目录结构，为每首歌生成一个同名的 `.npd` 文件：

```
音乐根目录/
├── 歌手A/
│   ├── 专辑1/01 曲目.mp3
│   └── ...
└── .ecov/            ← 生成的元数据目录（隐藏，固件同样在此查找）
    ├── 歌手A/
    │   └── 专辑1/01 曲目.mp3.npd
    └── ...
```

固件端把 `/sdcard/abc/x.mp3` 映射为 `/sdcard/.ecov/abc/x.mp3.npd`（见固件 `main/core/music/song_meta.c` 的 `song_meta_build_npd_path()`）。

### `.npd` 文件格式（二进制，小端）

| 顺序 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 1 | 标题 | `uint32 长度 + UTF-8 字节` | 标题文本（缺省时为空串） |
| 2 | 副标题 | `uint32 长度 + UTF-8 字节` | 副标题文本（缺省时为空串） |
| 3 | 回放增益 | `uint32` | dB → 线性（`10^(dB/20)`）× 65536 取整后的无符号值 |
| 4 | 封面 | `uint32 长度 + JPEG 字节` | 460×460 JPEG；长度为 0 表示无封面 |

固件端解析约束：文本字段上限 512 字节（UTF-8），JPEG 上限 256 KB（460×460 JPEG 一般 60~120 KB）。与 `song_meta_load()` 的读取逻辑一一对应，改动格式时需同步固件端代码。

---

## 目录结构

```
MP3InfoScanner/
├── MP3InfoScanner.slnx              # 解决方案文件（XML 格式，需 VS2022 17.13+ 或等效工具链）
├── .gitignore                       # 根忽略规则（VS 标准模板：bin/obj/.vs/packages 等）
├── packages/                        # NuGet 还原产物（不入库）
└── MP3InfoScanner/
    ├── MP3InfoScanner.csproj        # 项目文件（TargetFramework: net48）
    ├── packages.config              # NuGet 依赖：Costura.Fody / Fody / JetBrains.Annotations
    ├── App.config
    ├── FodyWeavers.xml              # Fody 织入配置（Costura 内嵌托管依赖）
    ├── Program.cs                   # 入口：释放 BASS 原生库到 %TEMP% 并加载
    ├── Form1.cs / Form1.Designer.cs / Form1.resx
    ├── Properties/                  # AssemblyInfo、Resources（内嵌 bass.dll/tags.dll）、Settings
    ├── dlls/                        # ⚠ 需手动放置 BASS.NET（见下）
    │   ├── .gitignore               #   忽略 Bass.Net.dll / Bass.Net.xml
    │   └── Bass.Net.dll / .xml      #   ← 手动下载后放入（不入库）
    └── BassDLL/                     # ⚠ 需手动放置 BASS 原生库（见下）
        ├── .gitignore               #   忽略 bass.dll / tags.dll
        └── bass.dll / tags.dll      #   ← 手动下载后放入（不入库，编译时作为资源内嵌）
```

> 说明：程序集名称为 `NightPlayerNanoInfoScanner`（生成的 exe 名），`RootNamespace` 保留历史值 `TestMP3`；窗体标题为「FakePodNano × NightPlayer - 封面扫描工具」。

---

## ⚠ 两处需要特殊处理的 `.gitignore`

项目中共有 **三处** `.gitignore`：

1. **根目录 `.gitignore`** —— 常规 Visual Studio 模板，忽略 `bin/`、`obj/`、`.vs/`、`packages/` 等构建产物与 IDE 文件，**无需处理**。
2. **`MP3InfoScanner/dlls/.gitignore`** —— ⚠ 需要特殊处理。
3. **`MP3InfoScanner/BassDLL/.gitignore`** —— ⚠ 需要特殊处理。

后两处各自**忽略了所在目录下编译/运行所必需的第三方二进制**：BASS 是闭源软件，受其自有许可约束，二进制不随源码分发，因此刻意不入库。**全新克隆仓库后这些文件是缺失的，必须手动下载放置后才能编译运行**。

### ① `MP3InfoScanner/dlls/` —— BASS.NET（托管封装）

`.gitignore` 内容忽略：

```
Bass.Net.dll
Bass.Net.xml
```

**需手动放置**（构建时编译引用，`csproj` 的 HintPath 指向本目录）：

- `Bass.Net.dll`
- `Bass.Net.xml`

**获取方式**：前往 <https://www.un4seen.com/#apis> 下载 **Bass.Net**，取 **full（完整）版本** 的两个文件放入 `MP3InfoScanner/dlls/`。

### ② `MP3InfoScanner/BassDLL/` —— BASS 原生库（含 tags 插件）

`.gitignore` 内容忽略：

```
bass.dll
tags.dll
```

**需手动放置**（编译时经 `Properties/Resources.resx` 作为二进制资源内嵌进 exe；运行时由 `Program.cs` 释放到 `%TEMP%\TestMP3PictureInfo\bass` 再加载，因此**运行 exe 无需额外携带 dll**）：

- `bass.dll` —— BASS 音频库本体（**64 位**）
- `tags.dll` —— BASS Tags 插件（**64 位**，Add-ons 栏目下）

**获取方式**：前往 <https://www.un4seen.com/> 下载 **最新 64 位** `bass.dll`，以及 Add-ons 一列中的 **64 位** `tags.dll`，放入 `MP3InfoScanner/BassDLL/`。

> 下载链接与放置说明同样写在各 `.gitignore` 文件内的中文注释中，克隆仓库后照着做即可。

---

## 环境要求与构建

- **系统**：Windows
- **SDK**：.NET Framework 4.8 Developer Pack（目标框架 `net48`，见 `App.config` 与 `csproj`）
- **IDE/工具链**：Visual Studio 2022（**17.13 及以上**，支持 `.slnx` XML 解决方案格式），或安装对应版本的 MSBuild 命令行构建
- **NuGet**：构建前需还原 `packages.config` 声明的依赖：
  - `Costura.Fody 6.2.0`（把托管依赖（含 Bass.Net.dll）织入单个 exe，配合 `FodyWeavers.xml`）
  - `Fody 6.9.3`
  - `JetBrains.Annotations 2018.3.0`

**构建前清单**：

1. ✅ 还原 NuGet 包；
2. ✅ 按上文「两处需要特殊处理的 .gitignore」放置 `dlls/` 与 `BassDLL/` 下的 4 个文件；
3. 编译（Debug / Release 均可），产物为 `bin\<配置>\NightPlayerNanoInfoScanner.exe`（单文件，原生 dll 已内嵌为资源）。

---

## 使用说明

1. 将编译好的 `NightPlayerNanoInfoScanner.exe` **复制到音乐目录根**（如 SD 卡根目录），双击运行；
   - **Release** 构建：扫描目录 = exe 所在目录（`Application.StartupPath`）；
   - **Debug** 构建：扫描固定目录 `Q:\MP3Player`（开发机调试用，可在 `Form1.cs` 构造函数中修改 `RootDir`）。
2. 点击 **「开始扫描」**：
   - 左上面板实时预览当前歌曲封面；
   - 右下日志面板输出扫描进度与每首歌的标签处理情况；
   - 按钮文字显示 `扫描中：当前/总数`；
   - 生成/更新的 `.npd` 文件写入 `<音乐目录>/.ecov/` 下。
3. 再次运行只会处理新增或变更过的 MP3（增量扫描）。

> 扫描期间窗口被禁止关闭（`FormClosing` 中取消），请等待扫描完成。
>
> 扫描完成后将 SD 卡插回播放器，固件即按 `/sdcard/.ecov/<相对路径>.mp3.npd` 读取元数据与封面（无 `.npd` 时固件回退到文件名等默认信息）。

---

## 技术要点

- **BASS 原生库不随 exe 外置**：`bass.dll` / `tags.dll` 编译时内嵌为程序资源，`Program.cs` 首次运行把它们写出到 `%TEMP%\TestMP3PictureInfo\bass`（含 `plugin/` 子目录）后 `Bass.LoadMe` + `BASS_PluginLoad` 加载，避免污染程序目录、免安装即可运行。
- **Costura.Fody 单文件化**：`Bass.Net.dll`（以及 JetBrains.Annotations 等托管依赖）在构建期织入主 exe，最终只分发一个 exe。
- **回放增益换算**：dB → 线性：`10^(dB/20)`，再 ×65536 取整存为 `uint32`（固件端按 Q16 定点解读）；标签无 ReplayGain 时按 `-6 dB` 兜底。
- **封面取图顺序**：遍历标签图片直到取到第一张可解码图片，保证对多图标签取到稳定结果。

---

## 版权与许可提示

- BASS / BASS.NET / BASS Tags 为 [Un4seen Developments](https://www.un4seen.com/) 的闭源软件，免费供**非商业**用途；商业使用需购买授权。使用本项目前请阅读并遵守 un4seen 的许可条款。
- 本项目源码（除上述第三方二进制外）遵循 FakePodNanoNightPlayer 项目自身的开源许可。
