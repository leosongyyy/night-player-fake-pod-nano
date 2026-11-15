# EQ 均衡器实现记录（NightPlayer .eq 兼容）

> 📅 本文记录 **S3 时代（1.x / 2026-09）**的实现与实测；DSP/解析/持久化部分仍然有效，
> 但"亮屏 240MHz"与"设置页第 7 格"是当时状态——S31 亮屏 320MHz，EQ 已移到九宫格第 4 格，
> 现状见 AGENTS.md 与 `CONFIG_SLOTS.md` §4.1。
> ⚠ 2026-11 多格式播放改造后，`mp3_out` 任务与 PCM ringbuf 已移除；EQ 仍在
> `audio_pcm_write` 内执行，但调用者是单任务 `mp3_play`（见 `MULTI_FORMAT_AUDIO.md`）。

> 对应 ideas_and_exps/FUTURE_PLAN.md 第 10 点（2026-09-03 完成）。
> 本文记录实现思路、关键设计取舍与踩坑，供后续维护/移植参考。

## 背景

FakePodNano 播放器的音频链路原本只有一道"回放增益"（16.16 定点整数乘法）。
本次为它加上 **10 段浮点 PeakEQ 级联 + 预增益**，读取 SD 卡 `/sdcard/.musicfx/*.eq`
预设（**与 NightPlayer 原版文件格式兼容**），并把"读取 .eq 得到的参数数据"一并存入
NVS，开机直接恢复 DSP 状态（不依赖卡上文件仍在）。

DSP 参考实现：Desktop/Ctyx（床头音响工程）的 `biquad_filter.{c,h}`——已在 ESP32
经典款上实现 10 个浮点 PeakEQ 级联并流畅处理 44100×2 数据。

## .eq 文件格式（NightPlayer 兼容）

文件存放于 SD 卡根目录的 `.musicfx` 隐藏文件夹，扩展名 `.eq`：

```
0.6                                ← 第一行：预增益（0~1 之间的**线性**系数，非 dB）
153,1.672240803,-13                ← 之后每行一段 PeakEQ：fCenter, fQ 的倒数, dBGain
1169,1.483679525,-8.3
...
```

校验规则（`eq_parse_file`）：
- 预增益行：整行一个数值 token，∈ [0,1]；
- 参数行：逗号**恰好 2 个**，切出的三段各自必须是合法数值 token（字符白名单
  `0-9 . - + e E` + `strtof` 完整消费校验，拦截 inf/nan/字母/中文），且
  - fCenter ∈ [16, 20000] Hz
  - fQ 的倒数 ∈ [0.2, 5]（即 Q = 1/倒数 ∈ [0.2, 5]）
  - dBGain ∈ [-15, +15]
- 空行/纯空白行忽略；任何一行非法 → **整个文件判无效**（返回对应错误码）；
- 参数段数 > 10 → 单独返回 `EQ_PARSE_ERR_TOO_MANY`（UI 弹"EQ 段数超过 10"）；
- 空文件 / 只有预增益没有参数段 → `EQ_PARSE_ERR_EMPTY`。

示例文件：Desktop/Ctyx 下的 `CTYX.eq`/`ZTYX.eq`/`ZTYX2.eq`、`Q:\MP3Player\.musicfx`。

## 代码结构

```
core/hardware/audio/eq.{c,h}       DSP（biquad 级联 + fs 系数）+ .eq 解析/扫描
core/hardware/audio/audio.c        audio_pcm_write 处理链宿主（每块 eq_snapshot + 逐样本处理）
core/hardware/audio/mp3_player.c   切歌静止点 eq_reset_state()
core/storage/config_store.{c,h}    eqname(str) + eqdata(eq_preset_t blob) NVS 持久化
ui/eq_popup.{c,h}                  设置页第 7 格弹出的 EQ 选择列表
ui/settings_page.c                 九宫格左下角「EQ」格（图标 EQ + 状态文字）
main.c                             开机从 NVS 恢复 EQ 并 eq_apply
```

- `eq_preset_t`：`.eq` 解析结果（段参数，纯数据，固定 128B 布局，NVS blob 直存）；
- `eq_runtime_t`：DSP 运行时（系数 + 直接型 II 状态 + `ver` 版本号）。

## DSP 处理链

EQ 挂在 **consumer（mp3_out 任务，Core1）写 I2S 前**（`audio_pcm_write` 内部）——
ringbuf 里存的是未处理 int16，EQ 只作用于最终输出流。统一浮点路径
（**EQ 关闭也走浮点**，用户要求，顺带给原回放增益补上 Clamp）：

```
int16 → float(÷32768 归一化)
      → [×预增益 → 段级联（band_count>0 时；关闭时直通）]
      → ×回放增益（q16 定点 ÷65536 → 线性 float；无符号增益 × 有符号样本
        统一在 float 域完成转换）
      → Clamp 到 int32 值域
      → ×2^31 左对齐 32bit slot（int16 内容 <<16 的 float 等价式）
```

- 输出缩放 `×2147483648.0f`（2 的幂，float 精确）；Clamp 上限 INT32_MAX
  （float ≥ 2^31 的先钳再转，避免 float→int 越界 UB，也取代旧整数路径
  "增益>1 自然回绕"的爆音）。
- 单样本处理 `eq_process_sample`/`eq_biquad_process` 为 header `static inline`：
  样本循环无函数调用开销（-O2 下内联展开），运行期纯 float 乘加。
- 系数（RBJ Peaking EQ，double 中间量算一次存 float）只在两个低频点计算：
  EQ 应用、采样率变化。`fs == 0`（尚未配置采样率）时只存参数、系数留待首次
  `audio_configure_fs` 生成。

