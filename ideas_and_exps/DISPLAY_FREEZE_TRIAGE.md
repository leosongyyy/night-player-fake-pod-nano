# 整屏定格（"LVGL 冻结"）排查记录 —— 已定位并修复

> **本文档是 2026-11 在实验分支 `listview_perf_improvement` 上做列表滚动性能优化时，
> 顺带抓到并修掉的一个真 bug 的完整取证链。修复已回灌 `master`（只回灌了修复与本文，
> 见 §7）。**
>
> 现象：**偶尔整屏完全定格**，像截了张图；触摸/按键/音频都正常（按键调音量、切歌
> 能听到效果），**息屏再亮屏后恢复**；串口日志里没有任何 assert / WDT / 错误。
>
> **结论：不是死锁，是 LVGL 的 tick 冻结了。根因在 IDF 6.1 的 esp_timer：
> "任务派发"与"中断派发"两类定时器混用时，任务派发那条通道的闹钟会被丢掉，
> 该通道上的定时器从此永不到期。而 LVGL 的 tick 原来正是"esp_timer 每 2 ms
> 回调一次 → lv_tick_inc(2)"实现的，于是 LVGL 所有 timer 一起停摆。
> 修复：tick 改为 `lv_tick_set_cb()` 直接读硬件时间，一个 esp_timer 都不用。**

---

## 0. 为什么这个 bug 值得记住

它**不是**画布（`vlist_canvas`）带来的：触发条件在优化之前的固件里就已完全具备 ——

- 触发条件①：`lvgl_port.c` 用 `esp_timer` **任务派发**喂 LVGL tick（2 ms，500 次/秒）；
- 触发条件②：`performance_monitor.c` 有一个 **`ESP_TIMER_ISR`（中断派发）**的
  500 ms CPU 频率采样定时器。

两条同时存在就够，与绘制路径无关。**这就是它被回灌进 `master` 的原因。**

---

## 1. 先排除"LVGL 卡死/死锁"

用户的第一猜想是"lvgl 是不是哪里锁起来了"。日志可以直接否掉这一类：

```
I (116927) lvgl_port: FPROF: n=388 draw=0/388 | frame=0/0/0/0 ms | handle=2 timer=2 render=0 flush=0 ms
I (118946) lvgl_port: FPROF: n=387 draw=0/387 | frame=0/0/0/0 ms | handle=2 timer=2 render=0 flush=0 ms
```

> ⚠ 上面这两行来自实验分支的帧分析埋点（`CONFIG_UI_FRAME_PROFILE` + 构建期补丁
> `patches/lvgl_refr_prof`），**`master` 上没有这套设施**（见 §7）。

字段含义：

| 字段 | 含义 |
|---|---|
| `n` | 本窗口内 `lv_timer_handler()` 的调用次数 |
| `draw=X/n` | 其中触发了 `LV_EVENT_RENDER_START` 的次数（= 真正发生重绘的帧数） |
| `frame=avg/p50/p90/max` | **两次重绘起点**之间的间隔（= 屏幕多久出一帧） |
| `handle/timer/render/flush` | `lv_timer_handler` 总耗时 / 其中 timer 阶段 / render 阶段 / flush 阶段 |

三条互证的推论：

1. **`n=388`、`handle=2 ms`** ⇒ 2 秒里 LVGL 任务跑了 388 圈、每圈 2 ms。
   如果卡在 `wait_for_flushing()` 的空转或 `lvgl_mux` 上，**这一行根本打不出来**
   —— `FPROF` 是 LVGL 任务自己在每圈结束时打的。所以任务活着、锁拿得到。
2. **`draw=0`** ⇒ `LV_EVENT_RENDER_START` 一次都没发。该事件是在 `lv_refr.c` 的
   `refr_invalid_areas()` 里、**先检查 `disp->inv_p`** 才发的
   （`if(disp_refr->inv_p == 0) return;`）。所以 `draw=0` 的物理含义是：
   **失效表是空的**，不是"画了没送出去"。
3. `FPBAND` 的 `n=0`、`FPSCROLL` 全 0 ⇒ 没有滚动、没有画布重画。

结论：定格期间 LVGL **完全空闲**，屏幕停在它最后一次画出来的那一帧上。

## 2. 再定位到"LVGL 时间冻住了"

新增 `FPLINK` 行（每 2 s）与 `FPSTALL` 看门狗后立刻复现，并且指认了 tick：

