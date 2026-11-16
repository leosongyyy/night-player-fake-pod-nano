# 写 flash / NVS 与 PSRAM（含 LVGL 对象堆）的关系审计

> 2026-11。起因：修掉"配置插槽切槽位死机"之后（根因是**跨任务调 LVGL**，见
> `CONFIG_SLOTS.md` §七 bug B），提出一个问题：
> **项目里还有很多写 NVS 的地方，LVGL 对象又从内部 RAM 搬到了 PSRAM，
> 这些写 flash 的地方会不会偶发地和 LVGL 冲突？**
>
> 本文是逐条审计的结论。审计结果：**没有发现新的可触发缺陷**（唯一新增的
> 规则是"IRAM ISR 里绝不许碰 LVGL/PSRAM"，属预防性约束），但机制值得写清楚，
> 否则下一次改动很容易踩进来。
>
> ⚠ **2026-11-16 更新**：播放列表数据已从 flash 的 `pldata` 分区搬到 SD 卡缓存文件
> （`/sdcard/.ecov/pldata.bin`），所以下文凡是把 `pldata` 当作"flash 擦写/关缓存"
> 场景的例子，**那条路径已经不存在**（`pldata_write/erase/munmap` 现在只是内存/文件
> 操作）。审计结论对 **NVS / font / OTA 等其他 flash 路径仍然成立**。
> 详见 `PLDATA_ON_SDCARD.md` §14。

---

## 一、结论先行

| 问题 | 结论 |
| --- | --- |
| 写 NVS（或擦写 pldata）时，LVGL 共并发跑渲染吗？ | **不会**。写 flash 期间两颗核都被停住且外部内存缓存关闭，除 IRAM 里的中断处理程序外**没有任何代码在跑**。 |
| 那"LVGL 对象在 PSRAM"会不会因为别人写 NVS 而崩？ | **不会**。冻结不等于出错：被停住的核恢复正常后数据完好。只有"**在关缓存窗口里被访问**"才会崩。 |
| 项目里有哪些 NVS 写入口？ | **唯一入口 `config_store.c`**（全项目 `nvs_*` 调用都在这里），调用者全部在**内部 RAM 栈**的任务上。 |
| 有没有栈在 PSRAM 的任务直接写 flash？ | **没有**。6 个 PSRAM 栈任务里凡涉及关缓存段的（重新扫描 / 伪关机）全部 `deep_worker_call()` 委托给内部 12KB 栈任务。 |
| 有没有 IRAM 中断在窗口里碰 PSRAM？ | **没有**。全项目只有一个 IRAM 中断回调（`performance_monitor` 的 `ESP_TIMER_ISR`），已在 2026-11 加 `IRAM_ATTR` 并只调 `esp_clk_cpu_freq()`。 |
| 2.8MB 的播放列表 PSRAM 缓冲直接写 pldata 有问题吗？ | **没问题**。IDF 的 `esp_flash_write` 对"非 DRAM 源"用 32 字节栈弹跳，`memcpy` 发生在关缓存**之前**。 |
| 那这次改动到底改了什么？ | 把**安全性网**拆掉了一层：以前 LVGL 对象在内部 RAM，IRAM 中断里误碰 LVGL 只是"不规范"；现在会直接崩。故新增硬规则（见 §五）。 |

---

## 二、机制：关缓存窗口里到底还有谁在跑

一次 `nvs_commit()` / `esp_partition_write()` 的真实过程（以 IDF 6.1 源码为准）：

```
esp_flash_write()                     spi_flash/esp_flash_api.c:1111
  └─ 逐块 start_prog() → spi_flash_disable_interrupts_caches_and_other_cpu()
                            spi_flash/cache_utils.c:283
       ├─ esp_intr_noniram_disable()      ← **只屏蔽"非 IRAM"中断**（同文件 :286）
       ├─ 关本核的 I-cache/D-cache（flash 与 PSRAM 一起关）
       └─ IPC 把另一核 park 在 IRAM 里的自旋代码上（同样关缓存）
     ↓（此刻起：PSRAM 与 flash 映射区都不可访问）
     chip->chip_drv->write(...)
     ↓
   end() → 恢复缓存与中断
```

