# PLAYER_CONTROLLER_SUMMARY — NightPlayer 播放控制器

> 本文档汇总 FakePodNanoNightPlayer 中**播放控制器（controller）**的设计、实现、接口与后续规划。
> 它是 NightPlayer 播放系统三大组成部分（播放引擎核心 → 播放列表提供者 → 播放控制器）中的第三部分，
> 位于播放引擎 `mp3_player` 与播放列表提供者 `playlist` 之上，作为**独立后台服务**负责播放控制与自动续播。
>
> 前置依赖：`PLAYLIST_SCAN_SUMMARY.md`（PLST 扫描与存储）与 `BOOT_FLOW_SUMMARY.md`（开机流程常规界面入口）。

---

## 1. 目标与背景

- 设备：ESP32-S3（16MB Flash，8MB PSRAM），460x460 AMOLED，SD 卡。
- 需求：由控制器负责 **pause / play / prev / next** 及**循环模式**控制：
  - 默认从**第一个目录的第一首**音乐开始播放。
  - 三种循环模式：**全部循环 / 单曲循环 / 列表循环**。
- 控制器为**独立 FreeRTOS 任务（固定在 Core0）**，通过命令队列串行处理控制请求；
  以**静态变量**保存状态供前端界面读取，以**方法**供前端界面调用（前端即后续要写的界面）。

---

## 2. 设计决策（含用户澄清）

### 2.1 循环模式语义（关键，已与用户确认）

三种循环模式主要决定**歌曲自然播完时**的自动续播行为；手动上一曲/下一曲另有规则：

| 模式 | 自然播完自动续播 | 手动上一曲/下一曲 |
|---|---|---|
| **全部循环** `PLAYER_LOOP_ALL` | 在所有目录/歌单连成的**整条序列**中前进；到达整库最后一首 → 绕回第一首 | 前进/后退到相邻曲目；到达整库末尾/开头 → 绕回开头/末尾 |
| **列表循环** `PLAYER_LOOP_LIST`（默认） | 在**当前列表**内前进；播到当前列表末尾 → 回到本列表第一首 | 前进/后退到相邻曲目；到达**整库**末尾/开头 → 停在原地（不越界） |
| **单曲循环** `PLAYER_LOOP_SINGLE` | **循环当前曲**（重播同一首） | **正常跳到相邻曲目**（用户确认：不被困在当前曲） |

> 关于"全部循环"的 向前/向后 方向：**用户确认按常理执行**，即
> **下一曲=前进**（最后一首 → 下一个播放列表；若已是最后一个列表最后一首 → 回到第一个列表第一首），
> **上一曲=后退**（第一首 → 上一个播放列表；若已是第一个列表第一首 → 跳到最后一个列表最后一首）。

### 2.2 位置模型

- 全部目录/歌单按 PLST 顺序连成**一条扁平序列**（`dir[0]` 的歌曲 → `dir[1]` 的歌曲 → …）。
- 用**全局歌曲下标** `cur_global ∈ [0, total_song_count)` 表示当前曲目，直接作为
  `playlist_song_at()` / `playlist_build_path()` 的 `song_idx` 使用。
- 由全局下标反查所在目录（`player_find_dir` 顺序线性查找，目录数有限）。
- 空目录/空歌单在扁平序列中自然不占位置，不会被选中。

### 2.3 独立 Core0 任务的原因

- `mp3_player_play()` 在切歌时会阻塞等待旧曲释放资源（最长 2s）。
- 若由 LVGL/UI 线程直接调用会卡住界面；故放在**独立控制器任务（Core0）**里，
  解码播放本身在 Core1 的 `mp3_play` 任务中，不与之竞争。
- 控制命令经**命令队列**投递到控制器任务串行处理，线程安全、非阻塞。

### 2.4 停止回调与自动续播的区分

- `mp3_player` 的停止回调 `mp3_player_stopped_cb(bool is_from_user)`：
  - `is_from_user == false`：**自然播完** → 控制器收到 `CMD_FINISHED`，按循环模式自动续播。
  - `is_from_user == true`：用户主动停止（含手动切歌时 `mp3_player_play` 内部停旧曲）→
    **忽略**，避免与已投递的 next/prev/play_song 命令重复跳曲。

---

## 3. 代码模块

| 文件 | 作用 |
|---|---|
| `core/player/player_controller.h` | 头文件：循环模式枚举、状态快照结构、全部对外接口声明 |
| `core/player/player_controller.c` | 实现：命令队列 + Core0 任务 + 循环逻辑 + 静态状态 |
| `main/CMakeLists.txt` | 注册 `core/player/player_controller.c` |
| `main/ui/boot_flow.c` | 在 `boot_enter_normal()` 中调用 `player_controller_init()` 启动控制器 |

---

## 4. 对外接口

### 4.1 状态读取（供前端界面）

```c
esp_err_t player_controller_init(void);          // 初始化 + 自动播放第一目录第一首
bool     player_controller_is_ready(void);
void     player_controller_get_status(player_controller_status_t *out);
player_loop_mode_t player_controller_get_loop_mode(void);
```

