# 虚拟 ClickWheel 大列表速滑组件（click_wheel）

> 对应 `FUTURE_PLAN.md` 第 24 点，已完成（见 `DONE_PLANS.md`）。
> 源码：`main/core/ui/click_wheel.{c,h}`；接入方：`main/ui/dir_list.c`、
> `main/ui/playlist_page.c`。

## 1. 需求与交互

- 目录页 / 歌曲页右下角悬浮一个**黄色圆形按钮**；
- **按住**后在屏幕上画圈 → 快速翻页：
  - 顺时针（角度增大）**向下**滚，逆时针**向上**滚；
  - 转满一圈（360°）= 滚动**内容总高度的 10%**；
  - 画圈期间屏幕中央出现**12 个灰色小圆点围成一圈**（半径 130px、直径 20px，
    从正上方起均分），给手指一个旋转参照；松手立即消失；
- **直接点击**（按住期间没转动）→ 弹出使用说明 Toast；
- 按钮**只在列表超过 10 页（72 条）时显示**（`CW_MIN_ITEMS = 73`）；
- 功能可在**设置菜单 →「虚拟 ClickWheel」**行开关（NVS 配置 `click_wheel`，
  **默认开**），点击立即生效（无需重启）。

## 2. 关键设计

### 2.1 按钮挂在 `lv_layer_top()`（顶层），必须断开滚动链

**第一版把按钮放在 tab 页容器里，实测无法画圈**：从按钮起手拖动时，LVGL 的
滚动链（`SCROLL_CHAIN_HOR/VER`，`lv_indev_scroll.c: find_scroll_obj`）会从
按下的对象一路向上找到**可横向滚动的 tabview**，把这次按压判定为"翻页
滚动"，并给按钮发 `LV_EVENT_PRESS_LOST` → 画圈状态被清掉，表现为
"拖动变成了页面左右滑动"。

修正（两个要点）：

1. **父对象改为 `lv_layer_top()`**（系统最顶层，`lv_hal_disp.c` 中
   `lv_obj_create(NULL)` 创建，不随屏幕内容滚动）：
   - 顶层对象不在 tabview 的祖先链上，滚动链找不到它，事件不会外泄；
   - 顶层不随页面横向滑动，按钮位置稳定；
   - 与 Toast 同层共存（Toast 后创建则显示在按钮之上）；
   - **同层内靠创建顺序定 Z 轴**：下拉控制中心面板同样挂在顶层且创建更晚
     → 展开时自然遮挡按钮并接管其区域触摸（见踩坑 8）。
2. **按钮自身清除 `SCROLL_CHAIN_HOR` / `SCROLL_CHAIN_VER`**（图标 label 同样
   清除）：即使滚动链走到按钮，`SCROLLABLE == false` + 链断开会让
   `find_scroll_obj` 立即 break，不再向父级找滚动对象。

因为挂在顶层，**显隐必须由外部驱动**（顶层不知道当前是哪一页）：
`click_wheel_set_tab()`（主界面切页时调用）+ `click_wheel_set_list()`
（各页列表构建时登记滚动目标）+ `click_wheel_set_overlay()`
（设置菜单开合时调用，否则按钮会浮在菜单之上并拦截该区域触摸；
下拉控制中心无需此调用——它同在顶层且创建更晚，天然在按钮之上）。

定位：`lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -CW_MARGIN, -CW_MARGIN_BOTTOM)`，
直径 56px，右/下边距各 **32px**（`CW_MARGIN` / `CW_MARGIN_BOTTOM`，按用户
要求在原 16px 基础上各加 16px）。

### 2.2 角度以**屏幕中心**为圆心

```c
angle = atan2f(y - H/2, x - W/2) * 180 / M_PI;   // 屏幕坐标系：x 右 y 下
```

- 屏幕坐标系下角度顺时针增大 → 顺时针 = 正角度；
- 圆心固定为屏幕中心（需求原意，不要改成按钮中心——曾试过按钮中心，
  被用户否掉）；手指绕右下角按钮转圈时，相对屏幕中心的角度变化被半径
  比例缩小（绕按钮一圈 ≈ 屏幕中心 36°），转一圈的滚动量由 `CW_TURN_RATIO`
  统一标定；
