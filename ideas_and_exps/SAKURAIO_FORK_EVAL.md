# SAKURAIO_FORK_EVAL — 第三方 fork（多格式音频 / 自动曲库发现）真机实测记录

> 2026-11-15 实测。对象：`C:\Users\ZYFDroid\Desktop\night-player-fake-pod-nano_branch_sakuraio`
> （基线 `c0715df` = 接蓝牙前的最后一次提交，+3 个 commit：
> `123b383` 开机自动发现曲库与换卡识别 / `a793c57` MP3+FLAC+OGG 多格式与封面增强 /
> `7313a47` Tremor OGG + 80MHz 息屏 + 播放爆音修复）。
> **本文只记录"能不能跑、多大、稳态占多少"，不含合并方案。**

## 1. 结论摘要

| 项目 | fork（原样） | fork（+1 项配置） | master（HEAD，含 BT） |
|---|---|---|---|
| debug 固件 | 2,406,128 B | 2,405,632 B | 2,472,496 B |
| release 固件 | — | **2,722,448 B** | **2,791,024 B** |
| 能否启动 | ❌ `assert s_mux` 死循环 | ✅ | ✅ |
| 开机完成内部 RAM 余量 | — | 99,679 B | 48,511 B |
| 稳态 `MEM: iram=`（亮屏 320MHz 播放） | — | 106,663 B | 56,951 B |
| 稳态 `MEM: iram=`（息屏 80MHz 播放） | — | 106,351 B | 56,951 B |
| 息屏 80MHz 播放 cpu1 | — | **26~28%** | **43~52%** |

- fork 的"多格式解码 + 自动曲库"在**内部 RAM 上几乎零成本**：与 master（含 BT 控制器/host）比，
  稳态差 ~50KB，正好是蓝牙栈的量级；与"master 去掉 BT"的 ~107KB 基本一致。
- 但**原样不能启动**：开机自动扫描会在内部 RAM 上打出一个 ~50KB 的瞬时峰值，
  把 `app_main` 剩余初始化（`power_service_init` 的 FreeRTOS 互斥锁）挤失败。
- fork 缺的是 master 在接蓝牙时做的两项腾挪（`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`
  与主任务栈 8KB）——加上前者即可启动，后者未验证。

## 2. 原样启动失败（可复现）

```
I (1683) playlist_builder: start scanning /sdcard ...     ← 开机自动扫描一开始就跑
I (1713) MEMTRACE: 12-boot_flow+first_frame int 30151/ 15360(247) | dma 14547/ 14336
I (1769) MEMTRACE: 14-key_handler         int  7303/  7168(604) | dma    15/     0
I (1789) power_service: esp_pm ready: screen on=320 MHz, screen off=80 MHz
assert failed: power_service_init power_service.c:181 (s_mux)
```

- `power_service.c:180` 是 `s_mux = xSemaphoreCreateMutex(); assert(s_mux);`。
  `pvPortMalloc` 走 `heap_caps_malloc(INTERNAL|8BIT)`（IDF `freertos/heap_idf.c`），
  即 FreeRTOS 对象只能从内部 RAM 分配。
- MEMTRACE 显示 `12-boot_flow → 14-key_handler` 之间内部 RAM 掉了 ~23KB，
  而此刻**扫描任务正在并发跑**（`collect_dir()` 对每个条目 `strdup()`，
  默认 malloc → 优先内部 RAM；1233 首的扁平目录一次就要 ~50KB）。
  两个日志点相隔 20ms 就少了 7KB 量级，`s_mux` 拿到 0 是扫描把最后一块吃掉了。
- 佐证：同一份代码只把 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 改 `n`（+26.75KB）后能启动，
  但 `15-power_service` 只剩 **2,159 B**，紧接着 `startPerformanceMonitor()` 报
  `create mem_monitor task failed (internal free=19)` —— 仍是扫描峰值吃满。
