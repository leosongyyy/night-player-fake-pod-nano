# 未来计划

> 本文档用于存放想法、Bug分析记录和和未来计划  
> **只有本文档（`FUTURE_PLAN.md`）里出现过的内容，才可以写进 `DONE_PLANS.md` 中**——
> 未在本文件登记过的临时改动/额外发挥，不做记录。若遇到踩坑，可以单独另起一个文档记录。
> 已完成的内容从本文档移入隔壁 `DONE_PLANS.md`，同一会话中所作的对同一个功能点的变更应该合并在一条记录里。

## 23. 歌词显示

还没想好

## 47. 虚拟列表滚动性能：重绑策略之后的下一步

**来源**：`DONE_PLANS` 第 34 点的尾巴（`IRAM_AUDIT_LVGL9.md` §13.4 第 2 条）
——「若要继续提速，方向在别处（绘制管线、虚拟列表的重绑策略等），不是继续钉对象」。
当前实测基线：列表快速滚动 **15 fps**、慢速 **20+ fps**，30 fps 目标未达成。

**已完成的部分（2026-10-16）**：**可行性调查**，报告见
**`ideas_and_exps/VIRTUAL_LIST_REBIND_STRATEGY.md`**。核心结论三条：

1. **重绑策略本身已接近最优**（每跨一行只重绑 1 行，O(1)/行），继续抠"重绑"收益小；
   只剩"图标文本/颜色无谓重设"这点单价冗余（`lv_label_set_text` 无条件
   free+malloc+度量，`lv_obj_set_local_style_prop` 无相同值短路）。
2. **真正的天花板是 LVGL 的滚动语义**：`lv_obj_scroll_by_raw()` 在 `lv_obj_scroll.c:413`
   无条件 `lv_obj_invalidate(obj)` → **每帧必然重绘整个列表区**（≈180K 像素、
   ≈110 个汉字字形）。要把 dirty 降成"一条窄带"，唯一办法是**不用 LVGL 原生滚动**
   （方案 S1 视口槽位制 + 自管滚动），代价是触摸滚动只能按整行吸附
   ——**该方案已因手感问题出局，见下方「已决策」**。
3. **fps 与单帧耗时成反比，没有"跨门槛翻倍"的巧取空间**：项目开了
   `LV_USE_PERF_MONITOR`，刷新定时器在渲染前**不被暂停**（`lv_refr.c:361` 的
   `#if !LV_USE_PERF_MONITOR lv_timer_pause(tmr)`），加上 `lv_timer.c:322` 先写
   `last_run` 再调回调 ⇒ 渲染一结束就立刻开下一帧，`fps ≈ 1000 / 单帧 ms`。
   所以 **15 fps ⇒ 单帧 ≈65 ms，30 fps ⇒ 必须砍到 32 ms（腰斩）**，
   收益线性可外推，验收口径必须是**单帧毫秒数**而不是 fps。
   （⚠ 调查初稿曾误判为"33 ms 节拍跳档"，已在报告 §2.1 更正。）

**已决策（2026-10-16，用户）**：**方案 S1（视口槽位制 + 自管滚动）出局**——
它的前提是触摸滚动只能按整行吸附、失去连续跟手，用户不接受。
⇒ **"把整块 dirty 降成一条窄带"这条结构性捷径已关闭**，
只剩"降低单帧成本"一条路。另一个推论：慢速滚动实测已有 48 ms 底座，
**即使重绑成本降到 0 也只能到 20 fps**，不砍底座就一定到不了 30 fps。

**待做（按顺序）**：

- 第 0 阶段（**一切判断的前提**，用户当前选择"暂不动固件"故推迟）：
  加"单帧耗时直方图"埋点（E1）+ 三段对照（E2：正常重绑 / 跳过重绑 / 静止，
  用 `lv_timer` 自动滚动），**1 次烧录、不改行为**。产出单帧毫秒数、重绑占比、
  以及"空帧率"（用来分辨"快速比慢速慢 15 ms"是重绑单价高、还是慢速时混了空帧）。
- 第 1 阶段（便宜、不改架构）：F2（`LV_FREETYPE_CACHE_FT_GLYPH_CNT` 1024→4096）
  + F4（重绑去冗余：图标 `strcmp`、颜色先比较再设）。
  单帧明显下降 ⇒ 进第 2 阶段；几乎不动 ⇒ 底座是"物理必需的绘制"，
  **要果断停手，不要零敲碎打**。
