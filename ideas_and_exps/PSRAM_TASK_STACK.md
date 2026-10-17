# 内部 RAM 紧张与 PSRAM 任务栈（task_psram）

> 源码：`main/core/utils/task_psram.{c,h}`；配置：`sdkconfig.defaults` 的
> `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`。

## 1. 现象（历史症状汇总）

内部 RAM 在开机末期几乎耗尽，任何"再分配一个内部栈"的代码都会静默失败：

| 症状 | 日志/表现 |
|---|---|
| 自动息屏、低电关机、暂停关机全部失效 | `E power_service: create power service task failed` |
| 设置菜单"重新扫描"点了没反应 | 任务创建失败（用户描述"提示无法分配栈内存"） |
| 串口不再打印 `MEM:` 内存/CPU 行 | `mem_monitor`（4KB）、`cpu_coll0/1`（1KB）创建失败 |
| 电池电压日志（开发者选项）不生成 | `batt_log`（4KB）创建失败 |

实测（debug 配置、内部堆 + DMA 堆统计）：

```
启动瞬间   internal=273083  (各池: 234KB/32KB/21KB/8KB/7KB)
LVGL 初始化前 internal=241211
player_controller_init 后 internal=136879   ← 播放列表/解码/元数据
app_ui_create 后        internal=17523     ← LVGL 堆：几百个对象/样式/字体缓存
开机完成（迁移前）      internal=18947，最大连续块仅 8692
```

即：**222KB 内部 RAM 被 LVGL 堆 + 播放引擎吃掉**，剩下不到 20KB 且碎片化
（最大连续块 8.6KB），3~8KB 的任务栈就分不出来了。

> 根因说明：`CONFIG_SPIRAM_USE_MALLOC=y` + `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`
> 下，**小于 16KB 的分配优先走内部 RAM**。LVGL 的 `LV_MEM_CUSTOM=stdlib`
> 让每个控件/样式/缓存都走这条路径——控件单次分配都很小（几百字节），
> 于是内部 RAM 被大量小对象瓜分，PSRAM 反而用不上。
>
> ⚠ **但真正让 debug 版雪上加霜的是 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`**
> （原先 debug 独有，release 为 `DISABLED`；**2026-09 起 debug 也已改为
> `DISABLED`**）。同一份代码、同一台设备实测：

| 配置 | 开机完成 internal | 最大连续块 | 稳态 iram |
|---|---|---|---|
| debug（Comprehensive 毒化） | 25,299 B | **14,836 B** | 13,679 B |
| debug（毒化关闭，仅此一项不同） | 50,239 B | **31,744 B** | 39,547 B |
| release（毒化关闭） | 51,351 B | 31,744 B | ≈39 KB |

> 即 **Comprehensive 堆毒化一项就吃掉约 26KB 内部 RAM**，并把最大连续块
> 从 31.7KB 压到 14.8KB（碎片化）。原因：该模式给每个分配加带调用栈的
> 元数据头，且**禁止空闲块合并**；LVGL 的成千上万个小对象把这个代价
> 放大成十几 KB 的碎片。
> 这解释了"release 版从未出现此问题、debug 版才出现"——不是版本差异，
> 是**堆毒化配置差异**。
>
> **决定（2026-09）**：debug 默认改为 `CONFIG_HEAP_POISONING_DISABLED=y`
> （`sdkconfig.defaults.debug` 内保留注释说明如何临时改回 Comprehensive）。
> 第 28/29 点的 PSRAM 栈迁移仍然必要：它让内部 RAM 与毒化开关解耦，
> 无论开关如何都有余量（关毒化后 debug 稳态约 39KB，开毒化约 13.7KB）。

## 2. 方案：非实时任务栈放 PSRAM

```c
BaseType_t task_create_psram_stack(TaskFunction_t fn, const char *name,
                                   uint32_t stack_bytes, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out,
                                   BaseType_t core_id);
```

内部实现：先 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`，
失败（PSRAM 未启用/耗尽）自动 `xTaskCreatePinnedToCore` 回退内部 RAM，
调用方无需处理分支。TCB 始终在内部 RAM（约 100B/任务），只有栈在 PSRAM。

### 2.1 判据（放 PSRAM 前必须满足）

1. **不做微秒级硬实时**：PSRAM 栈访问偶发缓存未命中会增加几十~几百 ns；
2. **不参与"关缓存"操作**：SPI Flash 擦写期间 PSRAM 不可访问，栈在 PSRAM
   会崩。本项目扫描任务只读 SD（SDMMC 驱动自带 DMA 缓冲）、写分区由驱动
   内部处理，均安全；