- 第二次启动（pldata v3 缓存命中、跳过扫描）就完全正常：`15-power_service` 99,679 B，
  性能监视器正常输出。**即：崩溃的触发条件是"开机即扫描 + 大曲库"，
  不是解码器本身。**

> 归因提醒：本机曲库 = `dirs=173 songs=9042`（物理文件 5833，含 .pl 引用），
> 全量扫描 620,134 B 数据、~2s。fork 作者的小卡上大概率复现不出来。

## 3. 让 fork 跑起来的最小改动（仅用于测量）

`.forktest`（本仓库 worktree，`fork_sakuraio` 分支）里只改了一行：

```
# sdkconfig.defaults
-CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y
+CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n     # 与 master 接蓝牙时同一改动，+26.75KB 内部 RAM
```

`CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` 保持不变（master 是 8192，此处未验证需要）。

## 4. 运行实测

### 4.1 启动（缓存命中，无扫描）

```
I (2795) boot_flow: library probe: ESP_OK, sig=... files=5833 in 1276 ms
I (2795) boot_flow: playlist cache match: card=... files=5833
I (2915) player_ctrl: play #8009 [O.日乙音乐大串烧（？）.pl] 07 - In the morning.mp3
I (3120) boot_flow: enter normal UI
I (3229) MEMTRACE: 12-boot_flow+first_frame int 104107/ 39936(224)
I (3451) MEMTRACE: 15-power_service       int  99679/ 34816(228)
```

- 开机 3.1s 进主界面；库指纹探针 1,276ms（5833 个文件，`d_type` 遍历）。
- 首次（无缓存）扫描 2s 建库，把 9042 首写进 pldata。

### 4.2 稳态（`MEM:` 每 2s，性能监视器）

| 状态 | iram（内部 RAM 空闲） | cpu0 | cpu1 | freq | fps |
|---|---|---|---|---|---|
| 亮屏、未播放 | 116,311 | 4~7% | 7~8% | 320MHz | 62 |
| 亮屏、播放 MP3 | 106,663 | 4~5% | 8~9% | 320MHz | 61~62 |
| 亮屏、加载封面 | 98,467（瞬时） | — | — | 320MHz | 49~57 |
| 息屏、播放 MP3 | **106,351** | 1% | **26~28%** | 80MHz | 0 |

- 约 90s 观察期内 `iram` 除封面加载的 ~8KB 瞬时下降外**完全无漂移**（+0）。
- 任务栈高水位（`MEM: stk:`）：`mp3_play=47,232/64K`（PSRAM 栈，Tremor 需求）、
  `LVGL=15,096/20K`、`deep_worker=11,944/12K`、`cover_load=5,060/8K`。
- 息屏 80MHz 播放 cpu1 只有 26~28%（master 同场景 43~52%）：minimp3 单任务直写 I2S
  比 Helix + ringbuf + mp3_out 省一半 CPU。

### 4.3 master（HEAD）同机基线

```
I (2358) MEMTRACE: 14-key_handler   int  48723/ 31744(396)
I (2396) MEMTRACE: 15-power_service int  48511/ 31744(398)
I (5605) MEM: heap=16093491  iram=56951  cpu0=6% cpu1=11%  freq=320MHz  fps=62
I (33623) MEM: heap=16093491 iram=56951  cpu0=5% cpu1=49%  freq=80MHz   fps=26
```

- 注意 master 的 12→14 之间掉了 ~29KB（Helix 打开首曲时分配），
  fork 的 minimp3/dr_flac/Tremor 侧没有这个量级的内部 RAM 分配（OGG 走 1MB PSRAM arena）。

## 5. 固件体积

| 构建 | debug | release | 分区余量 |
|---|---|---|---|
| fork 原样 | 2,406,128 B (0x24BAF0) | —（改那一行后 2,722,448 B） | fork 分区表 factory=0x5F0000，debug 余 61% |
| master HEAD | 2,472,496 B (0x25BA30) | 2,791,024 B | factory=0x300000；release 余 ~354KB |
| fork + BT 估算 | — | ≈2.8~2.9MB（两份差集需单独测） | 3MB factory 上余量会压到 <10% |

