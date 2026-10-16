# BUILD_MATRIX —— 配置矩阵的定义、`idfc_buildconfig.json` 格式与 idfc 实现说明

> **状态**：固件侧（本仓库）**已就绪**（配置项 + overlay 配置 + `idfc_buildconfig.json` 均已入库），
> idfc 侧**尚未实现**（当前已安装的 `idfc.exe` 不读 `idfc_buildconfig.json`，仍然只认 `.idfc/idfc_config.json` + `sdkconfig.defaults.<variant>`）。
>
> **本文档的用途**：给 idfc 仓库（`Q:\Projects\CSharp\idf-client-cli\`，代码基线 **commit `85a73ab`（2026-10-01，工作区干净）**）写的一份实现说明书。第 1 节定义"配置矩阵是什么"，第 2 节定义配置文件格式，第 3 节定义命令行行为，第 4 节**逐文件、逐调用点**列出修改点，第 5 节是兼容性与失败模式，第 6 节是实施顺序与自测清单。
>
> 固件侧的使用说明（本项目的 `dac_chip` 维度、DAC 配置项、构建命令）见 `ideas_and_exps/BUILD_CONFIG.md` §8 与 AGENTS.md 第 6 节。

---

## 1. 配置矩阵的定义

### 1.1 现状与动机

现在 idfc 只认识**一维**的"构建类型"（build variant）：`debug` / `releasecandidate` / `release`，一个 variant 对应

```
构建目录   build/<variant>/
配置链     sdkconfig.defaults ; sdkconfig.defaults.<variant>
生成配置   build/<variant>/sdkconfig
```

但真实硬件/固件往往还有**正交的第二维度**：同一份固件代码要针对不同芯片变种构建。本项目的例子是 **DAC 芯片**（`cs43131` / `pcm5102`），将来还可能加"芯片型号"（S3 / S31）等维度。把二维（或多维）展开成 `debug`、`debug_pcm5102`… 这类扁平名字会随维度数**指数爆炸**，也无法表达"某个维度选了什么"。

**配置矩阵**就是把这件事显式化：构建类型是一个**维度**，每个额外维度是一个 **Overlay**，它们的笛卡尔积就是所有可构建的**单元格（cell）**，每个单元格 = 一套独立的构建目录 + 配置文件链 + `idf.py` 参数。

### 1.2 术语

| 术语 | 含义 | 本项目实例 |
|---|---|---|
| **构建矩阵 / 配置矩阵** | 由一个 variant 维度与 N 个 overlay 维度构成的笛卡尔积 | 3 × 2 = 6 格 |
| **BuildVariant（构建类型）** | 主维度的一个取值，对应 `sdkconfig.defaults.<Name>` | `debug`、`releasecandidate`、`release` |
| **Overlay 维度** | 除构建类型外的一个正交维度，**单选**（每格只取一个类型） | `dac_chip` |
| **OverlayType（维度类型）** | 某个 overlay 维度的取值，可携带自己的配置文件 | `cs43131`、`pcm5102` |
| **单元格 / cell** | 一个"variant + 每个维度各一个类型"的组合，即一次具体构建 | `build/cs43131.debug` |
| **矩阵定义文件** | 描述维度、类型与单元格命名的 JSON，位于项目根目录 | `idfc_buildconfig.json` |
| **选择（selection）** | "当前用哪一格"，由命令行 `-t`/`-o` 与 `.idfc` 里的持久化状态决定 | `-t debug -o dac_chip=pcm5102` |

**关键约束**：每个 overlay 维度在每一格里**必须恰好有一个类型**（不允许"不选"）。因此每个维度必须声明 `DefaultType`，未显式指定的维度一律取默认类型 —— 这样**任意 (variant, 若干 -o) 都能唯一确定一个单元格**，不存在"半成品配置"。单元格总数 = `|BuildVariants| × Π|OverlayTypes(d)|`。

### 1.3 一个单元格由什么决定

给定"矩阵定义 + variant + 各维度类型"，idfc 就能拼出这四样东西（现在的 `GetCommonIDFArgsPrefix()` 已经在做其中三样，只是写死成 `<variant>`）：

```
① 构建目录      OutputDir 模板实例化，例：build/cs43131.debug
② 配置链        BaseConfig → variant.ConfigFile → 各 overlay 选中类型的 ConfigFile（按 Overlays 声明顺序）
                例：sdkconfig.defaults ; sdkconfig.defaults.debug ; sdkconfig.dac_chip.cs43131
③ 生成 sdkconfig 目录内固定名 sdkconfig，例：build/cs43131.debug/sdkconfig
④ idf.py 参数    -B <构建目录>
                -D SDKCONFIG_DEFAULTS="<配置链，分号分隔>"
                -D SDKCONFIG="<构建目录>/sdkconfig"
```

命令行形如（与今天完全同构，只是路径变了）：

```powershell
idfc idfpy -B build\pcm5102.debug -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.pcm5102" -D SDKCONFIG="build\pcm5102.debug\sdkconfig" build
```

### 1.4 合并顺序与优先级

配置文件按**后加载覆盖先加载**（ESP-IDF kconfgen 的 `replace=False` 语义，见 `IDFC_USAGE.md` §6.2）：

```
sdkconfig.defaults                      ← BaseConfig（公共配置，必须存在）
sdkconfig.defaults.<variant>            ← variant 维度
sdkconfig.dac_chip.<类型>                ← overlay 维度（多个维度时按 Overlays 数组顺序）
────────────────────────────────────────
= build/<cell>/sdkconfig
```

因此 **overlay 文件可以覆盖公共/variant 里的任何项**，这也是本项目 `sdkconfig.dac_chip.pcm5102` 必须显式写 `# CONFIG_AUDIO_DAC_CS43131 is not set` 的原因（公共默认开着 CS43131，两项同时为 y 会让固件在 `main.c` 的 `#if` 处 `#error` 编译失败）。

> ⚠ 这是**当前唯一"错了也不静默"的兜底**：矩阵接错了（比如 overlay 文件没被合并进去）会在编译期炸掉，而不是烧出一版"看起来对、其实芯片选错"的固件。idfc 侧的校验（§2.7）要在更早的地方拦住同类错误。

### 1.5 单元格的命名（cell 名）

单元格的**唯一标识就是实例化后的 `OutputDir`**（例：`build/cs43131.debug`），其最后一段目录名（`cs43131.debug`）作为**人读的 cell 名**，用于日志、`status`、`matrix` 列表与过期警告：

```
> 当前构建目标： cs43131.debug （dac_chip=cs43131, variant=debug）
> 构建目录： build/cs43131.debug
```

不再另外发明一套命名（例如 `debug-cs43131`），避免"目录名"和"cell 名"两套东西对不上。

### 1.6 与 `.idfc/idfc_config.json` 的关系

`.idfc/idfc_config.json` 继续保存**隐式选择状态**（不写进版本库）：

```json
{
  "IdfActivationScript": "C:\\Espressif\\tools\\Microsoft.v5.5.5.PowerShell_profile.ps1",
  "LastComPort": "COM5",
  "ActiveBuildVariant": "debug",
  "ActiveOverlays": { "dac_chip": "cs43131" }
}
```

- `ActiveBuildVariant`：对应 `-t`（已有）。
- `ActiveOverlays`：**新增**，维度名 → 类型名；对应 `-o`。缺失的维度用矩阵里的 `DefaultType`。
- 命令行 `-t` / `-o` 优先级高于这两个字段（与今天 `-t` 的语义一致）。

### 1.7 本项目的当前矩阵（实例）

`idfc_buildconfig.json`（本仓库根目录）当前定义：

- variant 维度：`debug` / `releasecandidate` / `release`
- overlay 维度 1：`dac_chip`，类型 `cs43131`（默认）/ `pcm5102`
- overlay 维度 2：`font`，类型 `ttffont`（默认）/ `binfont`（2026-10-25 新增）
- `OutputDir`：`build/{dac_chip}.{font}.{variant}`