3. 栈需求大、执行频率低的任务优先（扫描/监视/日志类）。

### 2.2 已迁移清单

| 任务 | 栈 | 说明 |
|---|---|---|
| `mem_monitor` | 4KB | 每 2s 采样打印 |
| `cpu_coll0` / `cpu_coll1` | 各 1KB | 每 1s 读空闲计数器 |
| `batt_log` | 4KB | 采样周期醒来写 SD（默认关） |
| `rescan` | 4KB | 设置菜单重新扫描（一次性；关缓存段委托 deep_worker，见 2.4） |
| `dev_restart` | 2KB | 开发者选项"立即重启"（一次性） |
| `cover_load` | 6KB | 封面读 .npd + JPEG 解码（后台、低优先级） |
| `power_svc` | 3KB | 1s tick 超时检查 + 浅投递 |
| `touch_init` | 4KB | 开机一次性触摸初始化 |

**常驻净省约 12KB 内部 RAM**（一次性任务不计入常驻）。

### 2.3 保持内部 RAM 的任务（不要迁移）

| 任务 | 原因 |
|---|---|
| `LVGL` | 每帧刷屏，栈访问最密集 |
| `mp3_play` / `mp3_out` | 实时解码/输出，卡顿直接可闻 |
| `button_svc` | 按键中断触发，要求低延迟响应 |
| `player_ctrl` / `deep_worker` / `sleep_timer` | 交互与深链主路径 |
| `boot_scan` | **32KB，扫描过程会擦写播放列表分区（pldata_write/erase → pldata_unmap_if_mapped → munmap 关缓存），PSRAM 栈会 assert**；开机早期内部 RAM 尚有约 130KB，32KB 内部栈可正常分配 |

### 2.4 关缓存陷阱：栈在 PSRAM 的硬性禁区

`esp_partition_munmap` / `esp_partition_mmap` / 任何 `esp_flash_*` 擦写都会
先**关闭外部内存缓存**，此时 PSRAM 不可访问。IDF 在
`esp_mm/esp_cache_utils.c: esp_cache_freeze_caches_disable_interrupts` 里
直接断言：

```c
assert(s_task_stack_is_sane_when_cache_frozen());   // 要求 sp 落在 DRAM
```

栈在 PSRAM 的任务一旦触发这条路径就 **panic**（实测报错：
`assert failed: esp_cache_freeze_caches_disable_interrupts ...`）。

**踩坑记录（重新扫描）**：`rescan` 最初迁到 PSRAM 后，点"重新扫描"立刻崩溃
（`pldata_munmap → esp_partition_munmap → esp_mmu_unmap → 关缓存断言`）。

**最终方案**：`rescan` 任务栈仍放 PSRAM（内部 RAM 已无 8KB 连续块——实测
最大连续块只有 **8180 B < 8192 B**，即使先释放 cover/sleep 任务也腾不出），
把**关缓存的那一段**用 `deep_worker_call()` 投递到 deep_worker（内部栈）执行：

```c
static void rescan_do_deinit(void *arg) {   /* 在 deep_worker 内部栈上跑 */
    player_controller_deinit();             /* 停播放 + playlist_deinit → munmap */
    playlist_store_invalidate();            /* 擦列表头（pldata 层同样会 unmap） */
}
...
if (!deep_worker_call(rescan_do_deinit, NULL)) { rescan_do_deinit(NULL); }  /* 兜底 */
```

deep_worker 栈保持 **8KB**（实测 HWM 约 7.4KB，够用）。⚠ 曾试图把它加到 12KB
以给这条链留余量，结果**任务创建静默失败**（内部 RAM 碎片化，最大连续块
约 8.6KB；当时未检查 `xTaskCreatePinnedToCore` 返回值）→ 任务根本不存在 →
每次 `deep_worker_call` 都等满 5s 超时，表现为"按键息屏/亮屏延迟 5 秒"。
修正：① 栈回到 8KB；② `deep_worker_init()` 提前到 `app_main` 早期
（config_store 之后、硬件初始化之前，此时内部 RAM 充裕，8KB 必定成功）；
③ 检查创建返回值并报错；④ `s_exec_task == NULL` 时 `deep_worker_call`
直接返回 false（调用方就地兜底），不再白等超时；⑤ 超时时 dump 全部任务
状态（`uxTaskGetSystemState`）便于定位。

**判据（放 PSRAM 前必查）**：任务调用链里是否有 `esp_partition_mmap/munmap`、
`esp_flash_erase/write`、`pldata_write`/`pldata_erase_range`（内部会 unmap）。
有 → 栈必须内部 RAM，或把这段委托给内部栈任务（deep_worker）。