- fork 的 release（含 WiFi 对时固件、含多格式解码、**不含** BT）2.72MB；
  master 的 release（**含** BT、**不含** WiFi，WiFi 已挪到 ota_0 双固件）2.79MB。
- 静态 DIRAM：fork release 用 177,972 / 503,744（35.3%），master release 用 186,184（37.0%）
  —— **fork 静态 RAM 反而更省 8KB**。
- 合并后体积必然叠加（BT + 多格式 + 丢掉 Helix、丢掉单固件 WiFi），
  3MB 的 factory 大概率吃紧，需要单独核（分区表只能动 factory↔font 之间的线，
  `pldata`@0xC00000 不能动）。

## 6. 其它实测观察（非结论）

- fork 的播放固件里**还带着 WiFi 对时**（`wifi_time_sync` 在 33s 触发过一次，
  WiFi 初始化瞬时吃掉 ~58KB 内部 RAM）；master 已改成双固件方案，合入时这部分要按 master 口径替换。
- fork 的 `power_service` 改动在本仓库语境下基本是**注释 + 频率缓存**，
  "息屏统一 80MHz / 去掉 160MHz 档"这句是针对它自己的 hello_world 测试固件说的，
  本仓库基线本来就是 80MHz——但 master 的 BT 播放需要 160MHz 档，合入时不能照抄。
- I2S DMA 加深到 8×511 帧（32,704 B）实测正常（`audio: I2S DMA: 8 x 511 frames`）；
  代价是 DMA 池占用 +21KB。

## 7. 极限场景复测（2026-11-15 下午，新测试卡）

新卡内容：`dirs=12 songs=134`（`covertest1` / `covertest2` / `fmttest` / `zanimusic`…），
含 FLAC、OGG、以及专门测封面的目录。开机缓存命中（未触发扫描），2.1s 进主界面。
固件同 §3（fork + `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`），约 4.5 分钟连续操作（人为换曲/翻封面/息屏）。

| 场景 | `iram=` 空闲 | cpu1 | 备注 |
|---|---|---|---|
| 开机完成（`app_main` 返回后） | 117,739 | 1% | 主任务 16KB 栈已释放 |
| FLAC 44.1k/2ch（320MHz） | 117,739 | **1~8%** | 11,184,343 帧 ≈ 253s，`FLAC stream opened` |
| OGG 48k/2ch（320MHz） | 117,527 | **11~19%** | `OGG arena ready: 1MB PSRAM`，Tremor 定点 |
| 封面 477×477 JPEG | —（不占内部 RAM） | 峰值 **42~48%** | HW 不收该尺寸 → 软解 **307~326ms** |
| 封面 460×460 JPEG | — | — | HW 解码 **16ms** |
| FLAC 息屏 80MHz | 116,243 | **19~21%** | |
| 全程最低值（4.5 分钟） | **115,663** | — | 单调缓降，见下 |

- **多格式解码对内部 RAM 依然是零成本**：FLAC 与 OGG 之间的 `iram` 只差 **212 B**，
  与 MP3 同量级；Tremor 的 1MB arena、FLAC 的读缓冲都不在内部 RAM。
- **真正的瓶颈是栈不是 RAM**：`mp3_play` 高水位 **62,256 / 65,536 B**（OGG 时达到），
  只剩 **3.2KB** 余量。Tremor 首次解码 codebook 的 `alloca` 再复杂一点就会溢出，
  而且栈在 PSRAM，溢出不会有干净的 canary 报错。→ 合并/加固时建议提到 80~96KB。
