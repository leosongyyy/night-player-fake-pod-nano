# 配置插槽（配置切换 / SAVE1~SAVE4）

> 2026-11 实现。NightPlayer 原版功能「配置插槽」：**不同场景用不同配置**。
> 播放设置页右下角（九宫格第 9 格）「配置切换」→ 弹出槽位列表 → 点击即切换。

---

## 一、需求

- 四个槽位（SAVE1~SAVE4），每个槽位**各自独立保存**一套配置；
- 槽位内的字段：**当前列表（当前歌曲）**、循环模式、随机模式、定时关机时长、
  音量、使用的 EQ 文件；
- 切槽位的入口：播放设置页第 9 格，标签「配置切换」，点击弹出一个类似
  「选择 EQ」的界面，列表展示 `SAVE1 [配置文件里的播放列表名]`，**当前活跃
  的槽位高亮**；
- 点击后**立即切换并应用**新配置；
- 后续关机时**自动保存到当前配置**，同时**记忆使用的配置**（下次开机回到
  上次用的那个槽位）。

### 实现时确认过的取舍

| 问题 | 结论 |
| --- | --- |
| 切换时是否先保存旧槽位 | **是**。切走前把当前这套存回原槽位，保证"切走再切回"回到离开时的状态；关机时再存一次（双保险） |
| 槽位里的"定时关机时长"怎么生效 | 按**存的原时长重新开表**（存 45 分钟 → 切过来重新倒数 45 分钟；存 0 → 关闭定时），不恢复剩余时间 |
| 切换过程中怎么处理播放 | 恢复该配置的歌曲并**自动播放** |
| 是否保存播放秒数位置 | **不保存**。沿用 NightPlayer 机制：只记"当前歌曲"，开机/切换都从歌曲开头开始（`config_store_get/set_last_song` 已没有秒数参数） |

---

## 二、设计：把可切换字段收进存储层，应用侧「无感」

### 2.1 为什么不是"另开一套 config_store2"

最省事的做法是让 UI 在切槽位时逐个 `config_store_set_volume/...`。但那样：

- 保存会**触发一堆 NVS 提交**（每个 setter 一次 `nvs_commit`）；
- `set_xxx` 是全局单份语义，"往旧槽位存"和"往新槽位存"分不清，很容易写错；
- **每加一个可切换字段都要改 UI**（保存列表、应用列表、弹窗摘要都要动）。

实际做法：在 `config_store` **内部**加一层"当前活跃槽位"，把可切换字段整体搬进
一个内存结构 `config_live_t s_live`，读写都落在 `s_live` 上；持久化时把 `s_live`
整块打包成**槽位 blob** 写进 NVS。

> **应用侧完全无感**：`config_store_get_volume()` / `set_volume()` / `get_loop_mode()`
> / `get_eq()` / `get_last_song()` … 签名与语义一字未变（唯一例外是删掉了恒为 0 的
> 秒数参数，见 §1）。业务代码（通知栏音量滑块、设置页、sleep_timer、
> player_controller、main.c）**一行都不用改就知道槽位的存在**。
> 以后要新增"可切换字段"，只在 `config_store.c` 内部加一处 + UI 摘要（如需）即可。

### 2.2 数据模型

```
config_store（core/storage/config_store.c）
├── 可切换字段（随槽位切换）  → static config_live_t s_live
│     volume / loop_mode / shuffle / shuffle_pos /
│     sleep_default_min / last_valid + last_global / eq_name + eq_data
│     ↑ 全部通过原有 get_xxx / set_xxx 访问
├── 不可切换字段（全局共用一份）
│     brightness / sleep_def_on / sleep_awake / screen_off /
│     click_wheel(×3) / batt_log(×2) / touch_test / replay_gain / was_playing
└── 槽位层
      4 × config_slot_blob_t（NVS 键 slot0..slot3，整块 blob 一次读写）
      + 活跃槽位下标（NVS 键 slot_active）
```

槽位 blob（`config_slot_blob_t`，POD，可直接 `nvs_set_blob`）字段：
`version` / `last_global` / `sleep_default_min` / `shuffle_pos` / `volume` /
`loop_mode` / `shuffle` / `shuffle_pos_valid` / `last_valid` / `eq_valid` /
`eq_band_count` / `eq_name[64]` / `eq_data(128B eq_preset_t)` / `last_list_name[128]`。

- `last_list_name` 是**保存时记下的目录/歌单名快照**，纯粹给 UI 显示
  `SAVE1 <列表名>` 用（`config_store` 不认识 playlist，名字由 UI 层
  `config_store_set_slot_list_name()` 提供）。
