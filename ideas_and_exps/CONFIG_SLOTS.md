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
| 切换槽位前 | `config_slots_switch()` → `player_controller_sync_config()` → `config_store_switch_slot()` 第 1 步 | 先把**播放现场**（当前歌曲 / 随机位置 / 列表名）同步进活跃槽位，再整份存回**旧**槽位 |
| 伪关机 | `player_controller_save_last_position()` | 同一个 `player_sync_config()` 同步现场后存回当前槽位（"后续关机时会自动保存到当前配置"） |

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

1. 把**播放现场**同步进活跃槽位（`player_controller_sync_config()`：当前歌曲 +
   随机位置 + 所在列表名）→ 再切；
2. `config_store_switch_slot(idx)`：存旧槽位 → 换下标（写 NVS）→ 载入新槽位 →
   `config_notify()`（设置页等 UI 随即刷新为新值）；
3. **音量**：`audio_set_volume_level()`（直接落 DAC，不等下次播放）；
4. **EQ**：`eq_apply()`（无预设则 `eq_apply(NULL)` 直通）；
5. **睡眠定时**：`sleep_timer_set()`（按槽位存的**原时长**重新开表，0 = 关闭）；
6. **播放器**：`player_controller_restore_config()` → 控制器任务里**原子**恢复
   循环模式 / 随机开关 / 随机序列位置 / 当前歌曲，然后开始播放；
7. Toast：**只报槽位编号**「已切换到 SAVE2」——列表名可能长达 128 字节，
   塞进底部提示框会溢出/折行（列表名在槽位弹窗里看，见 §4.2）。

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
   `active slot -> N` / `switch: load new slot: SAVE N -> live (...)`
   / `SAVE N saved to NVS: ... last=<歌曲>(valid) list=<列表名>`。
2. **切换**：设置页第 9 格 → 弹窗（当前槽位黄色、行文本带列表名）→ 点 SAVE2 →
   立即换列表/歌曲/音量/EQ/定时；Toast 只显示「已切换到 SAVE2」（不带列表名）。
3. **记忆**：在 SAVE2 下关机 → 重启后仍是 SAVE2（日志 + 设置页状态文字）。
4. **回切**：SAVE1 改了音量/换了歌 → 切 SAVE2 → 再切回 SAVE1，应回到离开时的状态。
   ⚠ 重点测"**从列表点一首歌后立刻切槽位**"（旧实现必错，见 §七）。
5. **升级兼容**（重要，用旧固件造数据）：先用旧固件跑一次配好音量/循环/EQ →
   刷本固件 → 配置应原样保留（日志 `active slot N empty: keep legacy global keys`），
   关机后变成 `slot 1 saved`。
6. **空槽位**：点从未保存过的槽位 → 回到默认音量/列表循环/顺序播放/EQ 关闭/无定时，
   从第一首开始播放，且该槽位随后变为"已保存"（弹窗里也应有列表名，不再是光秃秃的
   `SAVE2`）。
7. **配置通知不再崩**：开着播放，反复点"随机/播放模式/音量"与切槽位，
   串口不应出现 `task_wdt` / `lv_inv_area` 卡死。

### 踩坑提醒

- **别在 `set_xxx` 里回写 `set_xxx`**：`slot_persist_live()` 直接操作 NVS 与缓存，
  不重新走 setter（否则递归 + 重复通知）。
- **`config_store_set_slot_list_name()` 只改内存、不写 NVS**：名字与 `last_global`
  一起放在 `s_live` 里，随下一次 `config_store_save_active_slot()` 落盘。
  （旧实现把它写进"槽位缓存"并要求槽位已有数据，见 §七 bug A。）
- **`config_slots_switch()` 必须跑在 LVGL 上下文（持锁）**：切换后
  `config_store` 会请求一次 UI 刷新（**延迟派发**，见 §七 bug B）；`config_slots_switch`
  本身还会直接操作 Toast/音量/EQ，同样要在 LVGL 上下文中调用。
- **改 `config_slot_blob_t` 布局要 bump `SLOT_BLOB_VERSION`**，否则老 blob 会被按新
  布局解释（读出垃圾）。bump 后老槽位自动变"空槽位"，属于可接受的降级。