三条推论（审计就是按这三条做的）：

1. **只有两种代码能在窗口里跑**：执行 flash 操作的那个任务（在它自己的栈上），
   以及**带 `ESP_INTR_FLAG_IRAM` 的中断**。其余任务（含 LVGL 任务）与其余中断
   全部冻结/屏蔽。
2. 所以"栈在 PSRAM 的任务写 flash"是必死：IDF 在 `cache_utils.c:114` 有现成断言
   `esp_task_stack_is_sane_cache_disabled()`（本工程 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`，
   断言是**开启**的，会立刻报错而不是随机崩）。
3. "IRAM 中断里碰 PSRAM"也是必死，而且**没有任何断言保护**——这是唯一需要靠
   纪律守住的坑（`CONFIG_SPIRAM_RODATA` / XIP 指令都不影响这条）。

补充两个 IDF 事实，避免误解：

- **任务 TCB 永远在内部 RAM**（`freertos/esp_additions/idf_additions.c:44-49` 显式
  `pvPortMalloc`，注释写明 "the TCB must be in internal memory"），只有**栈**用
  `MALLOC_CAP_SPIRAM`。所以 1kHz tick 中断（IRAM）遍历延迟链表时不会碰 PSRAM。
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`：≤16KB 的普通 `malloc` 仍落内部 RAM。
  LVGL 对象进 PSRAM 是**因为** `lvgl_mem.c` 显式请求 SPIRAM，不是默认行为；NVS 的
  页缓冲（`calloc`，≤4KB）因此在内部 RAM。

### 顺带澄清一个常见误解

"写 flash 期间 PSRAM 不可用"**不是**"另一个任务访问 PSRAM 会读到脏数据"，而是
**那颗核被停住、根本不会继续执行**。所以：

- 不需要"写 NVS 前先把 LVGL 对象刷出去"之类的同步；
- 不需要为 NVS 写加互斥锁来"保护 LVGL"；
- 唯一要保证的是：**别让"栈在 PSRAM 的任务"或"PSRAM 中的数据"出现在窗口里**。

---

## 三、逐项审计

### 3.1 NVS 写入点（唯一入口 + 调用上下文）

> ⚠ 本节审的是**安全性**（会不会崩）。**写频率/性能**（亮度拖动卡顿、
> NVS 写预算、延迟落盘机制）见 `NVS_WRITE_BUDGET.md`——同一批写入口，
> 两种视角。

全项目 `nvs_set_* / nvs_erase_key / nvs_commit` **只出现在 `core/storage/config_store.c`**。
`pldata` 擦写只出现在 `core/storage/playlist_store.c`（→ `core/hardware/pldata.c`）。
这是很关键的一条：新增配置项不需要（也不应该）另开 NVS 写入口。

| 触发点 | 运行任务 | 栈位置 | 结论 |
| --- | --- | --- | --- |
| 设置页 9 格 / EQ 弹窗 / 通知栏滑块 / 设置菜单 / 槽位弹窗 | LVGL 任务 | 内部 20KB | ✓ |
| `key_handler` 音量键 | `button_svc` 任务（持 LVGL 锁） | 内部 4KB | ✓ |
| 开机 `boot_flow`（无卡时亮度拉满） | 主任务（持 LVGL 锁） | 内部 16KB | ✓ |
| 播放控制器：切歌/暂停/循环/随机/坏曲 | `player_ctrl` 任务 | 内部 4KB | ✓（实测 HWM 余量 1464B，见 §四） |
| 伪关机保存（`save_last_position`） | `deep_worker` 任务 | 内部 12KB | ✓ |
| `config_store_init` 的音量迁移写回 | 主任务 | 内部 16KB | ✓ |

⚠ `player_ctrl` 是 4KB 栈里唯一会写 NVS 的"小栈任务"，实测余量 1464B——**不要再给它
加栈上大对象**（`player_sync_config` 已刻意不走 `player_controller_get_status()`
那种 ~1KB 结构体拷贝）。

### 3.2 栈在 PSRAM 的任务（5 个）：会不会自己撞进关缓存段？