- **每换一首，内部 RAM 掉 ~116 B**（4.5 分钟 14 次事件累计 -1.8KB，无回升）。
  量级很小但单调，像是每曲一次的固定分配没释放（TCB/字符串/解码器小对象），
  值得单独查；按 1000 首估算会到 ~116KB，长播需要重启才回收。
- 同期 PSRAM 从 16.68MB 降到 14.71MB（-1.97MB）：1MB OGG arena + 封面像素缓冲，
  属预期，退出播放后未见泄漏。
- 无 panic / 无 `allocate_dma_buf` 失败 / 无坏曲跳过 / 无 underrun。
- 两个封面链路的实测限制（与 RAM 无关但影响体验）：
  ① **477×477 这类非 HW 友好尺寸会退回软解**（300ms/张，且吃满 Core1 半秒）；
  ② **目录里放 `folder.png` 会被直接拒绝**（`cover_art` 只收 JPEG，日志已明确提示）。

### 7.1 内嵌封面复测（fmttest 全部曲目都加了内嵌封面）

同一份固件，`--no-reset` 接着录，约 2 分钟（64 个 `MEM:` 采样）。日志里每张封面都有
`MEMTRACE: cover:req → meta_loaded → decoded → applied` 四段读数（内部 RAM / DMA / PSRAM）。

**结论：内嵌封面加载对内部 RAM 是零占用**——四段之间 `int` 一个字节都不动，只有 PSRAM 变化：

```
cover:req          int 117179/35840(232) | dma 85187 | psram 14108040
cover:meta_loaded  int 117179/35840(232) | dma 85187 | psram 14070124   ← 读出 JPEG blob
cover:decoded      int 117179/35840(232) | dma 85187 | psram 12738028   ← 1.33MB 像素缓冲（PSRAM）
cover:applied      int 117179/35840(232) | dma 85187 | psram 14070256   ← 归还
```

- 封面原图（9.8KB ~ 131KB）和 800×800 的 1,331,712 B 像素缓冲**全部在 PSRAM**，
  内部 RAM 与 DMA 都不吃；这也解释了为什么把 134 首全带上封面后 `iram` 仍然稳在 116KB。
- 全程 `iram`：117,391（开始）→ 115,439（结束），依旧只有每首 **~0.12~0.16KB** 的涓流；
  另有一次 **-6,724 B 瞬态**（快速连切 3 首 + 一张 800×800 软解叠在一起）和一次
  **+13.5KB/-13.5KB 瞬态**，都在 1~2 秒内归还，不是稳态成本。
- 全程最低 `iram` = **109,295 B**（就是上面那次瞬态），稳态区间 **115.4~117.4KB**。
- **真正的代价在 CPU 和 PSRAM**，与 IRAM 无关：

| 内嵌封面 | 走哪条路 | 耗时 | PSRAM 占用 | Core1 峰值 |
|---|---|---|---|---|
| 180×180 / 240×240 | **HW** | 5~7 ms | 64~115 KB | 可忽略 |
| 460×460 | **HW** | 16 ms | 423 KB | 可忽略 |
| **800×800** | **软解回退** | **329~491 ms** | **1.33 MB** | **54~61%** |
| 477×477 | 软解回退 | 326 ms | 486 KB | ~48% |

  HW 解码器只接受 **≤460×460**（屏幕尺寸）的图：460 走 HW，477 和 800 都被拒
  （`hw jpeg unsupported size`）。800×800 连播 4 首 ≈ 2 秒 Core1 被软解占满，
  期间 OGG 播放（80MHz 下已占 33%）的余量会被吃掉。
  ⚠ 另外**非 460×460 的封面是无缩放显示的**：小图居中、四周留黑边，大图超出部分
  被裁掉（不是等比缩放铺满）——所以 800×800 的封面既慢又看不到全图。
  → 建议：内嵌封面统一压到 460×460；或者软解改走 tjpgd 的 1/2 出图
  （800→400，只要 320KB/约 1/4 时间）再在绘制时缩放到 460×460，
  一次把"1MB PSRAM + 半秒 CPU"和"大图被裁/小图黑边"都解决掉。
  这比"给 mp3_play 加栈"更划算。