- `version` 与 `SLOT_BLOB_VERSION` 不符的 blob 视为**空槽位**（布局改了要 bump，
  老数据自动失效而不是读出垃圾）。
- 空间：每槽 ~330B，4 槽 ~1.3KB，24KB 的 nvs 分区余量充足
  （`config_store.c` 里有两条 `_Static_assert` 兜底）。

### 2.3 保存时机（三条路）

| 时机 | 位置 | 说明 |
| --- | --- | --- |
| 每次 `set_xxx` | `slot_persist_live()` | **增量存回当前活跃槽位**。同一 blob 反复覆盖写，NVS 分页复用，不会无限增长；这样"改完直接长按关机/断电"也不丢 |
| 切换槽位前 | `config_store_switch_slot()` 第 1 步 | 把当前整套存回**旧**槽位 |
| 伪关机 | `player_controller_save_last_position()` | 记完最后播放的歌曲/随机位置后，存回当前槽位（"后续关机时会自动保存到当前配置"） |

"记忆使用的配置" = 切换时把下标写进 NVS 键 `slot_active`，开机
`config_store_load_active_slot()` 读回来并整份载入。

### 2.4 升级兼容

槽位机制**不删除**旧全局键（`volume` / `loop` / `shuffle` / `sleepdef` /
`last_glob` / `eqname` …）。规则：

- 活跃槽位**从未保存过**（首次升级上来的机器）→ `config_store_init` 照旧读全局键，
  且此时 `set_xxx` 会**同时**写全局键（`s_slot_active_saved == false` 的兼容分支）；
- 第一次保存槽位后 → 全局键冻结，此后一律以槽位为准。

于是：老用户升级后**配置原样保留**，并在第一次关机时被收进"槽位 1"。

---

## 三、切换时"应用"了什么（运行时）

存储层只管存/取，**不碰硬件**（避免 `config_store` 反向依赖音频/播放器）。
运行时应用集中在 `core/storage/config_slots.c` 的 `config_slots_switch()`：

1. 记下"离开时所在列表名"（`config_store_set_slot_list_name`）→ 再切；
2. `config_store_switch_slot(idx)`：存旧槽位 → 换下标（写 NVS）→ 载入新槽位 →
   `config_notify()`（设置页等 UI 随即刷新为新值）；
3. **音量**：`audio_set_volume_level()`（直接落 DAC，不等下次播放）；
4. **EQ**：`eq_apply()`（无预设则 `eq_apply(NULL)` 直通）；
5. **睡眠定时**：`sleep_timer_set()`（按槽位存的**原时长**重新开表，0 = 关闭）；
6. **播放器**：`player_controller_restore_config()` → 控制器任务里**原子**恢复
   循环模式 / 随机开关 / 随机序列位置 / 当前歌曲，然后开始播放；
7. Toast："已切换到 SAVE2（<列表名>）"。

### 为什么播放部分要单独一条命令

循环模式与随机开关是 `player_controller` 的**内部状态**。若由 UI 线程分别
`player_controller_set_loop_mode()` / `set_shuffle()` / `play_song()`（各投一条
队列命令），命令之间可能被别的切歌命令插队，出现"用旧循环模式播新列表"。
`CMD_RESTORE_CONFIG` 把 4 件事包成一条命令，由控制器任务整条执行。

随机模式下切换时：先把随机列表目录切到**目标歌曲所在目录**，保存的随机位置
能映射到该目录内的歌曲就**精确恢复**（回到同一首歌、同一序列）；映射不上
（播放列表数据变过）则退化为"重新随机定位到目标歌曲"。空槽位/无记录 →
按顺序播放第一首。

---

## 四、UI

### 4.1 九宫格重排（`ui/settings_page.c`）

| 格位 | 原 | 现 |
| --- | --- | --- |
| 1/2/3 | 播放模式 / 顺序随机 / 睡眠定时 | 不变 |
| 4 | 空 | **EQ 预设**（原第 7 格） |
| 5 | 空 | **回放增益**（原第 8 格） |
| 6 | 空 | 空 |
| 7/8 | EQ / 回放增益 | 空 |
| 9 | 空 | **配置切换** |

- 图标：两行文字「配置\n切换」（系统字体 20px，与"回放\n增益"同款——
  系统字体不含 PUA 图标字形，只能排文字；`SETTINGS_ICON_H=48` 恰好放得下两行）。
- 状态文字：`SAVE1`~`SAVE4`（当前活跃槽位），由 `config_store` 变更回调刷新。

