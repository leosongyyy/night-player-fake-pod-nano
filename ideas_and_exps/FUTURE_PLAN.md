# 未来计划

> 本文档用于存放想法、Bug分析记录和和未来计划  
> 请将已完成的内容请移动到隔壁 DONE_PLANS.md 中，同一会话中所作的对同一个功能点的变更应该合并在一条记录里。

## 23. 歌词显示

还没想好

## 34. 在Token预算充足时，调查谁吃了IRAM，并尽量把LVGL的缓冲区优化回以前的1/10 VRES的程度。

> 🆕 **2026-09-16：本条现在有了现成的调查工具**——`idfc.exe size` 可打印固件体积分布表；
> `idfc.exe size --detailed` 输出精确到 `.obj` 的树形分析（同时落盘到 `build/<variant>/size_analysis_report.txt`）。
> 实测 debug 版基线：**`IRAM 100.0%（16384/16384，余 0）`、`DIRAM 47.06%（余 180,933 字节）`**。
> ⚠ 解读时注意：**IRAM 被填满是 ESP32-S3 的正常现象**（.text/.vectors 本就会用满），
> 真正该盯的是 **DIRAM 余量**与 **`MALLOC_CAP_INTERNAL` 的最大连续块**（见下文验收标准）。
> 配合串口的 `MEMTRACE:` 逐阶段插桩，可以定位到"是哪一阶段吃掉的"。

背景（2026-09 实测记录，详见 `PSRAM_TASK_STACK.md` 第 4 节）：
- 虽然v0.9.0.0_alpha3 能正常且稳定地运行，但是是关闭了堆毒化后卡着极限在运行。
- 现状：`EXAMPLE_LVGL_BUF_HEIGHT = EXAMPLE_LCD_V_RES / 12`（460/12=38 行 → 每块
  460×38×2B ≈ 34KB，双缓冲 ≈ 68KB **DMA**），帧率接近原 30fps，DMA 稳态余量约 15KB。
- 为什么从 1/10 降下来：原 `V_RES/10`（双缓冲 ≈ 83KB DMA）会把 DMA 池压到不足 3KB，
  SD 卡每次读的 512B `allocate_dma_buf` 都分配不出来 → 解码器从未启动 → 被
  `mp3_player` 判为"数据损坏" → 坏曲自动跳过。用户侧现象是"开机先播一秒上次的歌，
  然后跳到别的队列去"。所以这是**内存不足的连锁反应**，不是播放/随机逻辑的 bug。
