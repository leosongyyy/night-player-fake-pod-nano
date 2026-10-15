# 虚拟滚动列表（virtual_list）使用手册

> ⚠ **LVGL 9 迁移提醒（2026-10）**：本文写于 LVGL 8.4 时代。**v9 的坐标天然是
> `int32_t`，`CONFIG_LV_USE_LARGE_COORD` 已删除**，所以"必须开 LARGE_COORD"这类
> 结论不再适用（长列表依旧可靠）；点击穿透的实现也从"`LV_EVENT_HIT_TEST` 回调改
> res"改成了仅 `clear_flag(CLICKABLE)`。详见 **`LVGL9_MIGRATION.md`** §2/§9。

通用虚拟滚动列表组件，用**固定数量的控件**承载海量数据：滚动时把滚出视口的
cell 复用（重新赋值）到新条目上，从而以十几个控件显示几千上万条数据，同时保留
LVGL 原生滚动条、惯性滑动与点击选中。

- 文件位置：`main/core/ui/virtual_list.h` / `main/core/ui/virtual_list.c`
- 当前用例：目录列表（12 字节条目）、歌曲列表（8 字节条目）
- 依赖（v8 时代）：`CONFIG_LV_USE_LARGE_COORD=y`（**必须**，见"踩坑记录"第 1 条）；
  **LVGL 9 起该配置项已不存在，坐标本身就是 int32_t**

---

## 1. 数据要求

列表数据是一个**连续内存块**，每条 `item_size` 字节、共 `item_count` 条。组件不
解析数据内容，只按 `data + index * item_size` 算出条目指针交给绑定回调。

- 数据**必须静态**（同一 index 内容不变）。滚动时组件只重绑滚出/滚入视口的
  cell，常驻 cell 不会被重复赋值（见"工作原理"）。若数据会变化，改完后调用
  `virtual_list_refresh()` 强制重绑。
- 数据首地址：用 `playlist_dir_at(0)` / `playlist_song_at(start)` 之类取第 0 条
  即可；条目数为 0 时传 `NULL` / `0`，列表显示为空。

---

## 2. API 参考

```c
/* 创建列表，返回 lv_obj_t* 句柄，后续接口都用它 */
lv_obj_t *virtual_list_create(lv_obj_t *parent,
                              lv_coord_t w, lv_coord_t h,       /* 列表尺寸 */
                              lv_coord_t item_height,           /* 每条固定高度 */
                              uint32_t item_count,              /* 条目总数 */
                              const void *data, uint32_t item_size, /* 数据源 */
                              virtual_list_bind_cb_t bind_cb,   /* 绑定回调 */
                              void *user_data);                 /* 回调上下文 */

/* 滚动到指定下标（顶部对齐），越界自动收敛到末尾 */
void virtual_list_scroll_to_index(lv_obj_t *list, uint32_t index,
                                  lv_anim_enable_t anim);

/* 刷新：强制重新渲染当前可见 cell（数据变更后调用） */
void virtual_list_refresh(lv_obj_t *list);

/* 选中：-1 取消选中；越界当作取消 */
void virtual_list_set_selected(lv_obj_t *list, int32_t index);
int32_t virtual_list_get_selected(lv_obj_t *list);

/* 设置点击选中回调 */
void virtual_list_set_selected_cb(lv_obj_t *list,
                                  virtual_list_selected_cb_t cb);

/* 跑马灯开关：true 超长文本循环滚动，false 裁剪（默认 true） */
void virtual_list_set_marquee(lv_obj_t *list, bool enable);

/* 获取当前第一个可见条目下标（调试/定位用） */
uint32_t virtual_list_get_first_index(lv_obj_t *list);

/* 获取条目固定高度（像素；NULL/非法返回 0）。
   供外部"按整行"驱动滚动——ClickWheel 用它把滚动量取整到行高的整数倍，
   使列表停在整数行上（见 CLICK_WHEEL_USAGE.md 2.3 行对齐步进） */
lv_coord_t virtual_list_get_item_height(lv_obj_t *list);
```

### 回调类型

```c
/* 绑定回调：组件在 cell 需要填充/重用时调用，负责设置文本与颜色 */
typedef void (*virtual_list_bind_cb_t)(lv_obj_t *cell, uint32_t index,
                                       const void *item, void *user_data);

/* 选中回调：用户点击某条并选中后触发 */
typedef void (*virtual_list_selected_cb_t)(uint32_t index, void *user_data);
```

- `cell`：组件内部是 `lv_label`，文本/颜色都设置到它。
- `item`：条目原始数据指针，**已由组件算好**（= `data + index*item_size`），
  直接强转为你的条目结构体即可。
- `user_data`：创建时传入的上下文，用来存放取字符串/判变色所需内容（如
  song_start、数据源标识等）；用不到就传 `NULL`。

---

## 3. 使用示例

### 3.1 目录列表（12 字节条目）