### 2.5 关缓存的第二个禁区：**中断回调的代码**也必须在 IRAM

关缓存不只影响"栈在 PSRAM"的任务——**代码本身在 flash 里**同样致命：缓存关掉
后 CPU 取指失败，报的是

```
Guru Meditation Error: Core 0 panic'ed (Cache error).
Cache disabled but cached memory region accessed
```

**实测崩溃（2026-11，配置插槽功能测试中复现）**：切配置槽位（NVS 写 flash）
时命中 `performance_monitor` 的 500ms 频率采样点：

```
lvgl_port_task → lv_indev_read → slot_popup_row_cb → config_slots_switch
  → config_store_switch_slot → cfg_write_u8 → nvs_set_u8 → esp_flash_write   ← 关缓存中
      ↑ systimer 中断打进：timer_alarm_isr → timer_alarm_handler
  → cpu_freq_isr_cb   ← PC 在 flash（0x4204c094）→ panic
```

**根因**：该回调用 `.dispatch_method = ESP_TIMER_ISR` 创建，**在 systimer 中断里
直接执行**；而 systimer 中断注册时带 `ESP_INTR_FLAG_IRAM`
（`esp_timer_impl_systimer.c`）——`spi_flash_disable_interrupts_caches_and_other_cpu()`
里的 `esp_intr_noniram_disable()` **只屏蔽"非 IRAM"中断，屏蔽不掉它**。于是任何
flash 操作期间它照常触发；回调没加 `IRAM_ATTR`（在 flash 里）→ 取指失败。

**修法**：给回调加 `IRAM_ATTR`（`main/core/utils/performance_monitor.c` 的
`cpu_freq_isr_cb`，它只调 `esp_clk_cpu_freq()`，后者本身就是 IRAM_ATTR，安全）。
验证：`nm` 里该符号从 `0x4204c094`（flash）变成 `0x4037e04c`（IRAM）。

⚠ **两条规则**：
1. 凡是 `ESP_TIMER_ISR` / 任何 ISR 回调，**一律 `IRAM_ATTR`**，且只能调用
   IRAM 安全函数（`ESP_LOGx`、`malloc`、`lv_*`、`nvs_*` 都不行）；
2. **触发概率与 flash 写密度成正比**（单次写窗口约 1~10ms、采样周期 500ms →
   单次约 0.2%~2%）。本次是"切配置槽位"把 NVS 写密度抬高后才暴露的——**新增
   频繁写 NVS/flash 的功能时，要回头审一遍 ISR 回调是否都在 IRAM**。
   该崩溃只在 `CONFIG_PERFORMANCE_MONITOR=y` 的构建（debug / releasecandidate）
   存在，release 不带该模块故掩盖了这个 bug。

> 🆕 2026-11：**LVGL 对象已搬进 PSRAM**，于是这条规则多了一层后果——ISR 里除了代码
> 不能在 flash，**连碰 LVGL 对象都不行了**。全工程"谁会在关缓存窗口里跑"的完整清单
> 与逐项审计见 **`FLASH_WRITE_VS_PSRAM.md`**（结论：目前只有一个 IRAM 中断
> `cpu_freq_isr_cb`，且不碰 PSRAM；NVS 写入口唯一、调用栈全在内部 RAM）。

## 3. 迁移后实测

```
I (5213) MEM: heap=6664587 (+0)  iram=17703 (+0)  cpu0=43% cpu1=21% ...
I (23217) MEM: stk: deep_worker=7432 button_svc=3352 power_svc=2292 LVGL=2192
                   player_ctrl=1416 sleep_timer=3412 mem_monitor=1968
                   cpu_coll0=348 cpu_coll1=376 mp3_play=6048 mp3_out=3188
                   cover_load=2392
```

- 性能监视三个任务全部起来（`mem_monitor`/`cpu_coll0`/`cpu_coll1` 出现在 stk 行）；
- 内部空闲从约 4.5KB 提升到 **16~17KB**（不再有任务创建失败）；
- 各任务栈高水位（HWM）显示余量正常：mem_monitor 1968/4096、
  cpu_coll 348~376/1024、power_svc 2292/3072、cover_load 2392/6144。

## 4. 另一处同样紧张的池：DMA（LVGL 绘制缓冲）

内部 RAM 迁移只解决了**任务栈**，还有第二个池会被同一次"省内存"操作挤干：
**DMA 池**（`MALLOC_CAP_DMA`，与内部 RAM 共用物理内存，但要求地址/大小满足
DMA 约束）。本项目 DMA 池的最大消费者是 **LVGL 的两块绘制缓冲**。