## 线程安全与切换时机

写方（Core0：UI 切 EQ / main.c 开机恢复）与读方（Core1：consumer 每块处理）
并发，设计如下：

| 事件 | 位置 | 说明 |
|---|---|---|
| 参数切换 | UI 线程 `eq_apply()` | 锁外按当前采样率预计算系数（pow/cos/sin 不进临界区），临界区内整体替换 `s_preset/s_rt` + `ver++`。播放中切换**立即生效** |
| 采样率变化 | `audio_configure_fs` → `eq_notify_sample_rate(fs)` | consumer 未启动（静止期）；fs 未变则幂等跳过；变化才重算系数 + `ver++` |
| 切歌清记忆 | `mp3_player_play` 静止点（与应用回放增益同一处）→ `eq_reset_state()` | 只清滤波记忆（参数不变），`ver++` 让 consumer 下块从零状态起振 |

### 为什么 EQ 参数不走"回放增益式 pending"？

`mp3_player_set_gain` 的 pending（暂存 → 旧曲停止 + ringbuf 清空后才套用）针对的是
**逐曲变化的新歌属性**——若提前套用，旧曲 ringbuf 残留（最多 ~400ms）会以"新歌增益"
播出。EQ 是**用户手动选择的全局设置**：播放中切换期望"从此刻起整个输出流都是新 EQ"，
ringbuf 残留用新 EQ 播出正是"立即生效"语义；且立即生效本身通过"临界区整体替换 +
状态清零"做得干净（无撕裂、无旧状态混入新系数）。此取舍已与用户确认保持现状。

### ⚠ 踩坑：每块快照覆盖滤波器状态 → 周期性瞬态爆音

初版 `eq_snapshot` 每处理块全量 `memcpy` 内部模板到 consumer 快照。问题：
consumer 处理时滤波器状态只写在本地快照里演化，而内部模板的状态**恒为 0**，
于是每块开头的 memcpy 把状态覆盖回 0 → 滤波器每块（约 10~24ms）从零状态
"冷启动"，低频段/高 Q 段记忆反复被打断 → 实测开启 EQ 后**每秒十几个 -30dB 微爆音**。

修复：`eq_runtime_t` 增加 `ver` 参数版本：

```c
void eq_snapshot(eq_runtime_t *out) {
    taskENTER_CRITICAL(&s_mux);
    if (out->ver != s_rt.ver) {   /* 版本一致 → 什么都不做（状态跨块延续） */
        *out = s_rt;              /* 落后 → 新参数/系数 + 状态清零（模板状态恒 0） */
    }
    taskEXIT_CRITICAL(&s_mux);
}
```

`eq_apply` / 采样率变化 / `eq_reset_state` 各自 `ver++`。修复后稳态每块只多一次 4B
比较（比初版每块 ~720B memcpy 还快），爆音消失，播放中切 EQ / 切歌 / 暂停恢复 /
采样率切换衔接均干净（实测确认）。

## 采样率

MP3 仅支持 32k/44.1k/48k（Helix + CS43131 ASP_SPRATE）。系数与 fs 相关：
`audio_configure_fs` 每次输出启动/采样率变化都会调 `eq_notify_sample_rate`，
内部先比较 `s_fs` 幂等跳过（暂停恢复不重算）。

## NVS 持久化

`config_store` 两个新键（命名空间 `config`）：
- `eqname`：.eq 文件名（str，≤ EQ_FILE_NAME_MAX=63）；
- `eqdata`：`eq_preset_t` 二进制 blob（128B，无指针纯数据，整块直存）。

开机 `main.c`（audio_init 之后）读回并 `eq_apply`。UI 变更经 `config_store_set_eq`
通知设置页 EQ 格状态文字刷新。

## UI（设置页第 7 格 = 九宫格左下角）

- 格子：图标文字 "EQ"，状态行显示当前 .eq 文件名或"无 EQ"；
- 点击弹窗（`ui/eq_popup.c`，挂设置页 tab 上、翻页随页移出）：
  - 行 0：无 EQ（关闭）；之后为扫描到的 .eq 文件名（当前生效项黄色高亮）；
  - 点文件 → 同步解析（文件 KB 级，毫秒内）→ 成功：`eq_apply` + 持久化 + Toast
    「已应用 EQ」并关弹窗；失败按错误类型 Toast（超过 10 段 / 内容无效 / 空 / 打不开），
    弹窗保持打开供换选；
- 无 .musicfx 目录/无 .eq 文件时列表只剩"无 EQ"行 + 灰色提示行。

## CPU 实测

- EQ 关闭（浮点路径 + Clamp）：亮屏 240MHz 播放 cpu1 ≈ 20~25%（与旧整数路径相当）；
- 开启 10 段 EQ（48k 320kbps）：consumer 增量来自每样本 10 段级联 float 运算，
  实测 cpu1 涨幅约数个~十几个百分点，**240MHz 与 80MHz（息屏）均流畅无卡顿**；
  具体读数以 `mem_monitor`（CONFIG_PERFORMANCE_MONITOR）串口输出为准。

## 限制与后续

- 与回放增益同链路串联：预增益 → 级联 → 回放增益（元数据）→ Clamp；
- 若未来支持 22.05k/24k 采样率（软件重采样），系数重算已按 fs 泛化，直接可用；
- 切换 EQ 的入口目前仅设置页第 7 格；如需在设置菜单（lv_menu）加"均衡器"子页，
  可直接复用 `eq_scan_files`/`eq_parse_named`/`eq_apply`。