共 **12 个单元格**。为节省篇幅，下面按"配置链 = 公共前缀 + 两个 overlay + variant"归纳（后缀顺序固定为
`dac_chip` → `font` → `variant`，与矩阵里 `Overlays[]` 的声明顺序一致）：

| cell 目录 | 配置链（`-D SDKCONFIG_DEFAULTS=` 的分号列表） |
|---|---|
| `build/cs43131.ttffont.debug` | `sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.cs43131;sdkconfig.font.ttffont` |
| `build/cs43131.ttffont.releasecandidate` | `sdkconfig.defaults;sdkconfig.defaults.releasecandidate;sdkconfig.dac_chip.cs43131;sdkconfig.font.ttffont` |
| `build/cs43131.ttffont.release` | `sdkconfig.defaults;sdkconfig.defaults.release;sdkconfig.dac_chip.cs43131;sdkconfig.font.ttffont` |
| `build/cs43131.binfont.debug` | `sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.cs43131;sdkconfig.font.binfont` |
| `build/cs43131.binfont.releasecandidate` | `sdkconfig.defaults;sdkconfig.defaults.releasecandidate;sdkconfig.dac_chip.cs43131;sdkconfig.font.binfont` |
| `build/cs43131.binfont.release` | `sdkconfig.defaults;sdkconfig.defaults.release;sdkconfig.dac_chip.cs43131;sdkconfig.font.binfont` |
| `build/pcm5102.ttffont.debug` | `sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.pcm5102;sdkconfig.font.ttffont` |
| `build/pcm5102.ttffont.releasecandidate` | `sdkconfig.defaults;sdkconfig.defaults.releasecandidate;sdkconfig.dac_chip.pcm5102;sdkconfig.font.ttffont` |
| `build/pcm5102.ttffont.release` | `sdkconfig.defaults;sdkconfig.defaults.release;sdkconfig.dac_chip.pcm5102;sdkconfig.font.ttffont` |
| `build/pcm5102.binfont.debug` | `sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.pcm5102;sdkconfig.font.binfont` |
| `build/pcm5102.binfont.releasecandidate` | `sdkconfig.defaults;sdkconfig.defaults.releasecandidate;sdkconfig.dac_chip.pcm5102;sdkconfig.font.binfont` |
| `build/pcm5102.binfont.release` | `sdkconfig.defaults;sdkconfig.defaults.release;sdkconfig.dac_chip.pcm5102;sdkconfig.font.binfont` |

### 1.8 `font` 维度：binfont / ttffont

| 类型 | 含义 | 关键开关（`sdkconfig.font.*`） | 实测 app 体积（debug） |
|---|---|---|---|
| `ttffont`（默认） | TTF + FreeType，字体在 flash 的 `font` 分区 | `UI_FONT_TTF=y`、`LV_USE_FREETYPE=y`、`UI_SYSTEM_FONT_FULL` 关 | **1.85 MiB**（余 77%） |
| `binfont` | 内嵌位图字库（完整版）编进固件，完全不用 FreeType | `UI_FONT_TTF` 关、`LV_USE_FREETYPE` 关、`UI_SYSTEM_FONT_FULL=y` | **3.84 MiB**（余 52%） |

- **切换**：`idfc build -o font=binfont`（临时）/ `idfc set-overlay font binfont`（永久）；
  清除回默认 `idfc set-overlay font --default`。查看 `idfc matrix` / `idfc status`。
- ⚠ 加了维度后 **`OutputDir` 必须包含全部维度占位符**（§2.5 的硬规则），所以旧的
  `build/cs43131.debug`、`build/pcm5102.release` 等目录**不再被使用**，各 cell 会重新全量构建一次
  （binfont 首次约 167 秒，其余约 75 秒）。旧目录可手工删除。