- 每帧角度差需归一化到 (-180, 180]：

```c
while (delta >  180.0f) delta -= 360.0f;   // 处理 179° → -179° 的跨界
while (delta < -180.0f) delta += 360.0f;
```

未归一化时，手指经过 0°/360° 分界线会产生 ±360° 的跳变，列表会瞬间飞到底。

### 2.3 滚动量换算与方向

```
内容总高度 content_h = 视口高度 + (scroll_top + scroll_bottom)   // 反推，不依赖列表内部
每度滚动量 px_per_deg = content_h * 0.1 / 360
滚动量 px = -角度差 × px_per_deg
```

⚠ 取负号的推导（别搞反）：屏幕坐标系里**顺时针 = 角度增大**；而
`lv_obj_scroll_by(dy>0)` 表示**内容上移 = 视口向下**。需求是"顺时针向下滚"，
所以 顺时针（正角度差）→ 需要 dy 为正 → 公式带负号。

- 单帧钳制 `±CW_MAX_STEP_PX (400px)`：触摸坐标偶发跳变（如 180°）时不会乱飞；
- 小于 1px 直接跳过，避免无意义的行重绑；
- 用 `lv_obj_scroll_by_bounded()` 而非 `lv_obj_scroll_by()`：自带边界钳制，
  快速转动到顶/底不会出现过冲回弹；
- `LV_ANIM_OFF`：转动过程要求跟手，动画会累积延迟。

### 2.4 触摸坐标来源（重点）

LVGL 的按钮事件（`LV_EVENT_PRESSED` / `PRESSING`）**不带坐标**，而
`lv_indev_get_point()` 只能在事件回调里取当前点。要"按住期间持续跟踪角度"，
两条路：

1. 用 `LV_EVENT_PRESSING` 每帧取一次 `lv_indev_get_point()`；
2. 复用 `lvgl_port` 的触摸订阅回调（`lvgl_port_register_touch_cb`）。

本实现选 **方案 2**：与 LVGL 输入设备共用同一份触摸数据（含同一份 29/30
坐标校准，见 `lvgl_port.c`），回调运行在 LVGL 任务上下文（已持锁），
可以直接调 LVGL API。好处：

- 坐标与 LVGL 判定"是否点在按钮上"完全一致，不会出现"LVGL 认为按在按钮上、
  角度却按另一套坐标算"的偏差；
- 按住期间手指**可以移出按钮范围**（画大圈），不受 LVGL 命中测试限制。

⚠ **不要在回调里加"坐标必须在按钮附近"的过滤**（第一版踩坑）：按钮只有
56px，画圈时手指必然移出，一过滤就只剩按钮内那点角度变化，表现为
"每次只能拖动一点点"。起手是否按在按钮上已由 `LV_EVENT_PRESSED` 保证
（只有按在按钮上才进入画圈状态），之后整根手指的移动都照常采样。

⚠ 代价：每个触摸订阅占一个 `TOUCH_CB_MAX`（=4）槽位。当前占用：
`touch_test`（开发者选项开启时）+ `power_service` + ClickWheel = 最多 3 个
（ClickWheel 全局单例，只注册一次）。后续若再加触摸订阅者，需把
`TOUCH_CB_MAX` 调大。

### 2.5 画圈指示：12 个灰色圆点

按住按钮期间，屏幕中央出现 12 个灰色小圆点围成一圈（`cw_ring_dots_create`）：

- 圆心 = 屏幕中心（与角度计算的圆心一致），半径 `CW_RING_RADIUS = 130px`，
  圆点直径 `CW_RING_DOT_SIZE = 20px`、颜色 `0x808080`，从正上方（12 点钟）
  起均分 12 份；
- 与按钮同层（`lv_layer_top()`），常驻隐藏、按下即显示、松手 / PRESS_LOST
  隐藏（`cw_ring_dots_set_visible`，幂等，状态未变不遍历）；