| 任务 | 直接写 flash？ | 结论 |
| --- | --- | --- |
| `mem_monitor` / `cpu_coll0` / `cpu_coll1` | 否（只读 heap/CPU 计数器） | ✓ |
| `cover_load` | 否（读 SD + 解码，改 LVGL 时持锁） | ✓ |
| `power_svc` | 否——息屏/亮屏/伪关机全部 `deep_worker_call()` | ✓ |
| `rescan` | 否——`playlist_deinit` / 擦列表头委托 `deep_worker`（`settings_menu.c` 的 `rescan_do_deinit`）；**deep_worker 不可用时**才就地执行并已在注释里标风险 | ✓（有兜底，可接受） |
| `touch_init` | 否 | ✓ |

### 3.3 IRAM 中断清单（窗口里唯一还能跑的东西）

| 中断 | 是否 IRAM 标志 | 回调是否 IRAM | 是否碰 PSRAM |
| --- | --- | --- | --- |
| `performance_monitor` 频率采样（`ESP_TIMER_ISR`） | 是（systimer 注册时带 `ESP_INTR_FLAG_IRAM`） | **是**（2026-11 已修） | 否（只调 `esp_clk_cpu_freq()`） |
| FreeRTOS 1kHz tick（systimer） | 是 | 内核 IRAM 代码 | 否（只碰内部 RAM 的 TCB） |
| LCD 刷屏完成（SPI）/ 触摸 I2C / SDMMC / 按键 GPIO | **否** | — | 窗口内被 `esp_intr_noniram_disable()` 屏蔽，不会跑 |
| `vApplicationTickHook` | — | 本工程未实现 | — |

`CONFIG_*_ISR_IN_IRAM` / `CONFIG_*_ISR_IRAM_SAFE` 在本工程 sdkconfig 中**全部未开**，
所以上表就是完整清单。

### 3.4 PSRAM 源缓冲写 flash（2.8MB 播放列表）

`playlist_builder` 的构建缓冲（`PLS_BUILD_BUF_SIZE`，实测约 2.8MB）明确分配在 PSRAM
（`playlist_builder.c:304`，`MALLOC_CAP_SPIRAM`），最后经
`playlist_store_write_layout → pldata_write → esp_partition_write → esp_flash_write`
落盘。看起来是"关缓存时读 PSRAM"，实际安全：

```c
// spi_flash/esp_flash_api.c:1131
direct_write |= esp_ptr_in_dram(buffer);                  // PSRAM → false
...
uint32_t temp_buf[8];                                     // 32B 栈弹跳（内部 RAM）
write_len = MIN(len_remain, sizeof(temp_buf));
memcpy(temp_buf, buffer, write_len);                       // ← 关缓存之前完成
write_buf = temp_buf;
...
start_prog(chip);  // 到这里才关缓存
```

代价是"每 32 字节一次 flash 操作"较慢，所以扫描整体写分区以秒计；但**不会崩**。
想更快就得自己在内部 RAM 分块（本项目没这个需求）。注意 `esp_flash_read` 走的是另一
条路（临时缓冲 + `ESP_ERR_NO_MEM`），对本项目无影响。

### 3.5 mmap / munmap（同样会关缓存）

| 调用点 | 任务 | 结论 |
| --- | --- | --- |
| `font_npf_init` → `esp_partition_mmap`（font 分区，2026-11 起；TTF 时代是 `font_ttf_init`） | 主任务（开机、内部栈） | ✓ |
| `playlist_init` → `pldata_mmap` | 主任务 / `player_controller_init` | ✓ |
| 重新扫描 → `playlist_deinit` → `pldata_munmap` | `deep_worker`（内部栈） | ✓ |

### 3.6 同类但不同因：跨任务碰 LVGL（与 PSRAM 无关，已被本次修复收口）

这类才是真正会"偶发死机"的：不是缓存问题，而是**在非 LVGL 任务里改 LVGL 对象树**。

