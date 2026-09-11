# 未来计划

> 本文档用于存放想法、Bug分析记录和和未来计划  
> 请将已完成的内容请移动到隔壁 DONE_PLANS.md 中，同一会话中所作的对同一个功能点的变更应该合并在一条记录里。

## 23. 歌词显示

还没想好

## 34. 在Token预算充足时，调查谁吃了IRAM，并尽量把LVGL的缓冲区优化回以前的1/10 VRES的程度。

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

## 42. idfc.exe 的改进想法（工具链体验）

> 2026-09 一次完整会话（ClickWheel 按格滚动那次）的实测摩擦点汇总。前提：idfc 的
> "服务器 + SSE 流式输出"架构本身已经很好用，痛点几乎全在**"人/AI 必须记住隐式约定"**上。
> 那次真实踩到 4 个坑：① 不带 `-B` 时静默用根 `sdkconfig` + 根 `build/`（还顺手跑了一次
> cmake 配置），于是构建出的固件带着**过期的堆毒化配置**，连锁出内部 RAM 紧张 → 任务建不起来
> → SD 读 `allocate_dma_buf` 失败 → 开机跳曲；② 会话中途 IDF 服务器被回收，只能停下等用户
> 手动启动 VBS；③ `monitor` 永不返回，只能"后台起任务 + sleep + kill + 读增量"，丢尾部又靠
> 猜时间；④ 串口号要靠人告知（文档里留的是另一台机器的 COM12，本机实际是 COM5）。
> 下面按"值不值得做"分组，条目编号仅用于引用。

### 42.1 防呆（上述 ① 类事故，价值最高）

1. **`-B` 缺失 / 不匹配要直接报错，而不是静默退化**：① 若 `build/*/` 下有多个配置目录而调用方
   没给 `-B`，直接失败并列出候选项；② 若给了 `-B build/debug` 但 `-D SDKCONFIG=` 指向别的目录，
   拒绝执行（这正是"配置漂移"的入口）。
2. **每条命令开头回显解析结果**：
   `[idfc] build dir=build/debug  sdkconfig=build/debug/sdkconfig (mtime 09-10 21:25)  target=esp32s3  idf=5.5.5`
   —— 一行就让"我到底在构建哪套"变成可见事实。
3. **`idfc config-check -B build/debug`**：拿生效的 `build/<cfg>/sdkconfig` 与 defaults 链逐项比对，
   列出差异项并以非零码退出（另配 `config-diff` 看具体值）。那次"堆毒化仍开"的漂移是手写
   PowerShell 才查出来的——若构建/烧录前默认跑一遍，这类问题会当场暴露。
4. **配置过期提醒**：`sdkconfig.defaults*` / `Kconfig.projbuild` 的 mtime 比
   `build/<cfg>/sdkconfig` 新时，提示"配置可能未生效，需删 sdkconfig 再 reconfigure"——
   即那条 `replace=False` 合并坑（见 AGENTS.md 第 5 节）的自动检测器。

### 42.2 面向非交互调用者（脚本 / AI）的输出与退出码

5. **诊断摘要 / 过滤模式**：`idfc build --diagnostics`（只流 `error:|warning:|FAILED|E (` 等）
   或 `--tail N`，结尾给一段机器可读小结，如
   `BUILD_OK bin=0x416d60 free=32% elf=... version=0.9.0.0_alpha_4-2-g76ca90d warnings=2`；
   flash 同理 `FLASH_OK port=COM5 regions=3 verified`。现在要在 1500 行 ninja 进度里捞
   "binary size" / "Project build complete"，而长输出还会被上游截断或落盘。
6. **`--no-color`**：idf_monitor 传了 `--force-color`，非终端场景（日志文件、agent）里
   ANSI 序列是纯噪声。
7. **`idfc version -B build/release`**：从 ELF 读 app version + `git describe` + 编译时间。
   那次是从串口日志里看到 `0.9.0.0_alpha_4-2-g76ca90d` 才知道"设备上烧的是哪次提交"，
   有子命令可直接核对"设备固件 vs 工作区源码是否一致"。

### 42.3 monitor 的可用性（那次用得最痛）

8. **`monitor --duration 30 --log boot.log`**：到点自己退出、退出码 0、原始输出同时落文件。
   现在 monitor 永不返回，只能"后台起任务 + sleep + kill + 读增量"。
9. **`--until <regex>`**（如 `--until "App started"`）抓"到启动完成就停"；
   以及 `--no-reset` / 独立的 `idfc reset` 明确控制是否复位 —— 让"抓一次干净的开机日志"
   成为可复现的动作。
10. **落盘不丢 + 离线解码**：被 Ctrl+C/kill 时也要 flush 文件；再配
    `idfc decode <logfile>`（按对应 `-B` 的 ELF 做离线堆栈/地址还原）。那次 stderr 里
    `0x4200fbec: power_service_do_screen_off` 是实时解出来的，落盘日志能离线解会更好用。

### 42.4 一条命令走完固定流程

11. **`idfc run -B build/debug -p COM5`** = build → flash → 复位 → 抓 20~30s 启动日志 →
    输出"错误 N 条 / 已知无害警告 M 条"的小结（带可配置白名单，例如
    `i2c: This driver is an old driver`、`allocate_dma_buf`）。本项目"改完即烧录 + 看开机日志"
    是固定动作，一条命令能把三四个回合压成一个。

### 42.5 状态类子命令

12. **`idfc ports`**：列出串口 + 设备描述 + 自动识别 Espressif（USB-Serial/JTAG 的 VID:PID），
    标出哪个是目标板、哪个被占用。文档里写死的端口号必然会过时（另一台机器 COM12 / 本机 COM5），
    文档改成"用 `idfc ports` 确认"即可。更彻底：**每机一份不入库的配置**
    （如 `idfc.local.json` 记默认端口/默认 profile，或 `IDFC_PORT` 环境变量）。
13. **`idfc status`**：服务器是否在线（uptime、IDF 路径、是否有命令在跑）+ 各 `build/<cfg>`
    的状态（是否存在？sdkconfig 相对 defaults 是否过期？产物 mtime 是否比源码新？）——
    一张"我现在处于什么状态"的仪表盘。
14. **`idfc doctor`**：服务器可达性、IDF/python 版本、端口可用性、磁盘空间一次性体检。
    那次"IDF server is not running"只能停下等用户启动 VBS，有 doctor 就能一句话给出结论与修复命令。

### 42.6 顺手的小改进

15. **profile 简写**：`idfc --profile debug reconfigure` 自动展开为 `-B build/debug` +
    `-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug" -D SDKCONFIG="build/debug/sdkconfig"`。
    现在这串必须从文档复制，手抄极易出错，而 profile → 目录 → defaults 三者的对应关系本来就是死的。
16. **并发串行化**：两条命令同时发时明确回 `busy: 正在执行 build（已 12s）`，
    而不是让两路 SSE 交错。
17. **回显等价命令行**：build 也像 flash 那样打印实际执行的 `idf.py` 命令行（含展开后的 `-D`），
    便于贴进文档或复盘。

### 42.7 优先级建议

若只挑三个先做：**42.1 的 ③④（配置漂移检测 / 过期提醒）**、**42.3 的 ⑧⑨
（monitor 的 duration/log/until）**、**42.6 的 ⑮ + 42.1 的 ①（profile 简写 + `-B`/`SDKCONFIG`
一致性校验）**。共同点：都不需要新增能力，只是把**"必须记住的隐式约定"变成"命令自己会检查、
会说的显式事实"**——对 AI 协作尤其值钱（AI 不会记得上一台机器是 COM12）。