```c
#include "core/storage/playlist.h"
#include "core/ui/virtual_list.h"

static void dir_bind_cb(lv_obj_t *cell, uint32_t index,
                        const void *item, void *user_data)
{
    (void)user_data;
    const playlist_dir_entry_t *dir = (const playlist_dir_entry_t *)item;
    lv_label_set_text(cell, playlist_get_string(dir->name_str));

    /* 变色：当前播放目录黄色，其余白色 */
    player_controller_status_t st;
    player_controller_get_status(&st);
    bool is_playing = st.has_song && (index == st.cur_dir);
    lv_obj_set_style_text_color(cell,
        is_playing ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
}

static void dir_selected_cb(uint32_t index, void *user_data)
{
    (void)user_data;
    /* TODO: 进入该目录的歌曲列表 */
}

void create_dir_list(lv_obj_t *scr)
{
    uint32_t count = playlist_dir_count();
    const playlist_dir_entry_t *dirs =
        (count > 0) ? playlist_dir_at(0) : NULL;

    lv_obj_t *list = virtual_list_create(scr,
                                         EXAMPLE_LCD_H_RES,
                                         EXAMPLE_LCD_V_RES - 46, /* 状态栏下方 */
                                         44,                     /* 条目高度 */
                                         count,
                                         dirs, sizeof(playlist_dir_entry_t),
                                         dir_bind_cb, NULL);
    lv_obj_set_pos(list, 0, 46);
    virtual_list_set_selected_cb(list, dir_selected_cb);
}
```

### 3.2 歌曲列表（8 字节条目）

```c
static void song_bind_cb(lv_obj_t *cell, uint32_t index,
                         const void *item, void *user_data)
{
    const playlist_song_entry_t *song = (const playlist_song_entry_t *)item;
    lv_label_set_text(cell, playlist_get_string(song->name_str));

    /* user_data 传的是该列表第一首歌的全局下标，用于判断当前播放 */
    uint32_t song_start = (uint32_t)(uintptr_t)user_data;
    player_controller_status_t st;
    player_controller_get_status(&st);
    bool is_playing = st.has_song && (st.cur_global == song_start + index);
    lv_obj_set_style_text_color(cell,
        is_playing ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
}

static void song_selected_cb(uint32_t index, void *user_data)
{
    uint32_t song_start = (uint32_t)(uintptr_t)user_data;
    uint32_t global = song_start + index;   /* 全局下标，可用于播放 */
    /* TODO: player_controller_play_song(global); */
}

void create_song_list(lv_obj_t *scr, uint32_t dir_index)
{
    const playlist_dir_entry_t *dir = playlist_dir_at(dir_index);
    if (dir == NULL) return;

    uint32_t song_count = dir->song_count;
    uint32_t song_start = dir->song_start;
    const playlist_song_entry_t *songs =
        (song_count > 0) ? playlist_song_at(song_start) : NULL;

    lv_obj_t *list = virtual_list_create(scr,
                                         EXAMPLE_LCD_H_RES,
                                         EXAMPLE_LCD_V_RES - 46,
                                         56,                     /* 条目高度 */
                                         song_count,
                                         songs, sizeof(playlist_song_entry_t),
                                         song_bind_cb,
                                         (void *)(uintptr_t)song_start);
    lv_obj_set_pos(list, 0, 46);
    virtual_list_set_selected_cb(list, song_selected_cb);
}
```

---

## 4. 工作原理

1. **透明占位对象**：列表内放一个高度 = `item_count × item_height` 的透明对象，
   撑开真实内容高度，从而驱动 LVGL 原生滚动条（这也是必须开大坐标的原因）。
2. **固定数量 cell**：只创建 `可见数 + 2` 个 `lv_label` 作为复用 cell。
3. **滚动重绑定（逐个进出）**：监听 `LV_EVENT_SCROLL`，按 `scroll_y / item_height`
   算出视口覆盖的条目范围 `[first, last]`。cell 锚定在内容坐标
   （`index * item_height`），滚动时仍留在视口内的 cell **完全不动**；只有滚出
   视口的 cell 被回收（隐藏置空闲），重新定位到另一端新进入视口的条目上并重绑。
4. **复用粒度**：每跨一个条目高度，只有 1 个 cell 被重绑（滚出的那个回收复用给
   进入的条目），其余 cell 的位置/文本/颜色/跑马灯全部保留——不是全量重绑。
5. **按下反馈（无常驻选中背景）**：点击反馈是按下瞬间的黄色背景
   （`LV_STATE_PRESSED`，0x7f7f00），松开自动恢复透明。不再有点击后常驻的选中
   背景；"当前播放中"的条目由绑定回调以文字黄色标注。
