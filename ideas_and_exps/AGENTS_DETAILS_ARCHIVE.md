# AGENTS.md 压缩归档（2026-11-15）

> 2026-11-15 `AGENTS.md` 已从 82KB 压到 29KB（工作区指令预算 64KB，超了会被截断）。
> **规则性内容仍留在 `AGENTS.md`**；这里收的是从它里面移出来的**详细实测数据、
> 历史经过与逐项理由**，便于追查"当年为什么这么定"。各条末尾都指了更详细的文档。
>
> 结论已经在 `AGENTS.md` 里以一句话规则的形式保留，本文件只在需要考据时查。

---

## 1. 内部 RAM / IRAM / DMA 的历史实测

**硬件事实**：ESP32-S3 上 `IRAM(0x4037xxxx)` 与内部 `DRAM(0x3FC8xxxx)` 是同一块 SRAM 的
两个别名（`heap/port/esp32s3/memory_layout.c` 的 `soc_memory_regions` 成对给出配对地址），
`.iram0.text` 里超出前 16KB 的**每一字节都从内部 DRAM 堆里扣**（map 里的 `.dram0.dummy`）。
S31 同理。这正是"对象都搬去 PSRAM 了、可用内部 RAM 却不见涨"的原因。

**LVGL 绘制缓冲（`V_RES/10`）时代**：460/10=46 行 → 每块 460×46×2B ≈ 41KB，双缓冲 ≈ 83KB
DMA；实测 DMA 稳态 19.4KB / 最大连续块 17.4KB，SD 读无失败。
`V_RES/20` 那次测量的 DMA 池分布：启动 263KB → display 后 246KB → audio 后 233KB →
SD 后 227KB → **LVGL 后 171KB（绘制缓冲吃掉约 55KB）** → 开机完成 45KB。当年那 126KB
的"真凶"是 `app_ui_create()` 的 LVGL 控件对象约 105KB / +1795 个分配块（**LVGL 8.4 时代**；
v9 的 `LV_USE_CUSTOM_MALLOC` 把它们搬进 PSRAM 后不再成立）。
降档实测：`V_RES/20` 约 20fps、`V_RES/12` 接近原 30fps、`V_RES/10` 约 30fps（S3 时代）。

**蓝牙（2026-11）后曾降到 `V_RES/16`**：BT 控制器 + 协议栈从内部 DMA 池吃掉约 13KB，
实测 `/10` 时开机完成后 DMA 仅剩 **31B**、SD 读全部失败 → 所有歌被判坏曲。
`/16` 每块 460×28×2B≈25KB、双缓冲≈51KB，比 `/10` 省约 33KB DMA，代价是帧率略降。
**2026-11-15 双固件把 WiFi 移出播放器后已恢复 `/10`**（并加了 `/12`、`/16` 逐级回退）。

**当前基线（2026-11-15，debug）**：BT 关 `iram≈110KB / fps 61~63`；BT 开连耳机播放中
`iram≈58KB / fps 57~62`；开机日志 `draw buffers: V_RES/10 (42368 B/块, 双缓冲 84736 B)`。

---

## 2. 堆毒化（不要打开的实证）

`CONFIG_HEAP_POISONING_COMPREHENSIVE=y` 实测多占约 **26KB** 内部 RAM，并把最大连续块从
31.7KB 压到 14.8KB（给每个分配加带调用栈的元数据头，且禁止空闲块合并，LVGL 的小对象
把碎片放大）——这是"**release 从未出现、debug 才出现**内部 RAM 问题"的根因，不是版本差异。
2026-09-10 把最后一个开着它的 releasecandidate 一并改 DISABLED：RC 开毒化时比 debug
少 18~28KB 内部 RAM、最大连续块被压到 2.8KB（DMA 仅剩 955B / 连续 80B）。
详见 `IRAM_DMA_AUDIT.md`。

---

## 3. IRAM 审计（2026-10-16，`IRAM_AUDIT_LVGL9.md`）

- **IRAM 代码** 166.4KB → **108.9KB**（裁掉 6 种用不到的颜色格式，其中 LVGL 部分
  82.8→25.3KB；当时 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 仍为 y）。
- **内部 RAM 余量**：仅裁格式时稳态 84.2KB；再叠加"路径B"（把两个滚动列表页的热对象钉在
  内部 RAM）后稳态 38.8KB、开机完成最大连续块 11.3KB、DMA 最大连续块 11.3KB——比
  `LVGL9_MIGRATION.md` §10.4 记录的 29.5KB/20KB 更宽裕，列表快速滚动 11→15fps。
- **三个性能杠杆的排序**（S3 实测）："对象放哪（内部 21fps vs PSRAM 11fps）" >>
  "blend 放哪（11 vs 9）" >> "字体方案（11 vs 12）"——与"blend 是绘制热点"的直觉相反
  （滚动开销主要在对象树遍历/布局，对象工作集约 88KB，远超 32KB 数据 cache）。