```
I (21394) FPLINK: ... | tick=476ms inact=12ms | tick_act=1 tickcb=237 probe=1 isr=4 | alarm=-1924521us
W (22820) FPSTALL: invalidation pending (inact=12ms) but no render for 3001ms (tick=19136ms, ...)
I (23419) FPLINK: ... | tick=0ms inact=12ms | tick_act=1 tickcb=0 probe=0 isr=4
E (71772) tick frozen (only 122ms in 2s): restart attempt 1 -> ESP_OK (active=1)
I (73837) FPLINK: ... | tick=2056ms ... tickcb=1032 probe=4 isr=4          ← restart 一按就恢复
```

| 字段 | 含义 | 判读 |
|---|---|---|
| `tick=` | 本窗口 `lv_tick_get()` 走了多少 ms（正常 ≈2000） | `0` ⇒ LVGL 时间冻结 |
| `inact=` | `lv_display_get_inactive_time()`，距上次失效 | 随 `tick` 一起冻住 ⇒ 同上 |
| `flush sub/done/err` | `flush_cb` 提交块数 / 传输完成中断数 / `esp_lcd_panel_draw_bitmap` 失败数 | `sub>done` ⇒ 传输丢了；`err>0` ⇒ 面板提交失败（原先该返回值被完全忽略） |
| `tick_act` | `esp_timer_is_active(tick 定时器)` | 冻住时仍为 1 ⇒ 定时器"还在"，不是被停掉 |
| `tickcb` | tick 的 esp_timer 回调次数（**任务派发**） | 归零 ⇒ 回调不再被派发 |
| `probe` | 另一个空的"任务派发"定时器回调次数 | 与 `tickcb` 同生共死 ⇒ 是**整条任务派发路径**停了 |
| `isr` | 一个"中断派发"（`ESP_TIMER_ISR`）定时器的回调次数 | 冻住时仍为 4/2s ⇒ systimer 中断本身正常 |
| `alarm` | `esp_timer_get_next_alarm() - now` | 变成大负数 ⇒ 闹钟时间落在过去却没被处理 |

**三个数字一起看，指向唯一结论：任务派发的 esp_timer 回调永久停止派发，
而中断派发的照常；`esp_timer_restart()` 能立刻救活。**

## 3. 根因（IDF 6.1 esp_timer）

`components/esp_timer/src/esp_timer_impl_common.c`：IDF 6.1 把定时器拆成
**两条独立的 systimer alarm 通道**——

```c
/* [0] - for ESP_TIMER_TASK alarms, [1] - for ESP_TIMER_ISR alarms. */
uint64_t timestamp_id[2] = { UINT64_MAX, UINT64_MAX };
```

而 ISR 收尾时用 `esp_timer_impl_try_to_set_next_alarm()` **靠比较两个 timestamp
来猜"这次中断是哪条通道触发的"**：

```c
if (timestamp_id[0] < timestamp_id[1]) { now_alarm_idx = 0; next_alarm_idx = 1; }
else                                  { now_alarm_idx = 1; next_alarm_idx = 0; }
if (timestamp_id[next_alarm_idx] != UINT64_MAX) {
    esp_timer_impl_set_alarm_id(UINT64_MAX, now_alarm_idx);   // 把"当前"通道的闹钟丢掉
}
```

猜错的后果就是**把任务派发通道的闹钟时间清成 `UINT64_MAX`**，该通道上的定时器
此后不再到期（`is_active` 仍是 1，`esp_timer_get_next_alarm()` 却显示时间早已过去）。

实测复现率很高：开机后 19 s / 21 s / 71 s / 207 s 各一次，界面一停手就更容易撞上。

## 4. 修复（已回灌 master）

**LVGL 的 tick 不再用 esp_timer 喂**，改用 LVGL 9 的 tick 回调直接取硬件时间
（`main/core/ui/lvgl_port.c` 的 `lvgl_port_tick_cb`）：

```c
lv_init();
lv_tick_set_cb(lvgl_port_tick_cb);   /* ⚠ 必须在 lv_init() 之后，见 §5 */
```

- 时间来自 systimer 计数（`esp_timer_get_time()`），**一个定时器都不用**，
  任务派发路径坏掉也与 LVGL 无关；
- 顺带省掉 500 次/秒的中断开销；
- 息屏"冻结 LVGL 时间"的原语义用 `s_tick_pause_start_us` / `s_tick_paused_total_us`
  保留：挂起时时间停在那一刻，恢复后从暂停点继续（不会补跑挂起期间到期的 timer）。

## 5. 第二个坑：`lv_tick_set_cb()` 必须在 `lv_init()` 之后

第一版把 `lv_tick_set_cb()` 写在 `lv_init()` 之前，结果**LVGL 完全不刷新**
（只在解锁/亮屏的 `lv_refr_now` 时刷一帧），日志里 `tick=0ms` 恒定：

