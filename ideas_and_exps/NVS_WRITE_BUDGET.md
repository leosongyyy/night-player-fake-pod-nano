# NVS 写预算：频繁写 flash 导致的 UI 卡顿与优化

> 2026-10。起因：新固件下拉控制中心**拖动亮度条时 UI 剧烈卡顿**。
>
> 初判是“实时写 NVS + LVGL 都搬到 PSRAM，两者互斥”，**这个判断不对**（见 §二）。
> 真正的原因是**写 flash 时全系统停摆**，而亮度拖动每秒要写 30 次。
>
> 本文是逐项审计 + 已实施的优化（P0/P1/P2）+ 新增的硬规则。

---

## 一、结论先行

| 问题 | 结论 |
| --- | --- |
| 写 NVS 为什么会卡 UI？ | `nvs_commit()` 写 flash 时 `spi_flash_disable_interrupts_caches_and_other_cpu()` 会**关掉两颗核的 flash/PSRAM 缓存并把另一核 park 在 IRAM 自旋**，除 IRAM 中断外没有任何代码在跑。单次 5~20ms，直接吃掉 LVGL 的 33ms 帧预算。 |
| 和“LVGL 对象在 PSRAM”有关吗？ | **无关**。对象在内部 RAM 一样要停核；把对象搬回去不会改善卡顿（只是 ISR 更安全）。 |
| NVS 页缓冲在 PSRAM 吗？ | **不在**。IDF 用 `calloc` 申请 4KB 页，`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 让 ≤16KB 的 malloc 落内部 RAM。 |
| 全项目 NVS 写入口？ | **唯一入口 `config_store.c`**（见 `FLASH_WRITE_VS_PSRAM.md` §3.1）。问题不在入口分散，而在**调用频率**。 |
| 最严重的高频写？ | ① 亮度拖动（约 30 次/秒）；② 音量竖条拖动（一次最多 23 次，每次写整块 352B 槽位 blob）。 |
| 优化效果？ | 亮度一次拖动从 **100~250 次 commit → 1 次**；音量一次拖动从 **≤23 次 → 1 次**；暂停/停止从 2 次 blob → 1 次；关机保存从 4 次 → 2 次；切槽位 blob+u8 合并为 1 次 commit。 |

---

## 二、机制：为什么写一次 flash 会卡整个 UI

```
nvs_commit()
  └─ esp_flash_write()                          spi_flash/esp_flash_api.c
       └─ spi_flash_disable_interrupts_caches_and_other_cpu()
            ├─ esp_intr_noniram_disable()       ← 只屏蔽“非 IRAM”中断
            ├─ 关本核 I-cache/D-cache（flash 与 PSRAM 一起关）
            └─ IPC 把另一核 park 在 IRAM 自旋代码上
          ↓ 此刻起：除 IRAM 中断外，**两颗核都不执行普通代码**
          chip->chip_drv->write(...)            ← 写 4KB 页