- **S31 复测（2026-11）**：路径B 默认关闭（`CONFIG_UI_LVGL_PIN_HOT_OBJECTS=n`）。S31 上
  `LVGL_INTERNAL_RESERVE`(40KB) 很早就触发，路径B 只钉得进 **20.8KB**（不是 S3 的 82.8KB）；
  关掉后内部 RAM 稳态 147,175 → **167,707 B（+20.5KB）**、开机完成最大连续块
  31,744 → **86,016 B（+53KB）**，歌曲列表快速滚动 **24~36fps**，真机确认可接受。
  详见 `IRAM_OBJECTS_PSRAM_S31.md`。

---

## 4. 任务栈与 deep_worker 的历史事故

- **LVGL 栈**：LVGL 8.4 时代 6KB 就够（实测高水位 3.3KB）；v9 绘制管线深得多，**6KB 会在
  "首次渲染设置页"时溢出** —— 实测从主界面滑到第 4 页（播放设置）必定
  `A stack overflow in task LVGL has been detected` 并 panic 重启；12KB 后位图字体时代
  高水位 5532B。2026-10 TTF 迁移时 FreeType 把光栅化池放栈上，12KB 实测只剩 172 字节，
  才上调 20KB（实测余 ~8.3KB）；2026-11 改回位图（NPF1）不再光栅化 → 回到 **12KB**。
- **deep_worker 早期创建**：栈 12KB、prio 6、Core0、信号量驱动。**必须在 `app_main` 早期
  创建**（config_store 之后、硬件初始化之前）：内部 RAM 在开机末期高度碎片化
  （最大连续块约 8.6KB），放到 boot_flow 之后再创建会失败。历史上曾因把这种大栈任务挪到
  开机末期 + 未检查 `xTaskCreatePinnedToCore` 返回值而**静默失败** → 任务不存在 → 每次深调用
  等满 5s 超时（按键息屏/亮屏延迟 5 秒）。现在会检查返回值 + `s_exec_task == NULL` 快速失败
  （就地兜底执行）+ 超时时 dump 全部任务状态。"早期创建"也正是 12KB 能分配成功的前提
  （早期内部 RAM 还有 ~200KB 空闲 / 118KB 连续；实测创建成功、HWM 余 11.5KB）。
- 各任务栈均按 `mem_monitor` 的高水位实测收紧；调栈前必须覆盖目标路径实测
  （如自动息屏/亮屏后才打印的读数），不能只看亮屏态。

---

## 5. "息屏停掉 Core1"（`CONFIG_SCREEN_OFF_STOP_CORE1`）

该方案已从代码与 Kconfig **完全移除**。曾实测失败：长期 `esp_cpu_stall` 与被停核的
PSRAM/XIP 取指形成总线竞态 → INT WDT 超时。**勿重新引入。**
真正的进一步省电方向：暂停/停止时 light sleep；关机走 PMU 软关机（真断电）。

---

## 6. 绘制层与 64B 对齐（事故细节）

- **每层不透明整块背景 = 每个绘制块多填一遍 2 万像素**。现在 `scr`/`tabview`/tab 页全部透明，
  黑底只由 `main_ui` 的 tabview 内容容器提供（改 UI 层级时必看）。
- 卡片背景是"圆角帽（高 `2*corner`）+ 直角主体（从 `y+corner` 到底）"两段，视觉等价整卡圆角，
  但列表区因此走 `radius==0` 的纯色快速填充路径（`lv_draw_sw_fill.c` 只有
  `radius==0 && grad==NONE` 才走快速路径），不再每帧走圆角掩码。
- **64B 对齐事故**：LVGL 9 会把 `lv_image_dsc_t.data` 包成 draw_buf，指针不是 64 的倍数时
  **每次**刷 `W lv_draw_buf_init: Data is not aligned, ignored`。实测切歌后（真封面出现在
  正在播放页）滑动一次就刷一大片，115200 波特下一条 ~5ms，直接把 UI 卡死——现象就是
  "切歌之后变卡、只有带图片的页面卡"。封面已用 `heap_caps_aligned_alloc(64, …)`。
- **QSPI 80MHz 黑线**：2026-11 曾把 `AMOLED_QSPI_MAX_PCLK` 上调到 80MHz 做实验，结果留下
  "偶发 1px 纯黑竖线"：被局部重绘的区域**左边缘多出一条贯穿该区域高度的 1px 黑线**，
  且随对象**宽度变化**累积（变宽时被一起清掉）；连 LVGL 自带 FPS 监视器、进度条时长标签都有，
  说明是 **QSPI 传输时序边际**而不是绘制逻辑。判定依据：同一份固件只把时钟退回 40MHz，
  黑线立刻消失。**已回退到 40MHz，不要再调回去**；排查画面残留先退回 40MHz 做对照。

---

## 7. `sdkconfig` 里各 LVGL 配置项的逐条理由（压缩自旧 AGENTS.md）