- ⚠ binfont 下 `font` 分区是**闲置**的（分区表共享、不随维度变化）。如果以后想让两个维度用不同
  分区表，可以在 overlay 里覆盖 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`（当前没做）。
- ⚠ binfont 的字库 `.rodata` 会被 XIP 拷进 PSRAM，**可用 PSRAM 由 6.40MB 降到 4.35MB**——
  这是**内嵌字库方案的固有特性**（TTF 迁移前的老方案同样如此，属已知行为），不是引入本维度才有的问题。
- 详细说明见 `FontSystem.md` 与 `BUILD_CONFIG.md`。

> 约定（**idfc 不强制**，但本项目遵守）：overlay 配置文件命名 `sdkconfig.<维度名>.<类型名>`，
> 见 `sdkconfig.dac_chip.cs43131` / `sdkconfig.dac_chip.pcm5102` / `sdkconfig.font.ttffont` / `sdkconfig.font.binfont`。

---

## 2. 配置文件格式：`idfc_buildconfig.json`

### 2.1 位置与发现规则

- 固定在**项目根目录**（与 `sdkconfig.defaults`、`.idfc/` 同级），文件名固定 **`idfc_buildconfig.json`**（常量应定义在代码里，见 §4.1）。
- **文件不存在 = 使用"隐式矩阵"**（完全等于今天的旧行为，见 §2.8），因此对既有项目零破坏。
- 文件存在但**解析/校验失败 = 报错退出（退出码 1）**，不静默回退到旧行为（否则会拿错目录构建）。
- 该文件**应纳入版本库**（它是配置的组成部分），与 `.idfc/idfc_config.json` 相反。

### 2.2 顶层字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `SchemaVersion` | int | 否 | `1` | 格式版本。大于 idfc 支持的版本时报错退出（提示升级 idfc） |
| `ProjectName` | string | 否 | 目录名 | 仅用于 `matrix` / `status` 显示，不参与路径 |
| `BaseConfig` | string | 否 | `sdkconfig.defaults` | 公共配置文件（相对项目根目录），必须存在 |
| `OutputDir` | string | **是** | — | 构建目录模板，见 §2.5 |
| `BuildVariants` | array | **是** | — | 构建类型维度，见 §2.3；至少 1 项 |
| `DefaultBuildVariant` | string | 否 | `BuildVariants[0].Name` | 未指定 `-t` 且 `.idfc` 无记录时使用；必须是 `BuildVariants` 里的 Name |
| `Overlays` | array | 否 | `[]` | overlay 维度，见 §2.4；**顺序即配置文件合并顺序** |

> **未知字段一律忽略**（Newtonsoft 的默认行为即可），便于新版本字段与旧版本 idfc 共存；
> 但**类型不匹配**、**缺少必填字段**、**引用了不存在的名称**都要报错。
> 建议文件用 UTF-8（本项目的文件是 UTF-8 无 BOM，含中文 Description）。

### 2.3 `BuildVariants[]`

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `Name` | string | **是** | 构建类型名，`^[A-Za-z0-9_]+$`（沿用现有 `checkBuildVariantName()`）；同一数组内唯一 |
| `ConfigFile` | string \| null | 否 | 该类型的差异配置文件（相对项目根目录）。**null/缺失 = 该维度不追加文件**（仅用 BaseConfig）。当前项目里三套都填了 `sdkconfig.defaults.<Name>` |
| `Description` | string | 否 | 人读说明，`matrix`/`help` 可展示 |

### 2.4 `Overlays[]` 与 `OverlayTypes[]`

| `Overlays[]` 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `OverlayName` | string | **是** | 维度名，`^[A-Za-z0-9_]+$`，全局唯一；**它就是 `OutputDir` 里的占位符名**（`{dac_chip}`） |
| `DefaultType` | string | **是** | 该维度的默认类型，必须是 `OverlayTypes[].Name` 之一（强制"每格恰好一个类型"） |
| `OverlayTypes` | array | **是** | 至少 1 项，见下表 |

| `OverlayTypes[]` 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `Name` | string | **是** | 类型名，`^[A-Za-z0-9_]+$`，同一维度内唯一 |
| `ConfigFile` | string \| null | 否 | 该类型的配置文件（相对项目根目录）。**null/缺失 = 该类型不追加文件**（"只选型、不改配置"的维度，如把芯片型号当作命名维度） |
| `Description` | string | 否 | 人读说明 |

### 2.5 `OutputDir` 模板语法

- 由**字面文本 + 占位符**组成，占位符写作 `{名字}`，名字只能取：
  - `{variant}` —— 选中的 `BuildVariants[].Name`；
  - `{<OverlayName>}` —— 该维度选中的 `OverlayTypes[].Name`，**每个 overlay 维度都必须出现**。
- 解析规则：**必须包含 `{variant}`，且必须包含全部 `OverlayName`**。否则不同单元格会实例化到同一个目录（互相覆盖产物，且过期检测失效）——这是必须在加载期拒绝的**硬错误**。
- 出现未知占位符 → 报错。
- 路径必须是**项目内的相对路径**，不允许绝对路径、盘符、`..`；且**建议限制在 `build/` 之下**（现有 `IsBuildFileOutdated()` 靠 `Path.Combine(projectDir,"build")` 前缀把产物目录排除在"相关文件"之外，跑出 `build/` 会让过期检测把产物当配置扫）。**实现时建议强制要求以 `build/` 开头**。
- 分隔符统一写 `/`（Windows 下实例化时替换为 `Path.DirectorySeparatorChar`），与现有代码 `GetBuildDirName()` 的风格一致。
- 允许带子目录，例如 `build/{variant}/{dac_chip}`（此时 cell 名取最后一段 `pcm5102`，可能与别的 cell 重名 → **建议**要求实例化后的目录名唯一，重名直接报错）。

### 2.6 完整示例（本仓库当前内容，可直接作为 schema 的参考实现）

```json
{
	"ProjectName": "FakePodNanoNightPlayer",
	"BaseConfig": "sdkconfig.defaults",
	"OutputDir": "build/{dac_chip}.{variant}",
	"BuildVariants": [
		{ "Name": "debug", "ConfigFile": "sdkconfig.defaults.debug", "Description": "…" },
		{ "Name": "releasecandidate", "ConfigFile": "sdkconfig.defaults.releasecandidate", "Description": "…" },
		{ "Name": "release", "ConfigFile": "sdkconfig.defaults.release", "Description": "…" }
	],
	"DefaultBuildVariant": "debug",
	"Overlays": [
		{
			"OverlayName": "dac_chip",
			"DefaultType": "cs43131",
			"OverlayTypes": [
				{ "Name": "cs43131", "ConfigFile": "sdkconfig.dac_chip.cs43131", "Description": "…" },
				{ "Name": "pcm5102", "ConfigFile": "sdkconfig.dac_chip.pcm5102", "Description": "…" }
			]
		}
	]
}
```

> ⚠ 实际文件里的 `Description` 较长。**尾随逗号**（如 `{ … },`）在 idfc 当前使用的 `Newtonsoft.Json` 下**能解析成功**（实测：留了尾随逗号不会报错，问题只会在后续字段校验时才暴露），但换 `System.Text.Json` 或严格解析器就会直接失败——**所以不要写**。真正的语法错误（缺括号等）会给出带定位的报错：
> `> 错误：配置矩阵文件解析失败：<路径>` + `Unexpected end when deserializing object. Path 'BuildVariants', line 3, position 0.`
> 解析继续用 `Newtonsoft.Json`（`ConfigStore` 已经依赖它），并保持"打印文件路径 + 出错位置"的行为。

### 2.7 加载期校验规则（全部应在 `build` 之前拦截）

| # | 校验 | 不通过时的处理 |
|---|---|---|
| 1 | 文件是合法 JSON，字段类型正确 | 报错（含路径与 JsonReader 的位置信息），退出码 1 |
| 2 | `SchemaVersion` ≤ 支持版本 | 报错："配置矩阵格式版本 X 高于本工具支持的 Y，请更新 idfc" |
| 3 | `BaseConfig` 文件存在 | 报错 |
| 4 | `BuildVariants` 非空、`Name` 合法且唯一 | 报错 |
| 5 | 每个 `ConfigFile`（非 null）存在 | 报错，列出缺失文件 |
| 6 | `DefaultBuildVariant` 在 `BuildVariants` 中 | 报错 |
| 7 | `OverlayName` / 类型 `Name` 合法且唯一；`OverlayTypes` 非空 | 报错 |
| 8 | 每个维度的 `DefaultType` 在其 `OverlayTypes` 中 | 报错 |
| 9 | `OutputDir` 含 `{variant}` 与**全部** overlay 占位符、无未知占位符 | 报错（防撞目录） |
| 10 | `OutputDir` 实例化后：相对路径、不含 `..`、在 `build/` 下、目录名唯一 | 报错 |
| 11 | `.idfc` 里 `ActiveOverlays` 的维度名/类型名在当前矩阵中有效 | **报错**（不静默回退到 `DefaultType`），提示用 `set-overlay` 修正或删除该字段，并列出可用值 |
| 12 | 命令行的 `-o` 维度名/类型名有效、同一维度不重复 | 报错，列出该维度的可用类型 |

> 第 11 条是刻意的：`.idfc` 是**隐式状态**，静默回退会让"我明明选了 pcm5102 却烧了 cs43131"这种事故重新出现。宁可让用户/Agent 看到一条明确的错误。

### 2.8 隐式矩阵（文件不存在时的回退）

`idfc_buildconfig.json` 不存在时，用下面这个**内置定义**，行为与今天**逐字节一致**（老项目/老文档不需要任何改动）：

```jsonc
{
  "SchemaVersion": 1,
  "BaseConfig": "sdkconfig.defaults",
  "OutputDir": "build/{variant}",
  "BuildVariants": [ /* 由 sdkconfig.defaults.* 动态发现，或直接沿用 -t 指定的名字 */ ],
  "DefaultBuildVariant": "debug",
  "Overlays": []
}
```

具体做法有两个选择（推荐 A）：

- **A（推荐，改动最小）**：不构造隐式 JSON，而是让解析入口返回 `null`，各调用点走"无矩阵分支"——即保留现有 `GetProjectDirSDKConfigNameByVariant()` / `GetBuildDirName(variant)` 等方法的**旧签名**供回退路径使用。这样老行为一行没变，风险最低。
- **B**：动态合成一个 `BuildMatrixDefinition` 对象（overlay 维度为空），全代码只有一条路径。语义更统一，但要保证 `OutputDir` 实例化结果与 `build/<variant>` 完全一致（含分隔符），并让 `-o` 在无矩阵时报错。

无论选哪种，**都要保证**：无矩阵时 `-t <v>` 仍在 `sdkconfig.defaults.<v>` 缺失时报"配置文件尚不存在"（现有 `BuildConfigValidator.CheckBuildVariantOrThrow` 的行为）。

---

## 3. 命令行与配置状态

### 3.1 参数

| 参数 | 作用 | 备注 |
|---|---|---|
| `-t <variant>` | 指定构建类型（主维度） | **保持不变**，语义、校验、`.idfc` 回退都不变 |
| `-o <维度>=<类型>` | 指定某个 overlay 维度的类型，**可重复**（每个维度最多一次） | 新增。例：`-o dac_chip=pcm5102` |
| `-p COMx` | 串口 | 不变 |

- `-o` 的取值必须是 `<OverlayName>=<TypeName>`，缺 `=`、维度未知、类型未知、同一维度重复 → 报错退出 1，并列出该维度的可用类型。
- 命令行 `-o` 与 `.idfc` 的 `ActiveOverlays` 合并，**命令行优先**；未指定的维度取 `DefaultType`。
- 无矩阵（`idfc_buildconfig.json` 不存在）时使用 `-o` → 报错："当前项目未定义配置矩阵（缺少 `idfc_buildconfig.json`）"。
- `-o` 适用于所有会用到构建目录/配置链的命令：`build` / `flash` / `monitor` / `size` / `run` / `clean` / `fullclean` / `menuconfig`；`status` 与 `matrix` 用于查看。

> 可选的便利增强（P2，非必须）：只有一个 overlay 维度时允许 `-o <类型>` 的简写，自动套到该维度上；实现与否都不影响格式定义。

### 3.2 新命令

| 命令 | 行为 |
|---|---|
| `set-overlay <维度> <类型>` | 把该维度的选择写入 `.idfc/idfc_config.json` 的 `ActiveOverlays`（维度/类型不存在时报错并列出可用值） |
| `set-overlay <维度> --default` | 删除该维度的记录（回到 `DefaultType`） |
| `matrix` | 列出矩阵的**全部单元格**：cell 名、构建目录、sdkconfig 链、各维度类型、是否已构建（`<构建目录>/sdkconfig` 是否存在）/是否过期，并用 `*` 标出当前选择。无矩阵文件时打印"当前项目未定义配置矩阵（隐式行为：build/<variant>）"并退出 0 |
| `matrix --json`（P2） | 以 JSON 输出全部单元格，便于脚本/Agent 消费 |

### 3.3 既有命令的可见变化

- **回显行**：`# Executing \`idfc idfpy -B build\cs43131.debug -D SDKCONFIG_DEFAULTS="…" -D SDKCONFIG="…" build\``（前缀来自 cell，格式不变）。
- **过期警告**（`flash`）：文案带 cell 名，例：
  `> 警告：当前构建目标 cs43131.debug 已经过时。当前即将刷入的是旧版的固件。建议重新进行构建，会自动更新配置。`