### 4.1 症状与根因

用户报告："开启随机模式后，每次开机都会先播放一秒上次记住的音乐，然后
随机到另一个队列里去"。日志里的关键行不是随机播放逻辑，而是：

```
E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
```

即**SD 卡每次读的 DMA 缓冲（512B）都分配不出来** → 解码器从未真正启动 →
`mp3_player` 判为 `MP3_STOP_DATA_CORRUPT` → 坏曲自动跳过 → 表现为"播一秒
就跳走"。

根因：当时 `EXAMPLE_LVGL_BUF_HEIGHT = EXAMPLE_LCD_V_RES / 10`
（460×46×2B ≈ 42.3KB × 双缓冲 ≈ **84.6KB DMA**），把 DMA 池压到不足 3KB。

### 4.2 DMA 池实测分布（V_RES/20 那次测量）

```
启动           263,683
display 后     246,683
audio 后       232,995
sdcard 后      226,755
LVGL 后        171,603   ← 绘制缓冲吃掉 55KB
开机完成        45,659   ← boot_flow（封面/JPEG 解码）又吃掉 126KB
```

⚠ 注意最后一行：**开机流程本身也是 DMA 大户**（126KB），所以 LVGL 缓冲
不能按"启动瞬间还剩 263KB"来估算，必须留出开机流程的余量。

### 4.3 取值与权衡

| `EXAMPLE_LVGL_BUF_HEIGHT` | 双缓冲 DMA | 帧率 | SD 读 |
|---|---|---|---|
| `V_RES / 10`（原值） | ≈84.6KB | 满 30fps | ❌ 512B 都分配失败 → 误判坏曲 |
| `V_RES / 20` | ≈42KB | ≈20fps | ✅ |
| **`V_RES / 12`（当前）** | ≈70KB | 接近 30fps | ✅ 稳态 DMA 余量约 15KB |

定稿 `V_RES / 12`：帧率与 DMA 余量的折中。⚠ 460/12 = 38.33 会截断成 38
（LVGL 内部再对齐到偶数），若在意"整数除不尽"可换成 `V_RES / 12` 的等价
显式常量；调这个值**必须同时看帧率与 DMA 余量**。

### 4.4 硬性约束：缓冲必须是 DMA 能力

曾尝试把绘制缓冲改成非 DMA（普通内部 RAM）以省 DMA 池，结果
`panel_io_spi_tx_color failed` / SPI 驱动 `Failed to allocate priv TX buffer`：
非 DMA 缓冲会让 SPI 驱动为每次传输额外分配 **bounce 缓冲**（
`setup_dma_priv_buffer`），同样从 DMA 池取内存；分配失败 → 刷屏阻塞 →
任务看门狗复位。**结论：LVGL 绘制缓冲保持 `MALLOC_CAP_DMA`，只调高度。**

## 5. 后续建议

- 新建后台任务**默认**用 `task_create_psram_stack`；只有硬实时/关缓存
  场景才用 `xTaskCreatePinnedToCore`（见 2.4 判据）。
- ⚠ 单位提示：ESP32 的 `StackType_t` 是 `uint8_t`，FreeRTOS 的
  `usStackDepth` **单位就是字节**（与 vanilla FreeRTOS 的"字"不同），
  本项目所有 `*_STACK` 宏均为字节。
- 若内部 RAM 仍然吃紧，下一步方向（按收益排序）：
  1. 把 `SPIRAM_MALLOC_ALWAYSINTERNAL` 调小（如 4096），让中等大小的分配
     落到 PSRAM——注意会影响依赖内部 RAM 的驱动（DMA 缓冲需显式
     `MALLOC_CAP_DMA`）；
  2. LVGL 侧减少常驻对象（虚拟列表已复用行槽，主要是各页常驻控件）；
  3. 检查是否有未释放的 LVGL 对象/缓存（mem_monitor 的 iram 列长期应持平）。
- `mem_monitor` 的 `iram=` 列就是内部空闲字节数，是判断"是否又紧张"的
  第一手指标；出现任务创建失败时先看它和最大连续块（
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`）。
- **DMA 池要单独看**（第 4 节）：`heap_caps_get_free_size(MALLOC_CAP_DMA)`。
  症状是 `sdmmc_cmd: allocate_dma_buf: not enough mem` 或 SPI bounce 缓冲
  分配失败，不是任务创建失败。改 LVGL 缓冲高度、加 DMA 外设（I2S/SD/SPI）
  后都要复查这个数。