6. **跑马灯**：cell 默认 `LV_LABEL_LONG_SCROLL_CIRCULAR`。因常驻 cell 滚动时不被
   重绑（不 `set_text` / `set_pos`），跑马灯动画在滚动前后连续不断；只有新进入
   视口的 cell 会从头开始滚动（属正常表现）。
   ⚠ 滚动的**节奏**（起始延迟 / 滚动轮数 / 轮间停顿）不在这里定义，统一由
   `core/ui/global_style` 管理（详见 `MARQUEE_STYLE.md`）：
   - 行文本 label 由本组件在创建时 `global_style_marquee_attach()` **登记一次**；
   - 绑定回调里设置文本**必须**用 `global_style_marquee_set_text(cell->text, ...)`，
     **不要**裸调 `lv_label_set_text()`——内容相同的重设也会让 lv_label 重建滚动
     动画，表现为"跑马灯莫名回到起点"；而曲目/目录变化时本组件会重绑**全部可见行**
     （只为刷"正在播放"高亮），一裸调就会看到"切歌瞬间整屏跑马灯一起重置"；
   - `vlist_bind_row()` 故意**不碰**跑马灯（不重跑、不重设参数）。

---

## 5. 写死的样式与常量（`virtual_list.c`）

| 项 | 值 | 说明 |
|---|---|---|
| 字体 | `UI_SYSTEM_FONT` | 跟随 `CONFIG_UI_SYSTEM_FONT_FULL` 切换完整/精简 |
| 按下背景色 | `0x7f7f00`（50% 黄 + 50% 黑） | 仅按下瞬间显示（`LV_STATE_PRESSED`），松开透明 |
| 条目左右内边距 | `VLIST_PAD_H = 16` | 右侧留白避免压住滚动条 |
| 条目上下内边距 | 顶部 `(item_height − 行高)/2`，底部 0 | 顶部留白使文本视觉居中；底部 0 保证内容区 ≥ 行高不裁切 |
| 列表背景 | 黑底、无边框、无 padding | — |

两处使用（目录/歌曲）如需不同样式，改这里或改成参数即可。

---

## 6. 踩坑记录（务必遵守）

1. **必须开 `CONFIG_LV_USE_LARGE_COORD=y`**：LVGL 8 默认 `lv_coord_t` 是 int16
   （±32767），而真实内容高度 = 条目数 × 条目高会远超此值（1085 首 × 56px ≈
   60KB）。不开大坐标时滚动条无法按真实高度工作。已在 `sdkconfig` /
   `sdkconfig.defaults` 中开启。

2. **cell 定位用内容坐标，不是视口坐标**：`lv_obj_set_pos(cell, 0, index*item_height)`
   设的是相对列表内容区的坐标（随滚动整体平移）。若误写成视口坐标
   （`i*item_height - offset`），滚动时 cell 会越滚越偏、留下空白。

3. **`set_pos` 会重置跑马灯**：`lv_obj_set_pos` → `lv_obj_set_style_x/y` → 触发
   `LV_EVENT_STYLE_CHANGED` → `lv_label_refr_text` → 删除跑马灯动画、offset 归零。
   因此组件只对**回收进入视口**的 cell 调用 `set_pos`，常驻 cell 从不触碰，
   跑马灯得以连续（旧实现因滚动时全量重绑，被迫在滚动期间临时禁用跑马灯）。
   **不要**在外部对 cell 反复 `set_pos` / `set_style_x`。

4. **事件冒泡需 `LV_OBJ_FLAG_EVENT_BUBBLE`**：LVGL 8 的事件默认不冒泡到父对象
   （除 `CHILD_CREATED/DELETED` 外）。点击回调注册在 list 上，cell 必须带
   `LV_OBJ_FLAG_EVENT_BUBBLE` 才能让 `LV_EVENT_CLICKED` 冒泡上来。

5. **字体行高不能超过 cell 内容区**：完整字体 `system_global_font` 的
   `line_height = 33`，精简字体是 26。cell 内容区高度 = `item_height - 顶部内边距`
   （底部内边距为 0），顶部内边距 = `(item_height - 行高)/2`。
   若 `line_height > 内容区高度`，`LV_LABEL_LONG_SCROLL_CIRCULAR` 会误判纵向超长、
   变成**上下滚动**。当前歌曲列表用 56px（内容区 45px），目录列表 52px（内容区
   43px，配合精简字体 26 行高足够；用完整字体请 ≥ 52px）。

6. **数据必须静态**：见第 1 节。数据变动后必须手动 `virtual_list_refresh()`。

7. **线程安全**：LVGL API 非线程安全，创建/操作列表必须在持有
   `lvgl_port_lock()` 的情况下进行（LVGL 任务/事件回调内已持锁，无需重复加锁）。

---

## 7. 内存表现（实测）

1085 首超长歌曲列表、60 秒连续滚动实测：heap 稳定在基线 ±1KB 内波动（无泄漏），
iram 同步稳定；cell 复用与 `set_text` 的分配/释放完全配对，连续滚动不累积内存。

（完整字体固件因字体只读数据映射进 PSRAM，heap 基线比精简字体低约 2.5MB，属
静态占用，与滚动无关。）