```

- 写一页（4KB）约 **5~20ms**（`CONFIG_SLOTS.md` §七 实测），期间 LVGL 渲染停摆。
- NVS 是“追加写 + 页满 GC”：每次 `nvs_commit` 在页内追加条目，页满后要
  选页擦除并把有效条目搬走（4KB 扇区擦除 20~50ms，仍在关缓存窗口内）。
- 分区只有 **24KB = 6 页**（`partitions.csv` 的 nvs `0x6000`），每页约 100+
  个条目。按 30 次/秒算，**4~5 秒就写满一页**，随后周期性出现几十 ms 的擦除尖峰。

所以一次亮度拖动是“每 33ms 一次 5~20ms 停顿 + 每几秒一次几十 ms 擦除”的叠加，
表现为剧烈卡顿且抖动。

> 补充：写 flash 的代价与“数据多大”无关——NVS 的写单位是页，1 字节的
> `bright` 和 352 字节的槽位 blob 单次开销基本一样。

---

## 三、审计：所有 NVS 写入口与频率

| 触发点 | 代码位置 | 频率 | 单次代价 | 处理 |
| --- | --- | --- | --- | --- |
| **亮度拖动** | `notification_panel.c` 亮度条 → `config_store_set_brightness` | **最高**：indev 33ms 轮询，约 30 次/秒，一次拖动 100~250 次 | 1×u8 → 4KB 页 + commit | **P0：延迟落盘** |
| **音量竖条拖动** | `notification_panel.c` 音量条 → `config_store_set_volume` | 高：每 20px 一档，一次最多 23 次 | 1×352B blob + commit | **P1：延迟落盘** |
| 音量按键 | `key_handler.c` | 低（单击一档） | 1×352B blob | 保留（低频） |
| 暂停 / 停止 | `player_controller.c` `player_cmd_pause` / `CMD_STOP` | 中低（换歌后首次暂停） | **2×**352B blob | **P2：合并为 1 次** |
| 关机保存（AXP2101 真断电前） | `player_controller_save_last_position` | 低 | 4 次（sync 2 + was_playing 1 + slot 1） | **P2：2 次** |
| 切配置槽位 | `config_store_switch_slot` | 低 | 旧槽位 blob + `slot_active` u8 = 2 次 | **P2：合并 1 次 commit** |
| 设置菜单 / EQ / 随机开关 | settings_menu / eq_popup / player_controller | 极低 | 1 次 | 保留 |
| 兼容层（首次保存槽位前） | `slot_persist_live` | 仅升级后第一次 | **9 次**（8 键各 commit + blob） | **P1：8 set + 1 commit** |

---

## 四、已实施的优化

### P0：亮度延迟落盘

- `config_store` 新增：
  - `config_store_set_brightness_deferred()`：只改内存 + 置 `s_bright_dirty`；
  - `config_store_flush_pending(bool force)`：dirty 时写一次 `bright`。
- `notification_panel.c`：亮度条 `PRESSING` 用 deferred；`RELEASED` 调
  `config_store_flush_pending(true)`。
- `ui/app.c` 的 100ms 定时器：`config_store_flush_pending(false)` 做保底
  （静默 500ms 后写，防止漏 RELEASED）。
- `boot_flow.c` 的“无卡亮度拉满”仍用原 `config_store_set_brightness()`（立即写）。

### P1：音量延迟落盘 + 兼容层合并 commit

- `config_store_set_volume_deferred()`：只改内存 + 置 `s_volume_dirty`。
- 音量条 `PRESSING` 用 deferred；`RELEASED` 调 flush（与亮度共用）。
- flush 的落盘走 `slot_persist_live()`（与 `set_volume` 语义完全一致，
  兼容态下也会写旧全局键）。
- `slot_persist_live()` 的兼容层从“8 键各 commit”改为“8 次 set + 末尾 1 次
  commit”（`cfg_set_*` + `cfg_commit`）。

### P2：合并同一逻辑操作里的多次 blob 写

- 新增只改内存的 setter：`config_store_set_last_song_ram()` /
  `config_store_set_shuffle_pos_ram()`（返回是否变化，不写 NVS）。
- `player_sync_config()` 改为只改内存，落盘交给调用方的
  `save_active_slot()` / `switch_slot()`。
- 新增 `player_persist_position()`：暂停/停止时把 last_song + shuffle_pos
  合并成**一次** blob 写。
- `config_store_switch_slot()`：旧槽位 blob + 延迟的亮度 + `slot_active`
  合并成**一次** `nvs_commit()`。

---

## 五、规则（新增，务必遵守）

1. **高频 UI 调节（拖动、长按连发）一律用 `*_deferred` + flush**：
   - 拖动过程的每一帧只改内存，**绝不 `nvs_commit`**；
   - 松手事件里 `config_store_flush_pending(true)`；
   - 100ms 定时器里 `config_store_flush_pending(false)` 保底；
   - 新增“调节类”配置项时照此办理，不要直接写 `cfg_write_*`。
2. **`config_store_flush_pending()` 只能由“栈在内部 RAM”的任务调用**：
   - 可以：LVGL 任务（20KB）、`deep_worker`（12KB）、主任务；
   - 不可以：`power_svc` / `mem_monitor` / `rescan` 等 `task_create_psram_stack`
     创建的任务（写 flash 会命中 IDF 关缓存断言）。
   - 现有调用点：`ui/app.c` 定时器、`notification_panel.c` 松手、
     `power_service_do_screen_off`（**仅 arg != NULL = 经 deep_worker 执行**）、
     `fake_shutdown_common`（deep_worker）。
3. **批量写用 `cfg_set_*` + 一次 `cfg_commit()`**，不要多个键各写各的——
   NVS 的写单位是页，一次提交能覆盖所有脏键，省的是整页 4KB 写。
4. **同一逻辑操作里的多次槽位修改**用 `*_ram` setter 合并，末尾调一次
   `config_store_save_active_slot()`。
5. **不要为了“更保险”给拖动加 `nvs_commit`**：亮度/音量最多丢 500ms 内的
   最后一次修改，而且关机/息屏/切槽位都会强制 flush，正常路径不会丢。

---

## 六、验证方法

- 手感：拖动亮度/音量条应无卡顿、无周期性顿挫。
- 落盘正确性：调完亮度 → 松手 → 长按上键关机（AXP2101 真断电）→ 开机，亮度应保持。
- 日志：拖动音量时串口**不应**出现大量 `SAVE1 saved to NVS`（每次 blob 写
  都会打这一行）；只在松手后出现一次。
- 定量（可选）：在 `cfg_commit()` 前后打 `esp_timer_get_time()`，统计一次
  拖动的 commit 次数与总耗时。

---

## 七、未做 / 可选

- **把落盘移到独立低优先级任务**：P0 之后一次落盘已经可接受（5~20ms，
  且发生在松手时），新建内部 8KB 栈任务吃紧（`player_ctrl` 4KB 实测只剩
  1464B），暂不做。
- **扩大 nvs 分区**：`0x9000 + 0x6000` 正好顶到 `phy_init`，没有空间；
  降低写频率后寿命问题自然缓解。
- **NVS 写计数器**：若以后需要长期观察写密度，可在 `performance_monitor`
  的 2s 打印里加一个 `cfg_commit` 计数，本文件未实现。