- 第 2 阶段（按 E1/E2 归因二选一）：底座是对象/样式/管线 → F5（去行容器）
  + S2（行位图缓存）；底座是字形 blend 像素量 → S3（Xtensa SIMD blend，
  当前 `LV_DRAW_SW_ASM_NONE`，需走 `LV_DRAW_SW_ASM_CUSTOM`）。
- 第 3 阶段：F1（`REFR_PERIOD` 33→16）——**只在单帧已 <33 ms 后做**，否则零收益。

**长期候选（用户提出并修订，已评估，未实施）——S4：自己接管列表绘制**

用户设想（借鉴 NightPlayer 前身 FantasticMusicPlayer 的思路）：既然列表每帧都要整块重绘，
不如自己接管列表的绘制。**同日用户修订为**：「**不是**一次性绘制整个列表，而是
**只提供一个和视口大小一样的缓冲区，按照位置只绘制需要的项目**；列表的**坐标和惯性
计算之类的东西可以放在 IRAM 里**，同时**缓存小图标**。」
报告 §5.2.1 已按修订版重写完整评估，要点：

- **推荐实现形式**：**LVGL 的自定义绘制回调**（`LV_EVENT_DRAW_MAIN` +
  `lv_event_get_layer()` + `lv_draw_label()` / `lv_draw_image()`，全部是 9.4 公开 API），
  直接画进 LVGL 当前的绘制缓冲。
  ⚠ 这条很关键：若另建一块 PSRAM 的"视口缓冲"，每帧要多搬运 360 KB
  （**额外 7~9 ms，是现在没有的开销**），正好抵消掉收益。
- **砍掉什么**：每帧被 LVGL 遍历 / 派发事件 / 解析样式的对象从 **19 个降到 1 个**
  （→ 消掉 H3 对象/样式开销与 H5 管线开销）；样式链完全不解析（`dsc` 里直接填 font/color）。
- **没砍掉什么**：**每帧仍要画 ≈126 个汉字**（H1 重复光栅化、H2 mask blend 照旧）。
  这与"长条 surface + 跨帧复用"是本质区别——修订版用"实现简单"换掉了"字形零重画"。
- **手感**：完全保留 LVGL 原生滚动 / 惯性 / 滚动条（不需要 S1 的自管滚动，不触碰整行吸附）。
- **用户要求的两项**：① 坐标 / 惯性 / 行索引等热数据用
  `heap_caps_malloc(MALLOC_CAP_INTERNAL)` 显式放内部 RAM——这套方案下 LVGL 对象只剩 1 个，
  **路径B 的"钉 19 个行槽对象"机制不再需要，天然被取代**；
  ② 图标预渲染成 4 张小位图（`LV_SYMBOL_AUDIO` / `_DIRECTORY` / `_LIST` / `_SD_CARD`，
  20×20×2B×4 ≈ 3.2 KB，可整体放内部 RAM），绘制时 `lv_draw_image` 直接 blit，不再走字形路径。
- **代价**：**跑马灯要自己按 offset 实现**（最大工作量，含复刻 `global_style` 的
  "起始延迟 / 3 轮 / 轮间停顿"节奏）；文本宽度要自己调 `lv_text_get_size_attributes()`
  （这一项仍走 FreeType 度量，属 H1 残余）；点击命中改为按 y 坐标算行号；按下黄底自己画。
  切歌高亮反而变简单——draw 时按当前曲目判断即可，不再需要 `virtual_list_refresh()`
  重绑全部可见行。
- ⚠ **实施前仍要先做第 0 阶段测量**：它砍的是 H3/H5、不砍 H1/H2，
  所以"值不值得写"完全取决于底座归因——底座若是对象/样式开销 → 它能拿大头；
  若是字形渲染 → 收益有限，只能上 SIMD（S3）或接受每帧重画。

**⛔ 已严正否决：长条 / 跨帧复用的离屏内容缓存（即 S4 的最初设想）**

2026-10-16 用户：「长条方案应该严正否决，**因为存在单条 1000+ 的列表，长条必然会爆炸**」。
理由两条：

1. **产品约束**：本项目存在**单条 1000+ 项**的列表（大曲库、根目录）。长条要真正兑现
   "滚动时不重画字形"的价值，就得缓存**足以覆盖滚动范围**的内容 → 必然按 `item_count`
   分配 → 1000+ 行必然爆炸。