```c
void lv_init(void) { ...
    LV_GLOBAL_INIT(LV_GLOBAL_DEFAULT());   /* lv_global_init() 里： */
    ...                                    /* lv_memzero(global, sizeof(lv_global_t)); */
```

`lv_init()` 会把整个 `lv_global_t` 清零，`tick_state.tick_get_cb` 被一起清掉 ⇒
`lv_tick_get()` 永远返回 `sys_time`，而没人再调 `lv_tick_inc()` ⇒ 恒为 0。

## 6. 验证

**实验分支**（带帧分析埋点）：修复后连续运行 148 s + 56 s 交互运行，反复切页/滚动
（`flush sub=300~450`/2s）：

```
FPLINK: ... | tick=2049ms inact=460ms
FPLINK: ... | tick=2068ms inact=129ms
FPLINK: ... | tick=2052ms inact=88ms      ← 全程 tick ≈ 2000，无 FPSTALL、无定格
```

LVGL 任务栈高水位 9132~10012 B（上限 20 KB），`sub == done`、`err=0`。

**回灌 master 后**（临时探针，每 5 s 打一次 `lv_tick`，测完已删除）：

```
W (5674)  lvgl_port: TICKPROBE: lv_tick=5371ms  delay=1ms
W (10704) lvgl_port: TICKPROBE: lv_tick=10402ms delay=0ms
W (20752) lvgl_port: TICKPROBE: lv_tick=20449ms delay=30ms   ← 30ms = 刷新周期空转
W (65850) lvgl_port: TICKPROBE: lv_tick=65548ms delay=8ms
```

`lv_tick` 与真实时间 1:1 推进（5 s 窗口内增长 ≈5000 ms），无 `draw_bitmap failed`、
无 task_wdt。

## 7. 未回灌的诊断设施（都在实验分支上）

下面这些是为了**做这次取证**才写的，没有回灌 `master`。以后遇到同类问题（画面不动、
帧率异常）时，需要重新从分支取用：

| 设施 | 位置（分支） | 作用 |
|---|---|---|
| `CONFIG_UI_FRAME_PROFILE` | `main/Kconfig.projbuild` | 帧分析埋点总开关，release 为 n（编译为空） |
| `FPROF` / `FPSCROLL` / `FPBAND` / `FPREND` | `main/core/ui/lvgl_port.c` | 单帧耗时分解（draw / flush / blocks）+ 真实帧间隔 + 滚动位移分档 |
| `FPLINK` / `FPSTALL` | `main/core/ui/lvgl_port.c` | 刷屏链路健康度（提交/完成/失败 + tick + 距上次失效）与整屏定格看门狗 |
| `FP_HEARTBEAT` | `main/core/ui/lvgl_port.c`（默认 0） | 屏幕右下角每秒翻转的小方块：一眼区分"链路活着"还是"断了" |
| `patches/lvgl_refr_prof/` | `patches/` | 给 `lv_refr.c` 插桩（draw/flush/blocks 分项计时）；**整文件回写**，升级 LVGL 时要人工搬插桩点 |

**结论性判据（可直接复用，不需要那些埋点）**：三条互证 ——
① "任务派发"定时器回调计数归零；② `ESP_TIMER_ISR` 派发的计数照常；
③ `esp_timer_is_active()` 仍为真、`esp_timer_get_next_alarm()` 显示闹钟落在过去；
④ `esp_timer_restart()` 立刻恢复。

---

## 8. 运维备注：COM 口被占用的处理

`idfc monitor` 的进程树挂在 **idfc 服务器**下面：客户端 job 被杀掉后，服务器侧的
`idf.py monitor` 可能还活着并**持续攥着 COM 口**，表现为烧录报
`could not open port 'COMx': PermissionError(13)`，而 `job_list` 里所有 monitor
都是 killed、`Get-Process` 也看不到 python 进程（沙箱只列出部分进程）。

处理：`idfc stop-server` 会 taskkill 整棵树，端口随即释放；
之后必须由**用户自己的普通权限终端**重新 `idfc start-server`（沙箱内启动会被
低完整性标签挡住，见 AGENTS.md 的 idfc 说明）。

---

> 关联：`ROUND_REVIEW_LISTVIEW_PERF.md`（本轮实验的完整审阅）、
> `VLIST_CANVAS_S2P_IMPL.md`（画布设计与实测）、`MARQUEE_STYLE.md` §9。
> 这三份都在实验分支 `listview_perf_improvement` 上。