### 4.2 槽位弹窗（`ui/config_slot_popup.c`）

与 `ui/eq_popup.c` 同款式：全屏半透明遮罩 + 中央圆角面板 + 纵向行卡片；点击
行即切换并关闭，点击空白关闭。标题「配置切换」，行文本
`SAVE<n> <该配置里的列表名>`（无列表名时只显示 `SAVE<n>`），当前活跃槽位**黄色
高亮**。行数 = `config_store_slot_count()`（4），改槽位数只改 `CONFIG_STORE_SLOT_COUNT`。

---

## 五、涉及文件

| 文件 | 改动 |
| --- | --- |
| `main/core/storage/config_store.h/.c` | 可切换字段收进 `s_live`；槽位 blob 打包/解包/缓存；活跃槽位记忆；`config_store_load_active_slot` / `get_slot_info` / `get_slot_snapshot` / `save_active_slot` / `switch_slot` / `set_slot_list_name`；`get/set_last_song` 去掉秒数参数 |
| `main/core/storage/config_slots.h/.c` | **新增**：`config_slots_switch()`（切槽位 + 应用音量/EQ/睡眠定时/播放器 + Toast） |
| `main/core/player/player_controller.h/.c` | 新增 `CMD_RESTORE_CONFIG` 与 `player_controller_restore_config()`；`save_last_position` 末尾存回活跃槽位 |
| `main/ui/config_slot_popup.h/.c` | **新增**：槽位选择弹窗 |
| `main/ui/settings_page.c` | 九宫格重排（EQ→4、回放增益→5、配置切换→9）+ 槽位状态文字刷新 |
| `main/main.c` | `config_store_init()` 后立刻 `config_store_load_active_slot()`；开机按当前配置的定时时长恢复睡眠定时 |
| `main/CMakeLists.txt` | 加入两个新 `.c` |

---

## 六、验证

1. **存储**：开机日志 `config loaded: ... slot=N(saved/legacy/empty)`；切槽位日志
   `active slot -> N` / `slot N saved: ...`。
2. **切换**：设置页第 9 格 → 弹窗（当前槽位黄色）→ 点 SAVE2 → 立即换列表/歌曲/
   音量/EQ/定时；Toast 提示带列表名。
3. **记忆**：在 SAVE2 下关机 → 重启后仍是 SAVE2（日志 + 设置页状态文字）。
4. **回切**：SAVE1 改了音量/换了歌 → 切 SAVE2 → 再切回 SAVE1，应回到离开时的状态。
5. **升级兼容**（重要，用旧固件造数据）：先用旧固件跑一次配好音量/循环/EQ →
   刷本固件 → 配置应原样保留（日志 `active slot N empty: keep legacy global keys`），
   关机后变成 `slot 1 saved`。
6. **空槽位**：点从未保存过的槽位 → 回到默认音量/列表循环/顺序播放/EQ 关闭/无定时，
   从第一首开始播放，且该槽位随后变为"已保存"。

### 踩坑提醒

- **别在 `set_xxx` 里回写 `set_xxx`**：`slot_persist_live()` 直接操作 NVS 与缓存，
  不重新走 setter（否则递归 + 重复通知）。
- **`config_store_set_slot_list_name()` 只对已有数据的槽位生效**：否则会把空槽位
  凭空"造"成已保存槽位，UI 上表现为"SAVE3 明明没配过却有内容"。
- **`config_slots_switch()` 必须跑在 LVGL 上下文（持锁）**：`switch_slot` 会同步触发
  `config_store` 变更回调，回调方（设置页）直接操作 LVGL 控件。
- **改 `config_slot_blob_t` 布局要 bump `SLOT_BLOB_VERSION`**，否则老 blob 会被按新
  布局解释（读出垃圾）。bump 后老槽位自动变"空槽位"，属于可接受的降级。
- **⚠ 本功能抬高 NVS 写密度后暴露了一个潜伏崩溃**（切槽位 → NVS 写 flash 期间命中
  `performance_monitor` 的 `ESP_TIMER_ISR` 采样回调 → `Cache error` panic 重启）：
  根因是该回调没进 IRAM，而 systimer 中断带 `ESP_INTR_FLAG_IRAM`、**关缓存期间
  屏蔽不掉**。已给回调加 `IRAM_ATTR` 修复，完整机理与两条规则见
  `PSRAM_TASK_STACK.md` §2.5。**今后凡新增"频繁写 NVS/flash"的功能，都要回头
  审一遍 ISR 回调是否都在 IRAM。**