- **⚠ 本功能抬高 NVS 写密度后暴露了一个潜伏崩溃**（切槽位 → NVS 写 flash 期间命中
  `performance_monitor` 的 `ESP_TIMER_ISR` 采样回调 → `Cache error` panic 重启）：
  根因是该回调没进 IRAM，而 systimer 中断带 `ESP_INTR_FLAG_IRAM`、**关缓存期间
  屏蔽不掉**。已给回调加 `IRAM_ATTR` 修复，完整机理与两条规则见
  `PSRAM_TASK_STACK.md` §2.5。**今后凡新增"频繁写 NVS/flash"的功能，都要回头
  审一遍 ISR 回调是否都在 IRAM。**

---

## 七、2026-11 两个真机 bug 与修复（务必别回退）

### bug A：切走再切回"还是原来的数据"，关机再开机才"保存"

**现象**（用户实测）：改完当前配置直接切到别的槽位，那个槽位没被保存；切回来还是
原来的数据。只有 SAVE1（唯一一个已有数据的槽位）看起来"会保存"，其它槽位都要
"关机再开机一次"才保存。

**根因不是 NVS 写失败，而是"切走时存下去的是旧值"**——槽位里最显眼的字段是
**当前列表/歌曲**，而它有两个漏洞：

1. **从列表点歌不落盘**：`ui/playlist_page.c` → `player_controller_play_song()` →
   `CMD_PLAY_SONG`，旧实现只 `player_play_global()`，**不调用 `player_save_new_song()`**。
   于是 `config_store` 里的 `last_global` 仍是上一首；切走时存下去的就是上一首，
   切回来当然播回原来那首。
   而**伪关机走的是 `player_controller_save_last_position()`，它读控制器实时状态
   `s_cur_global`** → 所以"关机再开机一次就对了"。这条差异正是现象的来源。
2. **空槽位存不进列表名**：`config_store_set_slot_list_name()` 旧实现写在槽位缓存里、
   且要求"槽位已有数据"。空槽位第一次保存时名字被丢掉，弹窗只显示 `SAVE2`
   （看起来像"没保存"），要等下一次保存（如关机）才补上。
   SAVE1 之所以"正常"，是因为它是唯一一个**早就有数据（升级兼容的旧全局键/首次
   关机）**的槽位：`last_valid` 本来就有值，名字也在缓存里。

**修复**：
- `player_controller.c`：新增内部 `player_sync_config()`（歌曲 + 随机位置 + 列表名
  一起写进活跃槽位）。
- 新增公开接口 **`player_controller_sync_config()`**，`config_slots_switch()` 在
  `config_store_switch_slot()` **之前**调用它 → 存入旧槽位的一定是"此刻真正在播的歌"；
  伪关机保存也复用它（不再各写一遍）。
  ⚠ **不要**为了"记住歌曲"去给 `CMD_PLAY_SONG` / 切歌路径加落盘（第一版就是这么修的，
  随后按 §七.3 收敛掉了）：切换前的那一次同步已经把"切走时记住这首歌"完全覆盖。
- `config_store.c`：把 `last_list_name` 收进 `config_live_t s_live`（与 `last_global`
  成对打包/解包），`config_store_save_active_slot()` 不再从槽位缓存取名字，
  `set_slot_list_name()` 不再要求槽位已有数据（且仍然只写内存、不写 NVS）。
- `settings_page.c`：循环/随机格子的**当前值改读 `config_store`**（切槽位后控制器是
  异步应用的，紧接着点击会从上一个槽位的值起算）。

### bug A 的二次收敛（2026-11）：**不按曲落盘**

第一版修复给 `CMD_PLAY_SONG`（点歌）和 `CMD_RESTORE_CONFIG` 加了
`player_save_new_song()`，虽然有 bug 也修好了，但**点歌是全项目最高频的操作**：
每点一次歌就要把整个槽位 blob（352B + `nvs_commit`，关缓存 5~20ms）写一遍，还要
在 `player_ctrl` 那个只有 4KB 的栈上跑完整的 NVS/flash 路径（实测 HWM 只剩 1464B）。
既然"切走时记住"已由切换前的 `player_controller_sync_config()` 覆盖，这些写入纯属
冗余，已全部删除（`player_save_new_song()` 也随之消失）：