| 位置 | 状态 |
| --- | --- |
| `config_store` 变更回调（曾从 `player_ctrl` 里直接跑 UI 回调） | **已修**：改为 100ms lv_timer 延迟派发（`config_store_notify_dispatch`） |
| `cover_load` 任务改封面 | 正确：全程 `lvgl_port_lock/unlock` |
| 开机扫描写入阶段 | 正确且是最佳范例：扫描任务（内部栈）**先抢 LVGL 锁**再擦写分区，写完才归还（`boot_flow.c:398-451` 注释很完整） |
| `ui_toast_show` | 约定：**调用方必须已持锁**（toast.c 自己不锁）；现有 6 个调用点都在 LVGL 任务或持锁的主/按键任务里 |
| `fake_shutdown` 的提示渲染 | 在 `deep_worker` 里显式 `lvgl_port_lock` |

---

## 四、实测数据（本次审计时抓的）

```
I (23447) MEM: stk: deep_worker=11592 button_svc=3440 power_svc=2376 LVGL=10172
                   player_ctrl=1464 sleep_timer=3496 mem_monitor=2948
                   cpu_coll0=428 cpu_coll1=460 mp3_play=6104 mp3_out=3276 cover_load=2232
```

- `player_ctrl` 只剩 1464B（4KB 栈）——它每次切歌都写一次 NVS，说明这条 flash 路径
  的栈开销已经被包含在这个水位里，**但仍是最紧的任务**。
- 取样时设备正在播放（NVS 写路径已跑过：开机恢复那首歌时写过一次槽位 blob）。

---

## 五、规则（新增，务必遵守）

1. **新增任何 flash 擦写（NVS / pldata / OTA / mmap / munmap）**：
   - 发起任务必须**栈在内部 RAM**；不确定或栈在 PSRAM → 一律 `deep_worker_call()`；
   - 判断方法：`xTaskCreatePinnedToCore`（内部）✓ / `task_create_psram_stack`（PSRAM）✗。
2. **`ESP_TIMER_ISR` / 任何 IRAM 标志的中断回调**：必须 `IRAM_ATTR`，且**只许碰内部
   RAM**——不许碰 LVGL 任何 API/对象、FreeType 字形缓存、播放列表 mmap 指针、任何
   PSRAM 分配出来的缓冲。
   ⚠ 这条是 LVGL 对象搬进 PSRAM 后**新增**的硬约束：以前（对象在内部 RAM）ISR 里误碰
   LVGL 只是不规范，现在会直接 `Cache error`。
3. **配置变更（或任何"可能被非 LVGL 任务触发"的通知）不许同步调 UI**：
   `config_store` 已经做成"置标志 + LVGL 任务里 100ms 派发"，新模块要么复用这套，
   要么自己用 lv_timer/player_ticker 轮询，**不要**在回调里直接 `lv_*`。
4. 大缓冲写 flash 若来自 PSRAM，能用但慢（32B 弹跳）；对性能敏感就自己在内部 RAM
   分块。**不要**因此把它改成"绕过 esp_partition_write 直接操作 flash"。

---

## 六、怎么自查（下次怀疑同类问题时）

| 现象 | 指向 |
| --- | --- |
| `assert failed: esp_task_stack_is_sane_cache_disabled` | 有 PSRAM 栈任务直接写了 flash → §五 规则 1 |
| `Cache error` / `InstructionFetchError`，且 backtrace 出现在某个 ISR | IRAM 中断里碰了 PSRAM/flash → §五 规则 2 |
| 卡死在 `lv_*`（比如 `lv_inv_area`）且当前任务不是 `LVGL` | 跨任务碰 LVGL → §五 规则 3（本次 bug B） |
| `task_wdt: CPU x: <某任务>` 且它卡在 LVGL 内部 | 同上，多半是对象树被并发改坏 |

命令备忘：

```powershell
idfc menuconfig search "ISR" "IRAM"     # 确认新增驱动/定时器是否把 ISR 放进 IRAM
idfc menuconfig search "TASK_CREATE_ALLOW_EXT_MEM"   # 上面那条断言是否生效（应为 y）
```

（`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=n` 时断言消失，PSRAM 栈写 flash 会变成
随机崩溃而不是立刻报错——**不要关它**。）