2. **技术印证**：若把它缩成固定窗口（初稿设想 12 行 / ≈618 KB PSRAM），它其实**退化**了
   ——ClickWheel 行对齐步进下单帧最多跨 **14 行**（`CW_MAX_STEP_PX=800 / item_h 56`），
   会直接冲垮 12 行窗口并触发"基准跳跃 + 整块重画"，于是又回到"每帧重画大部分行"，
   与视口重绘（修订版 S4）没有实质差别，却白背一套状态机。
   ⇒ **长条在本项目列表规模上没有存在价值，以后不要再评估。**

由此提炼出**约束四（报告 §2.6）：任何方案的资源占用都必须与 `item_count` 无关（O(1)）**。
✅ **已逐行核对：当前 `virtual_list` 满足这条**——所有分配都基于 `row_count`
（= 可见数 + 2 = 9），`item_count` 只参与边界判断和占位高度这一个标量。
**这是本项目虚拟列表最有价值的既有设计，任何改造（含 S4）都必须原样保持。**

**体感观测（2026-10-16，用户）**：「帧率忽高忽低，单帧时间有剧烈波动，实际体验较卡；
能忍，但希望改善。」→ 已记录为报告 §2.5。**"波动"比"均值低"更容易被读成卡顿**，
候选来源见该节；其中最值得先查的是 **Core1 争抢 PSRAM 带宽**（MP3 解码持续读写 PSRAM，
封面解码是突发大流量）——若成立，则第 0 阶段的测量必须**固定在同一个播放状态下**做，
否则数据不可比（也可能正是"快速 15 / 慢速 20+"这组历史数据本身不稳的原因）。

**当前状态（2026-10-16 用户决策）**：**保持现状，不做实施。**
本条只登记调查结论、方案清单与 S4 这个长期候选，等有需要时再从第 0 阶段往下走。