| 路径 | 现在是否写 NVS | 说明 |
| --- | --- | --- |
| 点歌（`CMD_PLAY_SONG`）/ 上一曲 / 下一曲 / 自然播完 / 坏曲跳过 | **否** | 只更新内存里的 `s_cur_global`；切槽位前同步 1 次、伪关机时同步 1 次就够 |
| 切槽位（`CMD_RESTORE_CONFIG`） | 否 | 目标槽位内容刚载入，写入的是同样的值；随机位置也随同步/切换落盘 |
| 暂停 / 停止 | 是 | 低频且是"暂停后直接断电"的最后保障，保留 |
| 切随机开关 / 开机恢复随机起点 | 是 | 用户动作 / 每次开机一次，保留 |
| 切槽位前、伪关机 | 是 | **唯一的两个"主动保存"时机**（`player_sync_config`） |

**代价（有意接受）**：非正常断电（拔电池、长按 A 键到 3s 由硬件硬切、崩溃重启）时，
"上次播放的歌曲"会回退到上一次落盘点（暂停/停止/切槽位/伪关机）——正常走
伪关机（睡眠定时、低电、A 键长按 1.2s）不受影响。**换来的收益**：点歌/翻页这条
最高频路径完全不写 flash，"切走再切回"的正确性却与之前一模一样。

> ✅ **这条行为已与用户确认为"预期"，不是缺陷**：原版 NightPlayer 就是这个语义
> ——安卓版清后台、强制停止、应用崩溃都**不保存**，只有正常退出才保存。本固件里
> "正常退出"对应**伪关机**（睡眠定时 / 低电 / A 键长按 1.2s），它一定会保存。
> ⚠ **所以今后不要把它当 bug 去"加固"**：不要再给点歌/翻页/自然播完加落盘，
> 也不要设计"崩溃前抢救一下"之类的机制。

同一轮里还给两个"位置类 setter"加了**等值短路**（`config_store_set_last_song` /
`config_store_set_shuffle_pos`：值+valid 标志都没变就直接返回）。原因是从真机日志
看到一次切槽位会连着打 3 条 `SAVE2 saved to NVS`——分别是同步时的 `last_song`、
`set_shuffle_pos`（RESTORE 里先写快照值、再写 adopt 后的值，两次是同一个数），
全是冗余。现在一次切槽位**只写 1 次**槽位 blob（由 `config_store_switch_slot`
在换下标前完成），加 1 次 1 字节的 `slot_active`。

### bug B：切配置/切随机播放时死机（task_wdt，卡在 lv_inv_area）

**现象**（用户实测串口）：
```
E task_wdt: CPU 0: player_ctrl
--- lv_inv_area (lv_refr.c:302)
--- lv_label_refr_text / lv_label_set_text (lv_label.c)
--- settings_refresh_loop (settings_page.c:122)
--- settings_config_changed_cb (settings_page.c:300)
--- config_notify (inlined) ← config_store_set_shuffle (config_store.c:980)
--- player_cmd_set_shuffle (inlined) ← player_task (player_controller.c:659)
```

**根因**：`config_store` 的 `set_xxx` 旧实现**在调用者线程里同步执行变更回调**，
而回调方（设置页/状态栏/通知栏）直接改 LVGL 控件。`player_ctrl` 任务里调
`config_store_set_shuffle()` 时，回调就在**非 LVGL 任务**里改 LVGL 对象树，与同核
（Core0）的 LVGL 任务渲染并发 → 对象树/脏区链表被破坏，卡死在 `lv_inv_area` 死循环，
IDLE0 得不到调度 → task_wdt。

> 不是"NVS 写 flash 期间 PSRAM 不可用"：那条路只会 panic（见 `PSRAM_TASK_STACK.md`
> §2.5），不会卡在 LVGL 的脏区遍历里；而且调用栈明确是**回调链**进了 LVGL。

**修复**：`config_store` 的通知改为**延迟派发**——
`set_xxx` 只置 `s_notify_pending`（任何任务都可以安全调用），
`config_store_notify_dispatch()` 由 `ui/app.c` 的一个 **100ms lv_timer** 在
LVGL 任务上下文里统一调用（`config_notify_timer_cb`）。
延迟只影响"数值/文字标签刷新"，音量/EQ/亮度等硬件应用仍由调用方立即执行。
因此：**不要再把回调改回 `set_xxx` 内同步执行**，也不要在 `set_xxx` 里调任何 LVGL API。