- 圆点 `CLICKABLE/SCROLLABLE` 均清除 + `LV_EVENT_HIT_TEST` 透传：纯视觉指示，
  不拦截触摸、不影响按钮与列表操作。

### 2.6 点击 vs 画圈的区分

- `LV_EVENT_PRESSED`：记录起始角度，`rotated = false`，进入画圈状态；
- 画圈期间任何一次角度差非 0 → `rotated = true`；
- `LV_EVENT_RELEASED`：`rotated == false` 才弹使用说明。

不依赖 LVGL 的长按时间（`long_press_time`，默认 460ms）：用户"按住不动 1 秒
再松开"仍算点击，符合"直接点击会弹出使用说明"的预期。

`LV_EVENT_PRESS_LOST`（手指滑出按钮被其它控件接管滚动等）→ 立即退出画圈
状态，避免后续触摸继续按旧角度滚列表。

### 2.7 全局单例 + 由页面驱动显隐

按钮是**全局单例**（`s_btn` + 每页各一份 `s_lists[tab]/s_counts[tab]`），
不用每页各建一个：

- `click_wheel_set_list(tab, list, count)`：页面构建/重建列表时登记目标
  （重建前先传 `NULL` 解除绑定，避免悬空指针）；
- `click_wheel_set_tab(tab)`：`main_tabs_sync()` 切页时调用；
- `click_wheel_set_overlay(bool)`：`settings_menu_show/hide()` 调用；
- 三者共同决定显隐：`show = !overlay && tab < 2 && list != NULL && count >= 73`。

按下瞬间还会再 `cw_target()` 锁定一次滚动目标，防止画圈途中切页/换列表
导致滚错列表。

⚠ 为此把 `lvgl_port_unregister_touch_cb(cb)` 的签名改为
`lvgl_port_unregister_touch_cb(cb, user_ctx)`（按 cb+ctx 精确注销），
避免将来多实例时误摘订阅。

## 3. API

```c
lv_obj_t *click_wheel_create(void);                              /* 全局单例，创建一次 */
void      click_wheel_set_list(uint8_t tab, lv_obj_t *list, uint32_t item_count);
void      click_wheel_set_tab(uint8_t tab);
void      click_wheel_set_overlay(bool visible);
void      click_wheel_set_enabled(bool on);                      /* NVS 开关，默认开 */
```

- `create`：在 `main_ui_create()` 中调用（四个页面创建之后、首次
  `main_ui_switch_tab` 之前）；
- `set_list`：`dir_list.c`（tab 0）与 `playlist_page.c`（tab 1）在列表
  构建完成后调用，销毁列表前先传 `NULL`；
- `set_tab`：`main_ui.c` 的 `main_tabs_sync()`；
- `set_overlay`：`settings_menu.c` 的 `settings_menu_show/hide()`；
- `set_enabled`：`main_ui_create()` 创建后按 `config_store_get_click_wheel()`
  初始化；`settings_menu.c` 的「虚拟 ClickWheel」行切换后立即调用（关闭时
  隐藏按钮并终止进行中的画圈、收起指示圆点）；
- 全部接口需持 LVGL 锁。

### 3.1 功能开关（NVS）

`config_store` 新增 `click_wheel`（键 `clkwheel`，uint8，**默认 1 = 启用**）。
显隐判定因此变成四个条件同时满足：
`启用 && !覆盖层 && 当前页是列表页 && 条目数 ≥ 73`（`cw_sync_visible`）。

`core/ui/click_wheel.c` **不依赖 config_store**（保持 core/ui 与应用配置解耦）：
开关值由 UI 层传入，切换由设置菜单行回调驱动。

⚠ 设置菜单根页面因此多了一行（共 6 行），实测根页面内容高度 357px、6 行需
426px → **根页面出现滚动条**（用户确认可接受，不再压缩行距）。

## 4. 接入方式（三处）

`main_ui.c`：