**明确不建议**（避免重复劳动）：**长条 / 跨帧复用的离屏内容缓存（见上，已严正否决）**、
继续加大对象钉住窗口（§13.2 已证明 83 KB 后收益归零）、
关闭 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM`、把字形位图整体钉进内部 RAM、缩小绘制缓冲。

---

## 47.1 2026-11 更新：S2' 视口画布已在实验分支实现并实测

> 实验分支 **`listview_perf_improvement`**（基线 `f73d3b2`）。完整材料都在该分支上：
> `ROUND_REVIEW_LISTVIEW_PERF.md`（本轮完整审阅：规整性代价 + 可回灌清单 + 残留风险）、
> `VLIST_CANVAS_S2P_IMPL.md`（画布设计/实测/踩坑）、`DISPLAY_FREEZE_TRIAGE.md`（定格取证链）。

**做了什么**：把"§1 的核心结论"直接实施了一遍 —— 自己维护一块视口大小的 RGB565 画布
（PSRAM，430×370，310 KB），滚动只改一个环形偏移（不搬内存）、一帧只重画新露出的那条带，
再在 `LV_EVENT_DRAW_MAIN` 里 blit 进 LVGL 的绘制缓冲；行内控件用
`virtual_list_set_content_hidden()` 隐藏。配套做了三件与画布无关的渲染层优化
（背景透明化、卡片拆"圆角帽+直角主体"、QSPI 40→80 MHz）和一套帧分析埋点。

**结果**：歌曲列表滚动 **6 fps → 21 fps**（单帧 63~90 ms → 43~47 ms），静止时几乎零渲染；
但代价是 **1.6k 行新代码 + 自实现文本排版 + 两套渲染路径并存 + 4 条靠"记得"的隐式契约**
（详见 ROUND_REVIEW 的 §2）。**结论：这是"用规整性换帧率"的典型，画布代码留在分支观察，
不回灌主干。**

**已回灌主干（2026-11，3 项，全部与画布无关）**：

| 项 | 收益 | 记录位置 |
|---|---|---|
| LVGL tick 改 `lv_tick_set_cb()` 读硬件时间 | 修掉**整屏定格**的真 bug（IDF 6.1 esp_timer 任务派发通道丢闹钟）+ 省 500 次/秒中断 | `DISPLAY_FREEZE_TRIAGE.md` |
| 跑马灯滚动期统一暂停（label 登记表） | 跑马灯动画每帧让整块视口失效，与"滚动整块重绘"叠加 → 一帧画多次；实测滚动帧 38 → 20 ms 量级 | `MARQUEE_STYLE.md` §9 |
| 背景透明化 + 卡片拆"圆角帽/直角主体" | 每帧少填 4 层整块背景、列表区不再走圆角掩码路径（draw 约 -6 ms/帧） | `main_ui.c` / `list_common.c` 注释、AGENTS.md |

**试过但回退的（重要，别再走一遍）**：**QSPI 像素时钟 40 → 80 MHz**。
在实验分支上它把 render 从 27 → 24 ms（所有界面受益），但在**回灌主干后的连续实机上
暴露了"偶发 1px 纯黑竖线"残留**：被局部重绘的区域左边缘多出一条贯穿该区域高度的黑线，
且随对象**宽度变化**累积、变宽时被一起清掉；连 LVGL 自带的 FPS 监视器、进度条时长标签
都有。**判定方法（已实测确认）**：同一份固件只把 `AMOLED_QSPI_MAX_PCLK` 退回 40 MHz，
全部黑线立刻消失；把整份改动用 `git stash` 退回干净 master 也能复现"无黑线"。
⇒ 结论是**这块屏在本机走线上 80 MHz 处于传输时序边际**，为 3 ms render 不值得。
排查"画面残留/黑线"类问题，第一步先把 QSPI 退回 40 MHz 做对照。详见 AGENTS.md 的 QSPI 条目。

**仍在分支上、未回灌的**：`core/ui/vlist_canvas.c/.h`、`playlist_page.c` 的画布接入、
`virtual_list` 为画布扩的 6 个 API、画布版跑马灯、`CONFIG_UI_FRAME_PROFILE` 帧分析埋点
（`FPROF/FPSCROLL/FPBAND/FPREND/FPLINK/FPSTALL`）与构建期补丁 `patches/lvgl_refr_prof/`。
**要做下一步性能工作，第一件事就是从分支取回那套埋点**（它是唯一能拆
"draw / flush / blocks / 真实帧间隔"的手段，且 release 下编译为空）。

**本轮产生的新结论（替代/补充本条第 0 阶段的目的）**：

1. **瓶颈归因已经完成**（等价于原第 0 阶段）：滚动帧 `handle ≈ 43 ms` 拆开是
   `render 23 ms`（对象树遍历 + 画像素 17.5 ms + 字节交换/提交 5.5 ms）+ `timer 20 ms`，
   而 **约 34 ms 是"重画整个视口"这个物理必需项**。所以 30 fps（单帧 32 ms）在不改变
   "每帧整屏重绘"的前提下**不可能达成**——这与本条第 3 条结论一致，且已从"推断"变成"实测"。
2. **已证否、不要再试**（每条都有实测依据，详见 VLIST_CANVAS §4.6 与 §5.1）：
   只失效"新露出的那条带"（画面是错的，面板无显存滚动）、自己写 RGB565 字节交换
   （慢 30%，LVGL 那份在 IRAM）、滚动后催 `lv_refr_now()`（blit 量翻倍、更差）、
   把卡片整块圆角（掩码路径没那么贵，重复背景层才是大头）、
   **QSPI 80 MHz（会残留 1px 黑竖线，见上"试过但回退的"）**。
3. **`lv_draw_label` 的适用边界**：在**事件上下文**（非刷新循环内）同步渲染时，
   它会经 `lv_draw_dispatch_wait_for_request()` 卡到下一次刷新（33 ms）且任务可能被丢弃
   （文字**消失**）。要往自己的内存上画字必须逐字形混合
   （`lv_font_get_glyph_bitmap` + 手动 alpha blend + `lv_font_glyph_release_draw_data`），
   且 FreeType 会忽略传入的 `draw_buf`。
4. **仍未做的下一步**（按性价比，都在分支基础上继续）：减少绘制块数量（8 → 4 块，
   要动绘制缓冲与单/双缓冲权衡）、让列表区只剩一次整块填充、
   用 `lv_obj_scroll_by_raw()` 绕开 `lv_obj_add_state/remove_state(SCROLLED)` 的样式比对。

## 48. 拖动类控件被 100ms 延迟派发限速（legacy_s3 已修，master 待同步）

> 来源：2026-11-11 在 `legacy_s3` 上排查"下拉控制中心拖亮度条一顿一顿"时定位。
> **修法已在 `legacy_s3` 落地并真机验证**（commit `修复：亮度/音量竖条拖动只有 10Hz 刷新`），
> master 尚未改；两条线的 `ui/notification_panel.c` 这段代码目前几乎相同，可以直接照搬。

### 现象与误判

拖亮度/音量竖条时"画面一顿一顿（掉帧感）"，看起来像性能不够。实测（临时探针）却显示：

| 指标 | 实测 | 结论 |
|---|---|---|
| LVGL 触摸事件间隔 | 32~35 ms（≈30Hz），>60ms 的次数为 0 | 输入没被卡住 |
| "应用亮度"耗时（写内存 + QSPI 0x51 亮度命令） | 50~230 µs | 硬件命令不慢 |
| 单块刷屏传输 | 1.3~2.4 ms | QSPI 不是瓶颈 |
| LVGL 对象位置 | `scr` 在内部 RAM | 与"对象进 PSRAM"无关 |

**真正的根因**：`config_store` 的变更通知是**100ms 延迟派发**的（`CONFIG_SLOTS.md` §七 bug B 的
修法：`set_xxx` 只置标志，由 `ui/app.c` 的 `lv_timer` 在 LVGL 任务里统一派发）。
而 `notif_apply_brightness()` / `notif_apply_volume()` 只"写内存 + 写硬件"，
**竖条本身由 100ms 后的 `notif_config_changed_cb` 才更新** → 手指 30Hz 移动、进度条 10Hz 跳格。
v0.9 时代 `set_xxx` 同步回调，所以没有这个问题；v1.0.0.0 引入延迟派发后出现，
**与 LVGL 8.4/9.4、IDF 5.5/6.1 的版本组合无关**（所以"回退版本"修不好它）。

### 修法（master 可直接照搬）

`ui/notification_panel.c`：在改完内存与硬件后，**立即刷新本模块自己的控件**——

```c
config_store_set_brightness_deferred(brightness);
display_set_brightness(brightness);
notif_refresh_brightness_ui(brightness);   /* ← 新增：拖动即时反馈 */
```

音量同理：`audio_set_volume_level()` → `config_store_set_volume_deferred()` →
`notif_refresh_volume_ui(level)`。事件回调本来就在 LVGL 任务上下文、已持锁，直接改本地控件安全；
100ms 派发那一路照旧保留（值未变则不产生重绘，幂等）。

### 为什么不是"把延迟派发改回同步"

会复现 §七 bug B：`player_ctrl` 任务调 `set_xxx` → 回调里 `lv_label_set_text` 在非 LVGL
任务里改对象树 → 卡死 `lv_inv_area` + task_wdt。**延迟派发必须留着，只是不能让它管
"拖动时的本地反馈"。**

### 通用规则（写新控件时照着办）

1. **拖动/连续调节的控件**（竖条、滑块、进度条的手指反馈）**不能**依赖延迟派发，
   必须在事件回调里就地刷新自己；
2. 延迟派发只负责**其它模块**的显示同步（状态栏音量、设置页文字等），100ms 够用；
3. 判断"是不是被限速了"：比对**事件间隔**（≈33ms 正常）与**画面实际更新次数**——
   对不上就是刷新被合并了，不是性能不够。

### master 侧还要检查的地方

- `ui/now_playing.c` 的进度条 seek 走的是 `lv_slider` 自身拖动，不经过 config_store 回调，
  应该没有这个问题，但**建议同样用探针确认一次**；
- 任何新加的"拖一下就能看到反馈"的控件（未来的音量条/亮度条改造、歌词页拖动等）
  都要套用上面的规则；
- 若以后真的需要 <100ms 的全局通知（例如多个模块都要跟手），可以另开一条**专门给 UI
  的快速通道**（LVGL 定时器 16~33ms + 只通知"拖动中的临时值"），但**不要**把
  `config_store` 的持久化通知提到那个频率（NVS 写入见 `NVS_WRITE_BUDGET.md`）。

## 49. 睡眠定时器自动开启 bug + 睡眠定时与配置槽位解耦（legacy_s3 已修，master 待同步）

> 来源：2026-11-11 用户反馈"设置里已经关闭『开机默认开启睡眠定时器』，但每次开机睡眠
> 定时器仍会自动开启，可能是多配置切换的地方给开的"。
> **已在 `legacy_s3` 修复并真机验证**（commit `4eb04c6`，文档记录见该分支
> `CONFIG_SLOTS.md` §九）。**master 当前代码与修复前完全一致**：
> `main/main.c` 开机的 else 分支、`config_slots.c` 第 6 步、`config_store.c` 里
> `sleep_default_min` 属于 `s_live` 与槽位 blob —— 可以直接照搬同一套改法。

### 现象

1. 设置 →「开机默认开启睡眠定时器」**关**，重启后睡眠定时器照样在倒计时
   （下拉控制中心睡眠开关亮、设置页睡眠格子显示时长），到点会伪关机；
2. 切一次配置槽位 SAVE1~SAVE4，也会把定时按该槽位记的时长重新开起来。

### 根因

睡眠定时的**时长** `sleep_default_min` 被放进"配置槽位字段"（`s_live` + 槽位 blob），
于是"记忆的时长"在两条自动路径上被当成了"恢复定时的指令"：

- `main.c`：`sleep_timer_boot_autostart_min()` 返回 0（开关关闭）时，else 分支仍
  `sleep_timer_set(config_store_get_sleep_default_min())`；
- `config_slots_switch()`：无条件 `sleep_timer_set(槽位时长)`。

语义上它是**全局档位**——"下次开启时用多久"，而不是"这套配置下定时必须开着"。

### 修法（三处，缺一不可）

1. **`main/main.c`**：删掉 else 分支，只保留
   `sleep_timer_boot_autostart_min() > 0 → sleep_timer_set(...)`；开关关闭时打一条
   `boot: sleep timer off (开机默认开启已关闭)` 即可。
2. **`core/storage/config_slots.c`**：删掉第 6 步的 `sleep_timer_set(...)`——切槽位
   **完全不碰**睡眠定时（不 set、也不 cancel），只留一条日志：
   `sleep timer untouched by slot switch (def_on=…, %u min, active=…)`；
   同时把 `switched to SAVE%u: … sleep=%u min …` 日志里的 `sleep=` 去掉。
3. **`core/storage/config_store.c/.h`**：把 `sleep_default_min` 从可切换字段挪到全局：
   - `config_live_t` 删掉该成员（含初始化器），新增
     `static uint32_t s_sleep_default_min = CONFIG_STORE_SLEEP_DEFAULT_MIN;`；
   - `config_store_set_sleep_default_min()` 改 `cfg_write_u32(KEY_SLEEP_DEFAULT, minutes)`
     （全局键 `sleepdef`），**不再** `slot_persist_live()`；
   - `slot_pack` / `slot_unpack` / `slot_apply_defaults` / `slot_load_into_live` 的日志、
     `config_store_slot_snapshot_t` 的字段、`config_store_init` 的读取处全部改用全局变量；
   - **槽位 blob 里原字段改名 `reserved_sleep` 占位**（仍是 4 字节）：布局/大小/
     `SLOT_BLOB_VERSION` 不变 → 老槽位 blob 照常读出、该值被忽略，**升级不丢槽位配置**；
     不要为此 bump 版本号（会把用户 4 个槽位全清空）。
   - `slot_persist_live()` 兼容分支里的 `cfg_set_u32(KEY_SLEEP_DEFAULT, …)` 一并删掉
     （setter 已直接写全局键）。

### 改完后的语义

- 睡眠定时只由三处改变：设置页睡眠格子、通知栏睡眠开关、开机默认开启；
- 切换 SAVE1~SAVE4 既不改变时长/开关，也不重新开表——正在跑的定时剩余时间不变，
  没开的定时不会被切配置打开；
- 开机只在 `sleep_def_on = 1` 时按全局记忆的时长启动。

### 顺带确认（不需要改，但别踩）

息屏超时 `screen_off_s` **本来就是全局键**（`screenoff`，`s_screen_off`，不在槽位 blob 里），
切槽位不会改变它。master 同样如此；以后"整理槽位字段"时不要把它误加进槽位。

### 验证方式（legacy_s3 实测，master 照此复测）

- 空 NVS：`config loaded: … sleep_def=45 min def_on=0 …` → `boot: sleep timer off`；
- 切槽位：`sleep timer untouched by slot switch (def_on=0, 45 min, active=0)`；
- 槽位保存日志 `SAVE N saved to NVS: …` 里不再出现 `sleep=`；
- 老槽位兼容：升级后开机仍是 `slot=N(saved)`，槽位弹窗里列表名/歌曲都在；
- 开关打开 + 时长 45 → 开机 `boot: sleep timer auto started (45 min)`。

### master 侧注意

- master 的 `config_store.c` 结构（`config_live_t` / `dict blob`）与 legacy_s3 基本一致，
  按上面第 3 条等价替换即可；只动 sleep 相关行，别动其它字段顺序。
- 改完同步文档：`CONFIG_SLOTS.md`（数据模型 / 运行时应用 / 验证 / §九）、
  `config_store.h` 配置项说明、`config_slots.h` 注释、`CHANGELOG`。