- 此外这次覆盖到了之前缺的 **OGG × 80MHz 息屏播放：cpu1 = 30~39%**（不是按 320MHz
  线性外推的 44~76%），说明 Tremor 在 80MHz 下仍有余量，但叠加蓝牙 SBC 编码
  就必须走 master 已有的 160MHz 档。

## 8. 主分支扫描大目录为什么不炸 / fork 该借鉴什么

### 8.1 先纠正一个前提：扫描代码两边是一样的

`playlist_builder.c` 的扫描内核（`collect_dir` 每个条目 `strdup`、主入口
`calloc(4096) × 3` 指针数组、`PLS_MAX_ENTRIES=4096`、PSRAM 的 `ctx.buf` / 显式栈）
**master 与 fork 逐行相同**——fork 只加了 `d_type` 快路径与指纹探针。
所以主分支不是"把大目录扫描写出了低内存实现"，它躲过去靠的是**调度位置**与
**两个偶然**：

| | master | fork |
|---|---|---|
| 触发方式 | `boot_show_prompt_scan()` → 用户点「开始扫描」（`start_scan_btn_cb`） | `boot_flow_run()` 里判定缓存不符就**自动** `boot_show_scanning()` |
| 发生时机 | **`app_main` 已返回之后**（主任务栈已释放），power_service / key_handler / sleep_timer / perf_monitor 全部建好 | **开机路径中**，与上述初始化并发 |
| 扫描任务栈 | `BOOT_SCAN_STACK_BYTES / 4u` = **8KB**（历史 word/byte 单位 bug） | 注释意图 **32KB**（失败回退 16KB） |
| 扫描时可用内部 RAM | ~48KB（开机完成稳态 56,951 B） | ~40KB 还要分给后续 init（实测 12-boot_flow 40,267 B → 扫描+init 一起归零） |

### 8.2 真正让它"不炸"的两条机制

1. **内部 RAM 的默认分配会自动溢出到 PSRAM，FreeRTOS 对象不会。**
   `strdup` → `malloc` → `MALLOC_CAP_DEFAULT`（8BIT），内部不够就落到 PSRAM，
   所以 1233 首目录 ~70KB 的名字数组能把内部 RAM 吃到只剩 19 B 也**不失败**
   （实测：打补丁后的 fork 扫描 9042 首全程内部 RAM 见底但扫描正常完成）。
   而 `xSemaphoreCreateMutex` → `pvPortMalloc` → **固定 `MALLOC_CAP_INTERNAL|8BIT`**
   （`freertos/heap_idf.c`），内部没了就是 NULL。fork 的崩溃点正是它。
2. **扫描放在所有 internal-only 分配之后。**
   master 扫描时，内部 RAM 上"必须内部"的消费者（互斥锁、任务栈、TCB）已经全部
   建完，后面只剩 LVGL（对象在 PSRAM）与 PSRAM 栈任务；内部 RAM 见底只是"见底"，
   没有下一个 internal-only 请求来撞墙。fork 把扫描提前到开机路径，撞上了
   `power_service_init` 的互斥锁 —— 差的就是这几百毫秒的顺序。

> 也就是说：**主分支并不免疫，只是没撞上**。任何"开机自动扫描"（fork 的思路）
> 都会把 master 也推到同一个坑里，所以这个修复应该两边都做。

### 8.3 fork 可借鉴 / 该改的（按性价比排序）

1. **把自动扫描挪出开机路径**：开机只做"指纹探针 + 判定"，需要重扫时先起主界面，
   延迟 1~2 秒（或 `app_main` 返回后再丢给后台任务）再启动扫描。
   这是最小、最稳的改动，直接消灭竞态。