- **`status`**：在"当前构建类型"之后补一行（有矩阵时）：
  ```
  - **当前构建目标：** cs43131.debug （dac_chip=cs43131）
      - 构建目录：`build/cs43131.debug`
      - 配置链：`sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.cs43131`
  ```
- **`clean` / `fullclean`**：只作用于**当前 cell 的目录**（`DoClean` 删 `<cell>/sdkconfig` 与 `<cell>/config`；`DoFullClean` 删整个 `<cell>/`），与今天的语义一致，只是路径变成 cell 的。
- **`menuconfig`**：读 `<cell>/config/` 下的两个 json；提示语里的 `build/<构建类型>/config/` 改为实际 cell 目录。
- **`idfpy`**：**保持原样透传**（逃生舱不参与矩阵解析）；文档里提示"手工拼参数时用 `matrix`/`build --dry-run` 抄完整命令行"。

### 3.4 退出码

沿用现有约定（`0` 成功 / `1` 参数与校验失败 / `2` 权限或 `idf.py` 透传 / `-1` 服务器未运行）。新增的失败情形（矩阵文件非法、`-o` 非法、`.idfc` 里的选择失效、`OutputDir` 占位符缺失）**全部用 1**。

---

## 4. idfc 代码修改说明

> 以下路径均相对 `Q:\Projects\CSharp\idf-client-cli\IDFClientCLI\`。行号为基线 commit `85a73ab` 的行号，仅供定位。

### 4.0 影响面清单（现有调用点，改造前先看这张表）

| 现有成员 | 调用点（文件:行） | 矩阵化后的去向 |
|---|---|---|
| `BuildVariantHelper.GetBuildDirName` | `SizeCommand.cs:47,90`、`MenuconfigCommand.cs:130,186,198,354`、`BuildVariantHelper.cs:43,161,176` | 改为 `cell.BuildDir`（相对路径字符串） |
| `BuildVariantHelper.GetCommonIDFArgsPrefix` | `BuildCommand.cs:111,175`、`FlashCommand.cs:47`、`MonitorCommand.cs:64`、`SizeCommand.cs:79` | 改为 `cell.GetCommonIDFArgsPrefix()` |
| `BuildConfigValidator.IsBuildFileOutdated` | `BuildCommand.cs:104`、`FlashCommand.cs:42` | 改签名，加 cell（内含 overlay 配置 + 矩阵文件的时间戳） |
| `BuildConfigValidator.CheckBuildVariantOrThrow` | `BuildCommand.cs:74`、`FlashCommand.cs:40`、`MonitorCommand.cs:62`、`SizeCommand.cs:45`、`CleanCommands.cs:26,45`、`PressF5RunCommand.cs:63` | 改为 `CheckCellOrThrow(matrix, selection)` |
| `BuildConfigCleaner.DoClean` / `DoFullClean` | `BuildCommand.cs:157,161`、`CleanCommands.cs:28,46` | 改签名接收 cell |
| `CommandArgsExtension.PopBuildVariant` | `BuildCommand.cs:38`、`FlashCommand.cs:29`、`MonitorCommand.cs:41`、`SizeCommand.cs:34`、`CleanCommands.cs:24,43`、`PressF5RunCommand.cs:43`、`MenuconfigCommand.cs:286` | 保留；新增 `PopOverlays` 并在各命令里合成 cell |
| `BuildVariantHelper.GetProjectDirSDKConfigNameByVariant` | `BuildVariantHelper.cs:88,128`、`ConfigCommand.cs:89,99` | 保留（无矩阵回退 + `set-variant` 校验用） |

**结论**：**每个命令的 `Main` 都需要改一行**（把 `string buildVariant` 换成解析好的 cell），凡是把 `buildVariant` 往下传的地方（`ExecBuild` / `ExecFlash` / `ExecMonitor` / `ExecSize` / `DoClean` / `DoFullClean`）签名都要改。`Server/*` 与 `Core/ArgExtensions.cs` **不需要改**（`PopValue` 反复调用即可取出重复的 `-o`；`RawIdfCommand` 是纯透传）。

### 4.1 新增文件

**① `Client/BuildHelper/BuildMatrix.cs`**（模型 + 加载 + 校验 + 单元格解析；**记得加进 `IDFClientCLI.csproj` 的 `<Compile Include>`**）

建议的最小公共面：

```csharp
namespace IDFClientCLI.Client.BuildHelper
{
    /// <summary>矩阵定义文件（idfc_buildconfig.json）的模型与加载。文件缺失时 Load 返回 null（走隐式矩阵）。</summary>
    public class BuildMatrixDefinition
    {
        public const string FileName = "idfc_buildconfig.json";
        public const int SupportedSchemaVersion = 1;

        public int SchemaVersion { get; set; } = 1;
        public string ProjectName { get; set; }
        public string BaseConfig { get; set; } = "sdkconfig.defaults";
        public string OutputDir { get; set; }
        public List<BuildVariantDefinition> BuildVariants { get; set; }
        public string DefaultBuildVariant { get; set; }
        public List<OverlayDefinition> Overlays { get; set; } = new List<OverlayDefinition>();

        /// <summary>文件不存在→null；存在但非法→打印错误并 throw ExitWithCodeException(1)。</summary>
        public static BuildMatrixDefinition LoadOrNull(string projectDir);
        public void Validate(string projectDir);              // §2.7 的规则 2~10
        public OverlayDefinition FindOverlay(string name);    // 找不到返回 null
    }

    public class BuildVariantDefinition { public string Name, ConfigFile, Description; }
    public class OverlayDefinition
    {
        public string OverlayName, DefaultType, Description;
        public List<OverlayTypeDefinition> OverlayTypes;
        public OverlayTypeDefinition FindType(string name);
    }
    public class OverlayTypeDefinition { public string Name, ConfigFile, Description; }

    /// <summary>一格构建：variant + 每个维度一个类型，已解析成路径与配置链。</summary>
    public class BuildCell
    {
        public string CellName;                 // 例：cs43131.debug
        public string BuildDir;                 // 相对：build/cs43131.debug
        public string SDKConfigPath;            // 相对：build/cs43131.debug/sdkconfig
        public List<string> ConfigFiles;        // 相对：sdkconfig.defaults, sdkconfig.defaults.debug, sdkconfig.dac_chip.cs43131
        public string VariantName;
        public Dictionary<string, string> OverlayTypes;   // 维度 → 类型

        /// <summary>命令行前缀：-B … -D SDKCONFIG_DEFAULTS="a;b;c" -D SDKCONFIG="…" </summary>
        public string GetCommonIDFArgsPrefix();
        /// <summary>无矩阵时的旧行为（build/&lt;variant&gt; + 两份 defaults）。</summary>
        public static BuildCell Legacy(string variantName);
    }

    /// <summary>把（矩阵, variant 名, 维度→类型 的选择）解析成 BuildCell；校验选择是否合法。</summary>
    public static class BuildCellResolver
    {
        public static BuildCell Resolve(BuildMatrixDefinition matrix, string variantName,
                                        IDictionary<string,string> requestedOverlays);
    }
}
```

要点：

- **配置链拼接用 `";"` 硬编码**，不要用 `Path.PathSeparator` 表达语义（虽然 Windows 上都是 `;`，但这里表达的是"列表分隔符"，现有代码用 `Path.PathSeparator` 只是巧合可用；保持输出与今天一致即可）。
- `ConfigFiles` 里**跳过 null/空**的 `ConfigFile`（§2.3/§2.4 允许"不追加文件"）。
- `OutputDir` 实例化：按 `{...}` 切分替换，替换值来自 `VariantName` / `OverlayTypes`；替换结果用 `Path.DirectorySeparatorChar` 规整分隔符。
- `Legacy(variant)` 用于 §2.8 的回退路径，**不要**在矩阵存在时混用。

**② `Client/CustomCommands/MatrixCommand.cs`**（`matrix` 命令；**同样要加进 `.csproj`**）

- `TriggerArg => "matrix"`，`HelpMessage` 说明用法（`help --all` 会自动收录）。
- 无矩阵文件：打印 `> 当前项目未定义配置矩阵（隐式行为：build/<variant>）` 并返回 0。
- 有矩阵：笛卡尔积展开，逐行打印 §3.2 的表格；当前 cell 用 `*` 标注；`已构建` 判定 = `<cell>/sdkconfig` 存在；`过期` 判定复用 `IsBuildFileOutdated`。
- 建议实现 `--json`（P2）：`{ "cells": [ { "cell": "...", "buildDir": "...", "sdkconfig": [...], "overlays": {...}, "active": true, "built": false, "outdated": true } ] }`。

> 也可以把 `matrix` 拆到 `Client/CustomCommands/` 之外，但**必须**遵守 README「添加一个新命令」的三步：继承 `IClientCommandHandler` → 加进 `.csproj` 的 `<Compile Include>` → 反射自动注册。

### 4.2 修改文件

**① `Client/BuildHelper/BuildVariantHelper.cs`（核心，改动最大）**

- 顶部新增 `public const string MatrixConfigFileName = "idfc_buildconfig.json";`（或引用 `BuildMatrixDefinition.FileName`）。
- `GetCommonIDFArgsPrefix(string buildVariant)`（45~50 行）→ 新增重载 `GetCommonIDFArgsPrefix(BuildCell cell)`（或直接把它挪进 `BuildCell`）。**建议把三行拼装整段搬进 `BuildCell.GetCommonIDFArgsPrefix()`**，让 `BuildVariantHelper` 只保留旧签名（供 `Legacy` 回退）：
  1. `-B {cell.BuildDir} `
  2. `-D SDKCONFIG_DEFAULTS="{string.Join(";", cell.ConfigFiles)}" `
  3. `-D SDKCONFIG="{cell.SDKConfigPath}" `
- `GetBuildDirName` / `GetBuildDirSDKConfigName`（37、43 行）：保留旧签名（`Legacy` 用）；新增 `BuildCell` 上的等价属性。
- `CheckBuildVariantExtenstion.checkBuildVariantName`（54~67 行）：**原样保留**，overlay 维度名与类型名复用同一个正则；如需更明确的错误文案，再包一层 `checkOverlayName()` / `checkOverlayTypeName()`。
- `BuildConfigValidator.HasVariantConfig` / `CheckBuildVariantOrThrow`（86~99 行）：
  - 保留 `HasVariantConfig`（无矩阵回退 + `set-variant`）；
  - 新增 `CheckCellOrThrow(string projectDir, BuildCell cell)`：校验 `BaseConfig`、`cell.ConfigFiles` 全部存在（`File.Exists`），并把**实例化后的路径**打印进错误信息。
  - 建议在 `build` 里**先校验矩阵、再校验 cell**，错误信息一次给全。
- `BuildConfigValidator.IsBuildFileOutdated`（101~145 行）→ 改为 `IsBuildFileOutdated(string projectDir, BuildCell cell)`，**必须新增登记的相关文件**：
  ```csharp
  regWorkDirPath(BuildVariantHelper.MatrixConfigFileName);  // ← 新增：改矩阵定义（换 OutputDir/类型）也要重配
  cell.ConfigFiles.ForEach(regWorkDirPath);                 // ← 新增：BaseConfig + variant + 各 overlay 配置
  ```
  （`Kconfig` / `*.projbuild` 的递归登记与 `build/` 目录排除逻辑保持不变。）
  **这是整个改造里最容易漏、后果最严重的一处**：漏登记 overlay 配置 → "改了 `sdkconfig.dac_chip.pcm5102` 但增量构建不生效也不报错"，正是 `IDFC_USAGE.md` §6.2 那类历史事故的翻版。
  - 顺带（可选，建议做）：第 117~122 行的 `path.StartsWith(buildDirFullPath)` 在 `buildX` 与 `build` 并存时会误判，改成 `StringComparison.OrdinalIgnoreCase` 并补目录分隔符边界（`path.StartsWith(buildDirFullPath + Path.DirectorySeparatorChar, ...)`）。
- `BuildConfigCleaner.DoClean` / `DoFullClean`（148~188 行）→ 签名改成接收 `BuildCell`（内部用 `cell.SDKConfigPath` / `cell.BuildDir`），**保留旧重载**给内部回退路径用，或直接全量替换调用点。

**② `Client/CommandArgsExtension.cs`**

新增：

```csharp
public const string OverlayFlag = "-o";
public static string OverlayFlagHelp =>
    $"- `{OverlayFlag} <维度>=<类型>`: 指定配置矩阵中某个维度的类型（可重复，每个维度一次），例 `{OverlayFlag} dac_chip=pcm5102`；未指定的维度用矩阵里的 `DefaultType`。";

/// <summary>取出所有 -o（可重复），与 .idfc 的 ActiveOverlays 合并（命令行优先）。</summary>
public static Dictionary<string,string> PopOverlays(this string[] args, string cwd)
{
    var result = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
    var stored = ConfigStore.ConfigStore.LoadConfig(cwd)?.ActiveOverlays;   // 可能为 null
    if (stored != null) foreach (var kv in stored) result[kv.Key] = kv.Value;
    while (args.PopValue(OverlayFlag, out string pair))                      // PopValue 反复调用可取多个
    {
        // 解析 name=type；缺 = / 空值 → 报错；同维度重复 → 报错；覆盖 stored 里的同维度值
    }
    return result;
}
```

（`Core/ArgExtensions.cs` **不用改**：`PopValue` 会把用过的参数置 `null`，循环调用即可取出全部 `-o`。）

**③ `Client/ConfigStore/ConfigStore.cs`**

- `ConfigStoreDataModel`（76~86 行）新增：
  ```csharp
  [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
  public Dictionary<string,string> ActiveOverlays { get; set; }
  ```
  用 `NullValueHandling.Ignore`，这样**没有 overlay 的项目写出来的 json 与今天完全一致**（不会凭空多一个字段）。
- `InitConfig`（23~42 行）不必显式写 `ActiveOverlays`（null 即"全用默认类型"）。

**④ `Client/ConfigStore/ConfigCommand.cs`**

- `SetBuildVariantCommand`（75~103 行）：校验改为**矩阵优先**——
  矩阵存在 → 要求 `variantName` 出现在 `BuildVariants`（否则列出可用类型）；
  矩阵不存在 → 保留现有的 `sdkconfig.defaults.<v>` 文件存在性检查（第 89~101 行）。
- 新增 `SetOverlayCommand`（放在本文件即可，csproj 已包含）：
  - `TriggerArg => "set-overlay"`；用法 `set-overlay <维度> <类型>` / `set-overlay <维度> --default`。
  - 读取矩阵 → 校验维度与类型存在 → 读 `.idfc` → 增删 `ActiveOverlays` → `SaveConfig` → 打印结果（含解析后的 cell 名，例如 `> 已选择 dac_chip=pcm5102，当前构建目标： pcm5102.debug`）。
  - 无矩阵文件时报错："当前项目未定义配置矩阵（缺少 idfc_buildconfig.json）"。

**⑤ 命令层（逐个文件，模式一致）**

```csharp
// 改造前
string buildVariant = args.PopBuildVariant(cwd);
BuildConfigValidator.CheckBuildVariantOrThrow(cwd, buildVariant);
cmdLine.Append(BuildVariantHelper.GetCommonIDFArgsPrefix(buildVariant));

// 改造后
var matrix   = BuildMatrixDefinition.LoadOrNull(cwd);
var variant  = args.PopBuildVariant(cwd);
var overlays = matrix == null ? null : args.PopOverlays(cwd);   // 无矩阵时不允许 -o
var cell     = matrix == null ? BuildCell.Legacy(variant)
                              : BuildCellResolver.Resolve(matrix, variant, overlays);
BuildConfigValidator.CheckCellOrThrow(cwd, cell);
cmdLine.Append(cell.GetCommonIDFArgsPrefix());
```

- `IDFCommands/BuildCommand.cs`：`Main`（35~59 行）解析出 cell；`ExecBuild`（70~200 行）签名改为 `ExecBuild(BuildCell cell, bool verbose, ReconfigureType, bool dryRun)`（**或保留 `string buildVariant` 重载**供 `Legacy` 路径）；`--dry-run`（114~146 行）文案里补上 cell 名、构建目录、完整配置链；`DoClean/DoFullClean`（157~161 行）传 cell。
- `IDFCommands/FlashCommand.cs`：`Main` 解析 cell；`ExecFlash`（36~53 行）签名改 cell；过期警告（44 行）文案带 cell 名。
- `IDFCommands/MonitorCommand.cs`：`Main` 解析 cell；`ExecMonitor`（57~106 行）签名改 cell。
- `IDFCommands/SizeCommand.cs`：`Main` 解析 cell；`ExecSize`（41~111 行）签名改 cell；`buildOutputDir`（47 行）与报告输出路径（90 行）用 `cell.BuildDir`。
- `IDFCommands/PressF5RunCommand.cs`：`Main`（40~63 行）解析一次 cell，然后把 cell 依次传给 `BuildCommand.ExecBuild` / `SizeCommand.ExecSize` / `FlashCommand.ExecFlash` / `MonitorCommand.ExecMonitor`（现在传的是 `buildVariant` 字符串）。
- `IDFCommands/CleanCommands.cs`：两个命令都解析 cell；`-o` 也适用于它们（只清当前 cell）。
- `CustomCommands/MenuconfigCommand.cs`：`PopBuildVariant`（286 行）之后解析 cell；`GetBuildDirName(buildVariant)` 的 4 处（130、186、198、354 行）改为 `cell.BuildDir`；提示文案里的 `build/<构建类型>/config/`（`MenuconfigCommand.cs:42,93`、`Core/Kconfig/KconfigMenus.cs:7,174`、`Core/Kconfig/KconfigValues.cs:71,280`）改成实际 cell 目录（**纯文案**，可统一写成 `build/<cell>/config/`）。
- `CustomCommands/StatusCommand.cs`：在 `ActiveBuildVariant`（51~54 行）之后，读取矩阵与 `ActiveOverlays`，解析 cell 并打印 §3.3 的三行；矩阵非法时打印错误但**仍返回 0**（`status` 是诊断命令，不应因为配置坏了就无法用）。

**⑥ `Client/HelpCommand.cs` / 各命令的 `HelpMessage`**

无需改框架（`help --all` 自动收录），但要**逐个更新 HelpMessage**：`build`/`flash`/`monitor`/`size`/`run`/`clean`/`fullclean`/`menuconfig`/`status` 加入 `CommandArgsExtension.OverlayFlagHelp` 一行；`set-overlay` 与 `matrix` 是新增命令，自带 HelpMessage。示例仓库里 `IDFC_USAGE.md` 引用的 `idfc help --all` 输出会随之变化。

**⑦ 文档（idfc 仓库 README.md）**

| 位置 | 需要改什么 |
|---|---|
| `核心特性` 表（11~21 行） | "多配置构建封装"一行扩成"多配置/配置矩阵封装"：`-B build/<cell>` + 多条 `SDKCONFIG_DEFAULTS` |
| `命令参考 → 总览`（102~122 行） | 新增 `matrix`、`set-overlay`；`clean/fullclean` 说明改为"当前 cell 的目录"；通用参数补充 `-o` |
| `项目配置`（128~150 行） | `.idfc/idfc_config.json` 增加 `ActiveOverlays` 字段行；"多配置约定"扩展为 §1.3 的四件事，并说明 `idfc_buildconfig.json` 的发现规则与"缺失=旧行为" |
| `常见问题`（166~175 行） | 新增：`-o` 报维度/类型不存在；`.idfc` 里的选择失效；改了 overlay 配置不生效（过期检测）；`menuconfig` 需要该 cell 构建过 |
| `目录结构`（267~290 行） | `BuildHelper/` 补充 `BuildMatrix.cs`；`CustomCommands/` 补充 `matrix` |
| 新增章节（建议放 `项目配置` 之后） | `## 配置矩阵（idfc_buildconfig.json）`：字段表 + OutputDir 模板 + 校验规则 + 一个完整示例（可直接搬本文档 §2） |

---

## 5. 兼容性、失败模式与迁移

### 5.1 兼容性

| 场景 | 行为 |
|---|---|
| 项目没有 `idfc_buildconfig.json` | **完全等于今天**：`build/<variant>`、`sdkconfig.defaults;sdkconfig.defaults.<variant>`、`-o` 报错（未定义矩阵） |
| 有矩阵但只用 `-t` | 其余维度用 `DefaultType`，落到 `build/<默认类型>.<variant>` |
| 有矩阵 + `-o` + `-t` | 命令行优先 → 唯一确定一格 |
| `.idfc` 里存了失效的选择 | 报错（§2.7 规则 11），**不静默回退** |
| `idfpy` 逃生舱 | 不受影响，仍然原样透传 |

### 5.2 迁移注意（本项目这种"从一维升到二维"的项目）

1. **构建目录会改名**：`build/debug` → `build/cs43131.debug`。旧目录不会被自动清理或复用，**首次切换后会全量重新构建**（体积与耗时见 `BUILD_CONFIG.md` §1：debug 约 1.1MB / releasecandidate、release 约 4.2MB）。建议在 `build` 里检测到"旧式 `build/<variant>` 目录存在且新 cell 目录不存在"时，打印**一次性提示**（提示内容：旧产物已不再使用，可手工删除或 `idfc fullclean -t <variant>` 已不再覆盖它），**不要自动删**。
2. **过期检测覆盖面变宽**：新增 `idfc_buildconfig.json` 与所有 overlay 配置的时间戳比较（§4.2 ①）。否则"改了 `sdkconfig.dac_chip.pcm5102` → 增量构建"会烧出旧配置的固件。
3. **`clean` 语义**：只清当前 cell。想清一整套矩阵的所有 cell，用 `matrix` 列出后逐个 `fullclean -t <v> -o <dim>=<type>`（P2 可考虑 `fullclean --all-cells`，非必须）。
4. **固件侧的兜底仍在**：本项目 `main/Kconfig.projbuild` 的两个 DAC 项 + `main.c` 末尾的 `#if/#error`，会在"矩阵选型没合并进去"时让编译失败。idfc 侧不要把这条兜底当成"反正有编译错误"，校验该早拦就早拦。
5. **`AGENTS.md`/`IDFC_USAGE.md` 的联动**：本项目 `AGENTS.md` §4 的"执行回显"示例、`IDFC_USAGE.md` 的迁移对照表与 §6.1 都写死了 `-B build/<cfg>` 的形式。矩阵落地后，这些示例应改为 cell 形式（或明确注明"未定义矩阵时仍是 `build/<cfg>`"），否则会重新制造"照抄旧文档"的事故。

### 5.3 明确不做的事（边界）

- 不做"多维度的组合别名"（例如给 `cs43131.release` 起个名字 `prod`）——cell 名就是目录名，够用。
- 不做 overlay 维度的"多选"（每维度单选；需要多选就拆成多个维度）。
- 不做矩阵的"部分构建/批量构建"命令（例如 `build --all-cells`）；先让单格路径稳定，批量构建是后续独立特性。
- 不改 `idfpy` 的透传语义（矩阵解析只发生在高层命令里）。

---

## 6. 实施顺序与自测清单

### 6.1 建议的实施顺序（每步都可独立验证）

| 步骤 | 内容 | 验证方式 |
|---|---|---|
| 1 | 新增 `BuildMatrix.cs`（模型 + 加载 + 校验 + `BuildCell`）；实现 `matrix` 命令（纯客户端，不碰 idf.py） | 在 FakePodNanoNightPlayer 里 `idfc matrix` 应列出 6 格；故意改坏 json / 删占位符 / 删 ConfigFile，应分别报错 |
| 2 | `ConfigStore.activeOverlays` + `set-overlay` + `status` 展示 | `idfc set-overlay dac_chip pcm5102` 后 `idfc status` 显示 `pcm5102.debug` |
| 3 | `CommandArgsExtension.PopOverlays` + 各命令 `Main` 解析 cell（先只替换参数拼装，保留旧回退） | `idfc build --dry-run`、`idfc build -t release -o dac_chip=pcm5102 --dry-run` 打印的 `-B`/`-D` 与 §1.3 一致 |
| 4 | `IsBuildFileOutdated` 登记矩阵文件 + overlay 配置；`DoClean/DoFullClean` 接收 cell | 构建一次 → `touch sdkconfig.dac_chip.pcm5102` → `idfc build` 应报"检测到构建文件已更新，清理并重新配置" |
| 5 | `menuconfig` / `size` / `run` / `flash` 全部切到 cell；过期警告文案带 cell 名 | `idfc size`、`idfc menuconfig desc CONFIG_AUDIO_DAC_PCM5102`（先 build 过该 cell） |
| 6 | 帮助文案 + README 更新 | `idfc help --all` 输出里能看到 `-o`、`set-overlay`、`matrix` |

### 6.2 自测清单（建议直接做成手工验收表）

| # | 用例 | 期望 |
|---|---|---|
| 1 | 删除/重命名 `idfc_buildconfig.json`，`idfc build --dry-run` | 打印 `-B build\debug`、两段 `SDKCONFIG_DEFAULTS`（旧行为）；`-o` 用法报错 |
| 2 | `idfc matrix` | 6 格、当前格带 `*`、已构建/过期标记正确 |
| 3 | `idfc build -t debug -o dac_chip=pcm5102 --dry-run` | `-B build\pcm5102.debug`，配置链三份，顺序正确 |
| 4 | `idfc build -o dac_chip=cs43131 -o dac_chip=pcm5102` | 报错"维度 dac_chip 重复指定" |
| 5 | `idfc build -o dac_chip=xxx` | 报错并列出 `cs43131`/`pcm5102` |
| 6 | `idfc build -o pcm5102`（无 `=`） | 报错并给出正确用法 |
| 7 | `.idfc` 里写 `"ActiveOverlays": {"dac_chip":"xxx"}` | 任何命令都报错（不静默回退），提示 `set-overlay` |
| 8 | `OutputDir` 改为 `build/{variant}`（丢掉 overlay 占位符） | 加载期报错"OutputDir 必须包含全部维度的占位符" |
| 9 | `OutputDir` 改为 `../out/{dac_chip}.{variant}` | 报错（逃出项目/不在 `build/` 下） |
| 10 | `ConfigFile` 指向不存在的文件 | 报错并打印缺失文件路径 |
| 11 | `idfc clean` / `fullclean`（当前 cell） | 只影响 `build/<cell>`，其它 cell 目录原样 |
| 12 | 构建某 cell 后 `idfc menuconfig search "DAC"` | 能读到该 cell 的 `config/*.json`；换 cell（未构建过）→ 提示"先 build" |
| 13 | `idfc flash`（构建过期时） | 打印带 cell 名的过期警告，行为仍是"只警告不重建" |
| 14 | `idfc idfpy -B build\debug build`（绕过高层命令） | 不受矩阵影响（逃生舱），文档已提示风险 |

---

## 7. 实现验收记录（2026-10-02）

> 新版 `idfc.exe` 已按本文档实现并安装；本节是**实测记录**（"实现与文档是否一致"的证据），不是设计说明。
> idfc 侧改动状态：**未提交的工作区改动**，基线仍为 commit `85a73ab`；新增 `Client/BuildHelper/BuildMatrix.cs`、`Client/CustomCommands/MatrixCommand.cs`，修改 14 个文件（含 README，且新增文件已登记进 `.csproj`）——与本文档 §4 的文件清单完全一致。
> 测试环境：ESP32-S3 开发板接 **COM5**（USB-Serial/JTAG，VID_303A），IDF 服务器（新版）运行中，`.idfc` 默认 `debug`；隔离用例在 scratch 项目 `tempdir/matrix-test/` 中执行（测试后已删除），真实项目的构建产物为 `build/cs43131.debug`、`build/pcm5102.debug`。

### 7.1 通过项

**A. 矩阵解析与校验**（scratch 项目，逐条对照 §2.7）

| 用例 | 实测结果 |
|---|---|
| 无 `idfc_buildconfig.json`（隐式回退） | `matrix` → "当前项目未定义配置矩阵（隐式行为：build/<variant>）"，exit 0；`build --dry-run` 仍走 `build\debug` + 两段配置链；`-o` / `set-overlay` 均报错 exit 1 ✅ |
| `SchemaVersion: 99` | "高于本工具支持的 1 … 请更新 idfc"，exit 1 ✅ |
| 语法错误（缺右花括号） | `配置矩阵文件解析失败：<路径>` + `Unexpected end when deserializing object. Path 'BuildVariants', line 3, position 0.`，exit 1 ✅ |
| `BuildVariants` 为空 / 维度名重复 / `DefaultType` 不在类型表 | 各自报错，exit 1 ✅ |
| `ConfigFile` 指向不存在文件 | 报错并列出缺失文件的**绝对路径**，exit 1 ✅ |
| `OutputDir` 缺 overlay 占位符 | 报错（附"多个单元格会落到同一目录、互相覆盖产物"的理由），exit 1 ✅ |
| `OutputDir` 含未知占位符 `{foo}` | 报错并列出可用占位符，exit 1 ✅ |
| `OutputDir` 含 `..` / 不以 `build/` 开头 / 绝对路径 | 三种分别报错（`build/` 那条还写明了"过期检测会扫到产物"的理由），exit 1 ✅ |
| `.idfc` 里 `ActiveOverlays.dac_chip = "nope"` | `matrix` 与 `build --dry-run` 都报错 exit 1 —— **确实没有静默回退**（§2.7 规则 11 达成）✅ |
| `ConfigFile: null` 的类型 | 正常解析、不追加文件 ✅ |

**B. 参数与单元格解析**（真实项目）

| 用例 | 实测结果 |
|---|---|
| `build -t debug -o dac_chip=pcm5102 --dry-run` | `-B build\pcm5102.debug` + `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug;sdkconfig.dac_chip.pcm5102"` + `SDKCONFIG="build\pcm5102.debug\sdkconfig"` —— 与 §1.3 逐字一致 ✅ |
| 另外两格（`-t release -o pcm5102`、`-t releasecandidate -o cs43131`） | 拼接同样正确 ✅ |
| `-o dac_chip=xxx`、`-o pcm5102`（缺 `=`）、同维度重复、`-o dac_chip=`、`-o variant=debug`、`-t nosuchvariant` | 六种全部 exit 1 并给出可用维度/类型 ✅ |
| `-o` 贯通 | `build` / `clean` / `fullclean` / `flash` / `monitor` / `size` / `run` / `menuconfig` 都接受；`matrix -o` 可做预览 ✅ |
| 旧式目录提示 | 首次构建时打印"检测到旧式构建目录 `build\debug`，它不会被当前构建复用"（§5.2 的建议已实现）✅ |

**C. 构建、过期检测、清理**

| 用例 | 实测结果 |
|---|---|
| `idfc build`（cs43131.debug） | 全量构建成功，164.5 s，`FakePodNanoNightPlayer.bin` = 1,300,416 B（0x13D7C0，约 1.24 MB，符合 debug 预期）✅ |
| `idfc build -o dac_chip=pcm5102` | 全量构建成功，160.6 s，体积与 cs43131 格**完全相同**（只差一行日志，符合预期）✅ |
| 生成的 sdkconfig | cs43131 格：`CONFIG_AUDIO_DAC_CS43131=y` + `# CONFIG_AUDIO_DAC_PCM5102 is not set`；pcm5102 格反之 ✅ |
| 只改 `sdkconfig.dac_chip.cs43131` 的时间戳 | `matrix` 该格变"已过期"；`build --dry-run` 打印"构建相关文件发生了更新 → clean + reconfigure" ✅（**§4.2 ① 的关键要求达成**） |
| 改 `idfc_buildconfig.json` 的时间戳 | **全部**单元格变"已过期" ✅ |
| 增量重建 | 两格各 51.8 s / 50.8 s，状态回到"已构建" ✅ |
| `clean -o dac_chip=pcm5102` | 只作用于 `build\pcm5102.debug\sdkconfig` 与 `…\config`，其它格不受影响 ✅ |

**D. 端到端（烧录 + 运行）**

| 用例 | 实测结果 |
|---|---|
| `flash -o dac_chip=pcm5102` + `monitor --wait-for "DAC chip"` | 过期警告带单元格名；烧录成功；串口输出 `I (1822) app_main: DAC chip: PCM5102 (build-time selection only, real switching not implemented)` ✅ |
| `flash`（默认格）+ `monitor --wait-for "DAC chip"` | `DAC chip: CS43131 (build-time selection only, real switching not implemented)` ✅ |
| `size -o dac_chip=pcm5102` | 报告写到 `build\pcm5102.debug\size_analysis_report.txt` ✅ |
| `menuconfig search -o dac_chip=pcm5102 AUDIO_DAC` | 读的是该格的 `config/*.json`：`CONFIG_AUDIO_DAC_CS43131 = n`、`CONFIG_AUDIO_DAC_PCM5102 = y` ✅ |

**E. 状态持久化**

| 用例 | 实测结果 |
|---|---|
| `set-overlay dac_chip pcm5102` | 写入 `.idfc` 的 `ActiveOverlays`（`NullValueHandling.Ignore` 生效，字段只在需要时出现）；`status` / `matrix` 立即显示 `pcm5102.debug` ✅ |
| `set-overlay dac_chip --default` | 字段被**整个移除**，回到 `cs43131.debug` ✅ |
| `set-overlay dac_chip nope` / `set-overlay variant debug` | 报错并列出可用类型/维度，exit 1 ✅ |

### 7.2 与本文档的差异（文档已修正）

- **尾随逗号**：实测 `Newtonsoft.Json` **容忍**尾随逗号（留了 `,` 不报错，只会在后续字段校验时才暴露），本文档 §2.6 原先"不能有尾随逗号"的说法已改为"能解析但仍不要写，换严格解析器会失败"。

### 7.3 发现的小问题（均非阻塞，供后续打磨）

1. **`status` 不接受 `-o`**：`matrix -o dac_chip=pcm5102` 可以预览"换一个类型后是哪一格"，但 `status -o … ` 报"参数不在可接受列表中"。建议 `status` 也接受 `-o` 做预览，或在 help 里显式说明它只反映已保存的选择。
2. **"已构建"的判定偏乐观**：只检查 `<cell>/sdkconfig` 是否存在。实测在 `pcm5102.debug` **首次构建进行中**时，`matrix` 就已把它显示为"已构建"（reconfigure 阶段早已生成 sdkconfig、编译还没结束）；编译中途失败也会留下同样状态。建议同时要求 `*.bin`/`*.elf` 存在，或把这种状态单列一档（如"配置已生成，未完成编译"）。
3. **错误文案的来源提示**：命令行传 `-o variant=debug` 时，报错里附带了"如果它来自 `.idfc` 的 `ActiveOverlays`…"的建议，而这次来源其实是命令行。文案可以更中性（如"若来自 `.idfc`，请用 `set-overlay` 修正"）。
4. **输出顺序**：`.idfc` 里的选择失效时，`matrix` 先打印了表头/部分内容再报错（退出码 1 正确）。可以先校验、后输出，避免"半张表 + 一条错误"。

---

## 附：相关文件

**本仓库（固件侧）**

- `idfc_buildconfig.json` —— 矩阵定义（本文档 §2 的实例）
- `sdkconfig.defaults` / `sdkconfig.defaults.<variant>` —— BaseConfig 与 variant 维度
- `sdkconfig.dac_chip.cs43131` / `sdkconfig.dac_chip.pcm5102` —— overlay 维度配置
- `main/Kconfig.projbuild`（`AUDIO_DAC_CS43131` / `AUDIO_DAC_PCM5102`）、`main/main.c`（`#if/#error` 兜底）
- `ideas_and_exps/BUILD_CONFIG.md` §8 —— 固件侧使用说明；`ideas_and_exps/IDFC_USAGE.md` —— idfc 现有用法

**idfc 仓库（`Q:\Projects\CSharp\idf-client-cli\`，基线 commit `85a73ab`）**

- `IDFClientCLI\Client\BuildHelper\BuildVariantHelper.cs` —— 核心：路径、配置链、过期检测、清理
- `IDFClientCLI\Client\CommandArgsExtension.cs` + `Core\ArgExtensions.cs` —— `-t`/`-p`/`-o` 解析
- `IDFClientCLI\Client\ConfigStore\ConfigStore.cs` / `ConfigCommand.cs` —— `.idfc` 状态与 `set-*` 命令
- `IDFClientCLI\Client\IDFCommands\*` + `Client\CustomCommands\*` —— 各命令的 cell 解析
- `IDFClientCLI\IDFClientCLI.csproj` —— 新增 `.cs` 文件必须登记 `<Compile Include>`
- `README.md` —— 需同步的文档章节见 §4.2 ⑦