- ⚠ **2026-09-10 实测修正，详见 `IRAM_DMA_AUDIT.md`**：上面"开机流程吃掉 126KB DMA"的
  归因**是错的**。用新增的 `core/utils/mem_trace` 逐阶段插桩（debug 与 releasecandidate 各跑
  一轮完整启动 + 切歌）得到的结论：
  - **封面加载不占内部 RAM，更不占 DMA**：`.npd` 文件缓冲 61~77KB、JPEG 数据副本 61~77KB、
    解码像素 423200B **全部落 PSRAM**（用 `esp_ptr_external_ram()` 实测落点确认）。
    整条链路在内部 RAM 上只有 ~3~4KB 临时开销（JPEG 的 3100B work buffer 等），切歌即释放。
    JPEG 解码确实**不需要 DMA**——那个工作缓冲用的是 `MALLOC_CAP_DEFAULT`。
  - **真凶是 `app_ui_create()` 的 LVGL 控件对象**：`LV_MEM_CUSTOM=stdlib` + 阈值
    `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 让每笔 ≤16KB 的 `malloc` 都**强制落内部 RAM**，
    实测这一段吃掉 **105KB 内部 RAM、新增 1795 个分配块**（其中 `settings_menu` 最大，约 55KB）。
    此前"开机流程 126KB"其实是这份 UI 对象开销——只不过封面加载同期发生，被误记成了凶手。
  - **debug 与 releasecandidate 相差 18~28KB 内部 RAM，唯一配置差异是
    `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`**（RC 独有，debug 早在 2026-09 已关）。
    这就是"debug 从未出现、只有 RC/release 出现"的真正原因——不是版本差异而是配置差异。
  - **RC + V_RES/10 已复现故障**：`app_ui_create` 结束时 DMA 只剩 **955B / 最大连续块 80B**
    → `sdmmc_cmd: allocate_dma_buf: not enough mem` → 读曲失败 → 判坏曲 → **开机跳曲**；
    同刻内部 RAM 最大连续块只有 **2804B**，而 `sleep_timer` 需要 4KB 内部栈
    → **睡眠定时器服务起不来**。
- 修复优先级（按性价比）：① RC 关闭堆毒化（+18~28KB，零风险）；② 懒创建
  `settings_menu` / `notification_panel`（+50KB 以上）；③ 调低
  `SPIRAM_MALLOC_ALWAYSINTERNAL` 让 LVGL 小对象落 PSRAM（+80~100KB，需实测滚动帧率）；
  ④ LVGL 自定义分配器按大小分流。
- IRAM 侧仍待查的对象：LVGL 堆（`LV_MEM_CUSTOM=stdlib` + 几百个常驻控件/样式/缓存）、
  `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 这个阈值（<16KB 的分配优先走内部 RAM）、
  以及各驱动的常驻缓冲。`mem_monitor` 的 `iram=` 列与
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` 是第一手指标。
  此前在OtoClock上曾经研究出过给LVGL自定义内存分配的方案，或许可以把控件相关扔到PSRAM里去？（测测性能）
- 验收标准建议：绘制缓冲回到 `V_RES/10`（≈83KB DMA）后，DMA 稳态余量仍 ≥ 8~10KB、
  SD 卡读写无 `allocate_dma_buf` 失败、列表滚动帧率回到 30fps；
  **另加一条**：`MALLOC_CAP_INTERNAL` 的最大连续块 ≥ 4KB（SD 读要 512B、内部任务栈要 4KB，
  只看 free 不够，`largest` 才是能否分配成功的判据）。
- ⚠ 硬性约束：绘制缓冲**必须** `MALLOC_CAP_DMA`（非 DMA 会触发 SPI bounce 缓冲分配，
  失败则刷屏阻塞 → 任务看门狗复位）；调这个值前必须同时看帧率与
  `heap_caps_get_free_size(MALLOC_CAP_DMA)`。
  （当然，绘制缓冲区必须在IRAM里，不然带宽就不够刷满30帧了）

## 43. idfc.exe 的改进想法（工具链体验）

> 原始需求清单写于 2026-09 一次 ClickWheel 会话的摩擦点汇总（当时误编为 #42，与
> `DONE_PLANS.md` 的 #42 ClickWheel 撞号，此处更正为 **#43**）。前提不变：idfc 的
> "服务器 + SSE 流式输出"架构本身够用，痛点几乎全在**"人/AI 必须记住隐式约定"**上。
> 原始 4 个坑：① 不带 `-B` 时静默用根 `sdkconfig` + 根 `build/`（还顺手跑了一次 cmake 配置），
> 于是构建出的固件带着**过期的堆毒化配置**，连锁出内部 RAM 紧张 → 任务建不起来 → SD 读
> `allocate_dma_buf` 失败 → 开机跳曲；② 会话中途 IDF 服务器被回收，只能停下等用户手动启动
> VBS；③ `monitor` 永不返回，只能"后台起任务 + sleep + kill + 读增量"；④ 串口号要靠人告知。
>
> **2026-09-16 一轮集中开发后，原 17 条中的大半已实现**（idfc 重写为子命令版）。
> 已完成部分见 §43.1（**待移入 `DONE_PLANS.md`**），仍未做的见 §43.2——那才是真正的未来计划。

### 43.1 已实现（2026-09-16，待移入 DONE_PLANS.md）

| 原条目 | 现状与实现方式 |
|---|---|
| ① `-B` 缺失/不匹配要报错 | ✅ **从根本消除**：`build`/`flash`/`monitor`/`run`/`size` 自动拼 `-B build/<variant>` 与 `-D SDKCONFIG_DEFAULTS=… -D SDKCONFIG=…`，调用方不再写 `-B`（`idfpy` 仍可绕过，文档已警告） |
| ④ 配置过期提醒 | ✅ `build` 自动比较 `build/<v>/sdkconfig` 与 `sdkconfig.defaults*`、所有 `Kconfig`/`*.projbuild` 的时间戳；过期即自动 clean + reconfigure 并打印原因。另有 `build --dry-run` / `--reconfigure` / `--no-reconfigure` |
| ⑧ `monitor --duration` | ✅ `monitor --timeout <ms>`：到点自退，**退出码 1** |
| ⑨ `--until` / `--no-reset` | ✅ `monitor --wait-for <re>`（命中即退，**退出码 0**）、`--no-reset` |
| ⑪ `idfc run` 一条龙 | ✅ `run` = 构建 → 体积 → 烧录 → 监视，`--monitor-timeout/-grep/-wait-for` 控制抓取（实测一次约 31 秒跑完并命中 `App started`） |
| ⑫ `idfc ports` | ✅ `ports` 列出串口 + 设备名 + 设备实例路径（含 Espressif `VID_303A` 可用于识别目标板）；另有 `set-port` 把端口持久化进项目 |
| ⑮ profile 简写 | ✅ `-t <variant>` + `set-variant`；**比原设想更彻底**——默认值持久化在 `.idfc/idfc_config.json`，日常连 `-t` 都不用带 |

**清单外、一并实现的能力**（这些同样应记入 DONE_PLANS）：

- **输出折叠**：`build`/`flash` 的刷屏输出被折叠，实测 releasecandidate 全量构建 **1606 行 → 160 行（1:10）**，
  单块最大折叠 835 行，且标记会报出被隐藏行数；`-v` / `--verbose-build` / `--verbose-flash` 可展开。
- **`start-server` 就绪轮询 + `stop-server`**：不再"报了成功但服务器其实没起来"；停止不再只能关控制台窗口。
- **`size` 子命令**：ASCII 表格 / `--detailed` 树形体积分析（依赖 `.elf`+`.bin`+`.map` 三件套）。
- **未知命令 / 未知参数明确报错**（退出码 1）。历史上曾出现"未知命令静默 exit 0"——最危险的失败模式。
- **`monitor --grep <re>`**：只输出匹配行（仅过滤 stdout，stderr 与错误信息始终透出）。
- **项目级配置**：`set-idfenv` / `set-variant` / `set-port` → `.idfc/idfc_config.json`。
- **`idfpy` 的完整 Windows 命令行转义**（引号、结尾反斜杠、空参数均逐字保真）。
- **`help` / `help <命令>` / `help --all`** 的分命令帮助体系。

### 43.2 仍未实现（真正的未来计划）

按原分组保留，已完成项已剔除。

#### 43.2.1 防呆（剩余）

- **② 每条命令开头回显解析结果**：目前只有 `build --dry-run` 会打印完整命令行。建议 `build`/`flash`/`monitor`
  **实际执行时**也回显一行 `[idfc] build dir=build/debug  sdkconfig=… (mtime 09-10 21:25)  target=esp32s3  idf=5.5.5`
  ——一行就让"我到底在构建哪套"变成可见事实。

#### 43.2.2 面向非交互调用者（脚本 / AI）

- **⑦ `idfc version -B build/release`**：从 ELF 读 app version + `git describe` + 编译时间。
  有子命令才能直接核对"设备上烧的固件 vs 当前工作区源码是否一致"（目前只能从串口日志里认）。

#### 43.2.4 状态类子命令（剩余）

- **⑬ `status` 扩展**：目前有"服务器是否在线 + 项目配置（构建类型）"，但**没有各 `build/<cfg>` 的状态**——
  是否存在？`sdkconfig` 相对 defaults 是否过期？产物 mtime 是否比源码新？做成一张"我现在处于什么状态"的仪表盘。

#### 43.2.5 顺手的小改进（剩余）

- **⑰ 回显等价命令行**：`build --dry-run` 已能打印，但 `run` 的各阶段（构建/体积/烧录/监视）执行时
  也应打印实际使用的命令行，便于贴进文档或复盘。

#### 新功能：
- build目录下有个json记录了所有可用的menucondig设置项，且其中包含详细的描述说明。

### 43.3 本轮（2026-09-16）新发现的问题

排查过程中新暴露、尚未处理的点：

- **`flash` 不做配置过期检测**：`idf.py flash` 会隐式构建，因此"改了 defaults 直接 flash"会烧出**旧配置的固件**。
  目前选择在 `help` 里显式声明这条限制（"本命令不会检测构建是否需要刷新，建议在 `build` 之后进行 `flash`"）。
  可考虑：至少打印一条警告。
- **`flash` 没有 `-b <baud>`**：调波特率只能靠 `ESPBAUD` 环境变量，而受"服务端启动时快照环境变量"的架构约束，
  必须在**启动服务器之前**设置并重启。加一个 `-b` 参数显式拼进命令行会更直接。