2. **扫描栈 32KB → 8~12KB**：master 的 8KB（虽然是 `/4` bug 撞出来的）实测能扫完
   1233 首；fork 自己的注释也写了"历史 8KB 版本已能完成 1233 首扫描"。
   32KB 连续内部 RAM 在开机中后期本身就很难拿到，失败回退 16KB 更说明它没底。
3. **`strdup` 换 PSRAM 版**：`heap_caps_malloc(len+1, MALLOC_CAP_SPIRAM)` 再 memcpy，
   一次性消掉大目录 ~70KB 的内部峰值。**这是根因，主分支同样受益**，建议两边都改。
4. **`cap=4096` 的三个指针数组（96KB）别一次性 `calloc`**：虽然 >16KB 会落 PSRAM，
   但内部 RAM 紧张时容易先被切走一块；改成按实际条目数 `realloc` 或单个数组复用。
5. **扫描前做准入检查**：`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < N`
   就延后/提示，而不是硬上（现在 32KB 栈创建失败才回退，属于事后补救）。
6. **指纹探针也挪出开机关键路径**：实测 5833 个文件 1,276 ms，纯粹为了判断"要不要扫"，
   可以开机后异步做、结果出来再决定是否补扫。

### 8.4 反过来：fork 修了 master 一个真 bug

`BOOT_SCAN_STACK_BYTES / 4u` 这个 `/4` 是 **IDF 老 word 语义的残留**，
`xTaskCreatePinnedToCore` 的栈单位是字节——master 注释写 32KB、实际只给 8KB。
fork 把它改对了（并加了 16KB 回退）。合入时应"取 fork 的单位修正 + 8~12KB 的尺寸"，
不要连 32KB 一起搬。

## 9. 合入 master 后的实测（PR !1 已接受，2026-11-15 下午）

贡献者的三个 commit 已 rebase 到 `014ed5a`（无冲突、快进合并），之后在本机做的回归。

### 9.1 体积（合并基线）

| 构建 | 大小 | 余量 |
|---|---|---|
| debug（含 80KB 播放栈） | **2,621,136 B**（0x27FED0） | factory(3MB) 余 524,592 B；**ota_0(2.5MB) 只剩 304 B** |
| release | 未测（估算 ≈2.94MB） | 发版前必须实建确认能进 3MB factory |

debug 已经贴到 ota_0 的边（对时固件才需要装进 ota_0，播放器只要进 factory，所以不影响运行，
但说明"再多一点东西就会越线"）。

### 9.2 大曲库（9042 首）开机自动扫描：没崩，但余量见底

| 阶段 | int 空闲 | DMA |
|---|---|---|
| 11-sd | 92,603 | 60,611 |
| 12-boot_flow（扫描已起） | 42,763 | 27,159 |
| 14-key_handler | 30,207 | 14,603 |
| 15-power_service | 16,939 | 1,295 |
| **17-perf_monitor** | **13,243** | **15 / 最大块 0** |
| 扫描完成后稳态（BT 开、播放 MP3） | **76,555** | — |

- 日志里的关键一行：`W (1778) boot_flow: 32KB scan stack alloc failed, retry 16KB`
  —— **合并基线（蓝牙已初始化）下 32KB 连续内部 RAM 根本拿不到**，每次都走 16KB 回退。
  这条实测直接确认了 FUTURE_PLAN #55 的建议：扫描栈应直接写 8~12KB。
- 之所以没像 fork 原样那样 assert：主任务栈是 8KB（master 在蓝牙阶段改的）+
  扫描栈回退 16KB，把 `power_service_init` 的互斥锁垫住了；但 **DMA 15 B** 说明
  "能开机"和"有余量"是两件事（#56 的 strdup→PSRAM 仍然值得做）。

### 9.3 换卡 / 格式矩阵卡

`SD card changed: cache=f20836f36537205c now=38261c7e86a32030` → 自动扫描新卡 →
`dirs=6 / songs=?`。格式矩阵里 OGG 48k、OGG 44.1k、FLAC 44.1k 全部正常打开。