```c
/* 四个页面创建完成后 */
click_wheel_create();
...
/* main_tabs_sync() 内，切页时 */
click_wheel_set_tab(act);
```

`dir_list.c` / `playlist_page.c`：

```c
click_wheel_set_list(0, s_list, count);   // 列表构建后（歌曲页用 tab 1）
...
click_wheel_set_list(0, NULL, 0);         // 重建/销毁列表前解除绑定
```

`settings_menu.c`：`settings_menu_show()` → `click_wheel_set_overlay(true)`；
`settings_menu_hide()` → `click_wheel_set_overlay(false)`。

## 5. 调试与验证

- 每累计转动 `CW_LOG_STEP_DEG (30°)` 打一条日志，串口可见：

```
I (6483) click_wheel: rotate 35 deg -> scroll 21 px (scroll_y=1140)
I (11227) click_wheel: rotate 32 deg -> scroll 68 px (scroll_y=979)
```

`scroll_y` 变化方向可确认顺/逆时针与上下滚的对应关系。

- 创建日志：`click wheel created on top layer (56x56 px, margin 32/32, ...)`；
- 列表登记日志：`tab 0 list set: items=171 (wheel eligible)` /
  `(wheel hidden)`——可据此确认按钮是否该显示。

## 6. 踩坑记录

1. **不能放在 tab 页容器里（第一版失败原因）**：从按钮起手拖动会被 LVGL
   滚动链找到可横向滚动的 tabview → 判定为翻页 → 按钮收到
   `LV_EVENT_PRESS_LOST`，画圈失效（表现就是"拖动变成页面左右滑动"）。
   必须放 `lv_layer_top()` 并清除按钮的 `SCROLL_CHAIN_HOR/VER`。
2. **顶层对象的显隐必须外部驱动**：顶层不随页面切换变化，切页/开菜单时
   必须显式调用 `click_wheel_set_tab()` / `click_wheel_set_overlay()`；
   否则按钮会浮在设置菜单之上并拦截该区域的触摸。
3. **系统字体没有 PUA 区图标字形**：按钮图标用 LVGL 内置符号
   `LV_SYMBOL_REFRESH` + 内置 `lv_font_montserrat_20`（与 `virtual_list`
   行图标同一处理），换成 `UI_SYSTEM_FONT` 会显示方块。
4. **调试版字体缺字**：使用说明 Toast 文案「你点击的是 ClickWheel／按住该
   按钮并在屏幕上画圈可以快速翻页」里的中文字不在精简字库中，调试版会显示
   方块，发布版（完整字体）正常——调试阶段可无视。
5. **`M_PI` 自备**：部分 toolchain 严格 ANSI 模式下 `math.h` 不定义
   `M_PI`，文件内 `#ifndef M_PI` 兜底。
6. **阈值判断用条目数而非页数**：`item_count < 73` 隐藏；阈值常量
   `CW_MIN_ITEMS` 集中定义，改"10 页"只需改这一处。
7. **列表销毁前必须解除绑定**：`click_wheel_set_list(tab, NULL, 0)`，
   否则 `s_cur_list` 会指向已释放的对象。
8. **同层覆盖层靠创建顺序定 Z 轴（下拉控制中心）**：通知栏面板也挂在
   `lv_layer_top()`——`app_ui_create()` 的顺序是 `main_ui_create()`
   （内含 `click_wheel_create()`）→ `create_status_bar()` →
   `notification_panel_create()`，面板与按钮同层且**创建更晚**，因此面板
   绘制在按钮之上、展开时自然遮挡并接管按钮区域的触摸。
   ⚠ 曾用"面板开合时调 `click_wheel_set_overlay()` 隐藏按钮"处理该问题，
   属于治标（要跟踪拖拽/动画状态），已删除；**顺序敏感**，若把面板改回
   `lv_scr_act()`（屏幕层永远低于顶层）或提前创建，按钮会重新浮在
   控制中心之上。设置菜单仍在 `lv_scr_act()` 上，故它的
   `click_wheel_set_overlay()` 调用保留（见踩坑 2）。