- `LV_USE_CUSTOM_MALLOC=y`：LVGL 对象堆整体走 `core/ui/lvgl_mem.c` 进 PSRAM。不加这条
  （走 CLIB = 标准 malloc → 内部 RAM）实测开机完成内部 RAM 只剩 **23 字节**、DMA 只剩 19 字节、
  最大连续块 0 → SD 读失败 + newlib 建锁 abort（重启循环）。两次复测（blend 留 IRAM 82.3KB /
  关 FAST_MEM 106.3KB）都在 `app_ui_create` 之后见底：内部 RAM 仅剩 7,735B、DMA 仅 19B，
  `sdmmc_cmd: allocate_dma_buf: not enough mem` → 判坏曲跳歌。对象堆峰值 ≥105KB，本项目内部
  RAM 装不下。**这是唯一可行方案**，见 `LVGL9_MIGRATION.md` §10。
- `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`：开启会让约 82.8KB 混合代码进 IRAM（= 同样多地从内部
  RAM 扣）。关闭后内部 RAM 稳态 23.5→106.7KB、最大连续块 3.5→31.7KB，帧率只掉约 1 帧；
  代码进 `.flash.text` 后由 `SPIRAM_FETCH_INSTRUCTIONS` 拷到 PSRAM 执行（比 flash 快）。
  2026-11 因为蓝牙控制器要一块 21KB 连续内部 RAM（不够时 `orca_ems_env_init` 直接失败并
  panic）而正式定为 n。详见 `BLUETOOTH_AUDIO.md` §4。
- 裁掉 6 种颜色格式（XRGB8888 / ARGB8888_PREMULTIPLIED / L8 / AL88 / A8 / I1）：LVGL 9
  默认 10 种格式各一份混合实现，本机只用到 RGB565/ARGB8888/RGB565A8/RGB888。
  ⚠ 缺格式的失败方式是**静默**的（目标格式缺 = 整块不画且无日志；源格式缺 = 一条
  `LV_LOG_WARN`；变换缺 = 花屏）。排查用 `idfc monitor --grep "not enabled"`。
- `LV_USE_PPA=n` + 自定义 SW blend handler：9.4 上游的 `lv_draw_ppa*.c` 在 PARTIAL 渲染下
  有硬伤（FILL 忽略 buf_area 每 46 行一条黑线、非阻塞却立刻标记完成、每次操作同步整块缓冲），
  改用 `core/ui/lvgl_ppa_accel.c` 只把"向 RGB565 图层做图像混合"交给 PPA。实测 PPA **FILL**
  约 285~330µs / 1.5 万像素（≈50M px/s），比 320MHz CPU 软件填充慢一个数量级，所以纯色填充
  默认仍走 CPU。见 `PPA_ACCEL.md`。
- `LV_USE_SYSMON=y` + `LV_USE_PERF_MONITOR=y`：v9 起性能监视器挂在 SYSMON 之下，
  SYSMON 默认 n，不显式打开会被 Kconfig **静默丢弃**（屏幕角落就没有 FPS）。
- `LV_BUILD_EXAMPLES`/`LV_BUILD_DEMOS` 关：examples 下 250 个 .c 编了也会被 `--gc-sections`
  全丢（map 里 0 处出现），纯省全量编译时间。
- `LV_FONT_MONTSERRAT_12/16/20/24/28/32/48` + 默认 18、`LV_FONT_FMT_TXT_LARGE=y`、
  `LV_ATTRIBUTE_MEM_ALIGN_SIZE=64`、`LV_DRAW_BUF_ALIGN=64`（PPA 与绘制缓冲的硬要求）。

---

## 8. idfc 为什么不能放进工作区（完整机制）

DeepSeek Harness v0.1.7-rc.1 起会给工作区打**可继承的 Low 完整性标签**（`icacls` 可见
`Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`）。Windows 派生进程的完整性 =
`min(创建者令牌, 映像文件)`，所以工作区内的 exe **必定以 Low 启动**，写不了 `%LOCALAPPDATA%`
（服务器的权限自检正是探测这个），于是 `start-server` 被拒绝。**`danger-full-access` 也救不了**
——它只解除 DSH 沙箱、不解除完整性标签。唯一出路：把 `idfc.exe` 放到工作区外的 PATH 目录
（已实测：普通权限、无提权、无 `danger-full-access` 也能正常启动并长期存活）。

---

## 9. 分区重排历史（细节见 `partitions.csv` 头部注释）

2026-10 起共重排四次：① TTF 化时从 factory 割 2MiB 给 font（pldata 0x610000→0x400000，
**旧播放列表作废、首次开机重扫**）；② 换 3.55MiB 字体后 font 8M→4M、factory 3.9375M→7.9375M，
pldata 挪到尾部 0xC00000 锚定（再次重扫，nvs 未动）；③ 改回 NPF1 位图全字库：font 4M→6M
@0x600000、factory 7.9375M→5.9375M，**pldata 不动**（播放列表与配置都不丢，但要重刷字体）；
④ 2026-11-15 双固件：factory 5.9375M→3M，新增 `ota_0`(2.5M@0x310000) 与 `otadata`(8K@0x590000)，
**font/pldata/nvs 全不动**（不重刷字体、不重扫列表，但要重刷分区表）。