### 9.4 蓝牙「关一次就再也连不上」——根因与修复

**现象**：控制中心关蓝牙再打开后，点设备连接 `esp_a2d_source_connect()` 返回 `ESP_OK`，
但之后**一条 HCI 事件都没有**，耳机连不上；重启不动开关则一切正常。

**根因**：IDF 的 `esp_bluedroid_deinit()` **不清理** BTC 里 profile 的"已初始化"标志
（`btc_avrc.c` 的 `s_rc_ct_init` 是静态变量，只有 profile 自己的 deinit 才置 0）。
于是第二次开蓝牙时：

```
W BT_BTC: btc_avrc_ct_init already initialized
E bt_service: a2d source init failed: ESP_ERR_INVALID_STATE
```

A2DP source 停在半初始化态 → ACL 能连、流起不来。

**走过的弯路**（记下来免得再试）：曾试过"关=只 disable、开=只 enable"（省内存方案），
失败得更彻底——S31 的控制器是预编译库，`esp_bt_controller_disable()` →
`enable()` 之后**寻呼不工作**，`connect` 返回 OK 但没有任何 HCI 事件。

**最终修复**（`main/core/hardware/bt/bt_service.c`，+18 行）：关 host 前先反初始化两个 profile：

```c
esp_avrc_ct_deinit();        /* ① AVRC CT 必须先于 A2DP（btc_avrc_ct_deinit 会查 g_a2dp_on_deinit） */
esp_a2d_source_deinit();     /* ② 都是异步投递到 BTC 任务 */
vTaskDelay(300ms);           /* ③ 留时间真正 cleanup（A2DP 还要等状态回 IDLE） */
esp_bluedroid_disable(); esp_bluedroid_deinit();
esp_bt_controller_disable(); esp_bt_controller_deinit();
```

验证：关→开→连→起流全程无 `already initialized`，`iram` 在关掉的瞬间 **+51,428 B**
（说明"关"是真断电、真释放）。设计说明补进了 `BLUETOOTH_AUDIO.md` §2.1。

### 9.5 压力测试：48kHz OGG + 10 段 EQ + 蓝牙 + 息屏

| 项 | 实测 |
|---|---|
| 重采样 | `resampler: init 48000 -> 44100 Hz (L=147 M=160 taps=32, 18816 B coeffs)` |
| EQ | `eq: EQ enabled (10 bands)`（`原道StringSnow.eq`，preamp 0.75） |
| A2DP | 44.1k SBC，`stack pull 176.5 kB/s, miss 0.000%`（换曲瞬间偶发 0.75~1.7%） |
| 息屏频率 | `freq=160MHz`（蓝牙流触发的档位生效） |
| cpu1 | 320MHz 27~42%；**160MHz 息屏 42%** |
| `iram` | BT 关 76,555 → BT 流 71,643（BT 流只吃 ~4.9KB 内部 RAM） |
| `mp3_play` 栈高水位 | **61,016 / 65,536 B（只剩 4.5KB）** → 已把栈提到 **80KB** |

> 栈这一项是合并后唯一"差一点就炸"的地方：Tremor 的 codebook 索引 + `bt_audio_write()`
> 的重采样/EQ/增益**在同一个任务栈上**。80KB 仍在 PSRAM，不占内部 RAM。

## 10. 复现步骤

```powershell
# 1) fork 原样（会 boot loop）
cd C:\Users\ZYFDroid\Desktop\night-player-fake-pod-nano_branch_sakuraio
idfc build ; idfc flash ; idfc monitor

# 2) 可启动版本（本仓库 worktree，只改了一行 sdkconfig）
cd C:\Users\ZYFDroid\Desktop\FakePodNanoNightPlayer\.forktest
idfc build ; idfc flash ; idfc monitor
idfc monitor --no-reset     # 第二次启动（缓存命中）之后再看稳态
```