`player_controller_status_t` 字段：

```c
typedef struct {
    mp3_player_state_t playing;   /* IDLE / PLAYING / PAUSED */
    player_loop_mode_t loop_mode; /* 当前循环模式 */
    bool has_song;                /* 是否有可播放歌曲 */
    uint32_t cur_global;          /* 当前歌曲全局下标 */
    uint32_t cur_dir;             /* 当前目录/歌单下标 */
    uint32_t cur_song_in_dir;     /* 当前歌曲在目录内序号（0 起） */
    uint32_t dir_song_count;      /* 当前目录歌曲总数 */
    uint32_t dir_count;           /* 目录/歌单总数 */
    uint32_t total_song_count;    /* 歌曲总数 */
    uint32_t position_s;          /* 当前播放位置（秒） */
    uint32_t duration_s;          /* 当前歌曲总时长（秒） */
    char dir_name[128];           /* 当前目录/歌单名 */
    char song_name[256];          /* 当前歌曲文件名（含扩展名） */
} player_controller_status_t;
```

### 4.2 控制方法（供前端界面 / 按键调用，线程安全、非阻塞）

```c
void player_controller_play(void);          // 暂停中恢复；未播则从头播当前曲
void player_controller_pause(void);         // 暂停
void player_controller_toggle(void);        // 播放/暂停切换
void player_controller_next(void);          // 下一曲（按循环模式处理边界）
void player_controller_prev(void);          // 上一曲
void player_controller_seek(uint32_t s);    // 跳转（秒）
void player_controller_play_song(uint32_t global_idx); // 播放指定全局下标（未来列表 UI 用）
void player_controller_stop(void);          // 停止
void player_controller_set_loop_mode(player_loop_mode_t mode);
```

---

## 5. 内部实现要点

- **命令队列**：`xQueueCreate(8, sizeof(cmd_msg))`，投递用非阻塞 `xQueueSend(..., 0)`（队列满则丢弃，避免阻塞调用方）。
- **命令集**：`CMD_PLAY/PAUSE/TOGGLE/NEXT/PREV/SEEK/PLAY_SONG/FINISHED/STOP`。
- **控制器任务**：`xTaskCreatePinnedToCore(player_task, "player_ctrl", 4096, NULL, 5, &s_task, 0)`，循环 `xQueueReceive(portMAX_DELAY)` 串行处理。
- **自然播完回调**：`player_stopped_cb` 仅在 `!is_from_user` 时投递 `CMD_FINISHED`。
- **自动续播**：`CMD_FINISHED → player_play_global(player_auto_next())`。
- **手动切歌**：`CMD_NEXT → player_manual_next()`，`CMD_PREV → player_manual_prev()`。
- **路径缓冲**：`s_path_buf[512]`（静态 BSS），`playlist_build_path()` 填充后传给 `mp3_player_play()`（该接口要求路径为常驻内存，静态缓冲满足）。
- **位置元信息**：`player_update_meta()` 更新 `cur_dir / cur_song_in_dir / dir_song_count / dir_name / song_name / path`，仅控制器任务写入。

---

## 6. 验证结果

- ✅ `idfc build` 编译链接通过（`player_controller.c`、`boot_flow.c`、`main.c` 均编译，最终固件 `FakePodNanoNightPlayer.bin` 生成成功）。
- ⏳ **尚未真机烧录验证**（用户需回家换电脑继续，暂未烧录）。
- 可用串口：**COM3 / COM4 / COM12**（此前真机验证用 COM12）。

---

## 7. 当前状态与后续

### 已完成
- [x] 控制器独立 Core0 任务 + 命令队列（串行、非阻塞控制接口）
- [x] 默认从第一个目录第一首自动播放
- [x] 三种循环模式（全部 / 单曲 / 列表），默认**列表循环**
- [x] 全部循环按常理：下一曲=前进跨列表并整库绕回；上一曲=后退跨列表并整库绕回
- [x] 单曲循环：自然播完重播当前曲；手动仍可切相邻曲目
- [x] 列表循环：自然播完在当前列表内前进、播完回本列表第一首
- [x] 静态状态 `get_status` + 控制方法，供前端界面调用
- [x] 自然播完自动续播（区分用户主动停止）
- [x] 编译通过，接线 CMakeLists 与 boot_flow

### 待办 / 规划
- [ ] **真机烧录验证**（COM12 或其它）：自动播放、三种循环、上一曲/下一曲、seek、暂停恢复
- [ ] 前端播放界面（主界面显示曲名/进度/控制按钮，目录→歌曲列表→播放）
- [ ] **随机播放**（用户已有"绝妙的点子"，后续单独完成）
- [ ] 循环模式**NVS 持久化**（用户确认：后续统一做 nvs 保存与读取，当前先固定列表循环）
- [ ] 循环模式 / 播放进度的界面切换入口（后续随 UI 一起做）
- [ ] （可选）切歌失败（文件缺失/采样率不支持）时的自动跳过策略

---

*文档整理时间：NightPlayer 播放控制器阶段。*
