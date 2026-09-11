# 虚拟 ClickWheel 大列表速滑组件（click_wheel）

> 对应 `FUTURE_PLAN.md` 第 24 点，已完成（见 `DONE_PLANS.md`）。
> 源码：`main/core/ui/click_wheel.{c,h}`；接入方：`main/ui/dir_list.c`、
> `main/ui/playlist_page.c`。

## 1. 需求与交互

- 目录页 / 歌曲页右下角悬浮一个**黄色圆形按钮**；
- **按住**后在屏幕上画圈 → 快速翻页：
  - 顺时针（角度增大）**向下**滚，逆时针**向上**滚
    （可配置为**反转**：顺时针向上滚，见下方「滑动方向」）；
  - **滚完整个列表所需的圈数由条目数决定**（2026-09 用户数学建模，取代原先
    固定的"5 圈到底 / `CW_TURN_RATIO`"）：

    ```
    y = log_1.618(x) - 7        // x = 列表条目数，y = 滚到底所需圈数
    y 钳制在 [1.0, 20.0]
    ```

    标定值：73 条 → 1.92 圈、100 条 → 2.57 圈、200 条 → 4.01 圈、
    600 条 → 6.29 圈、1000 条 → 7.36 圈、5000 条 → 10.70 圈、
    10000 条 → 12.14 圈。
    钳制在现有列表规模下不生效（y=1 需 x ≤ 47 条，低于 `CW_MIN_ITEMS` 显示
    阈值；y=20 需 x ≈ 44 万条），保留只为公式完整性。
    实现见 `click_wheel.c` 的 `cw_turns_for_count()` 与文件头常量注释；
    **速度档位**系数直接乘在上述圈数上（0.25 一档均匀变化）：
    普通 ×1.00（不计算）/ 快速 ×0.75 / 极速 ×0.50（见 `CW_SPEED_FACTOR`）；
  - **行对齐步进**（2026-09，用户要求）：内部连续累加像素量，但只把**行高的
    整数倍**交给列表 → 行槽屏幕位置不动，转动呈现"内容一格一格换"的阶梯式翻行；
    步进相对**列表当前位置**（不做按第一项对齐的吸附），滚到底按**最后一项**
    对齐（见 2.3）；
  - 画圈期间屏幕中央出现**12 个灰色小圆点围成一圈**（半径 130px、直径 20px，
    从正上方起均分），给手指一个旋转参照；松手立即消失；
- **直接点击**（按住期间没转动）→ 弹出使用说明 Toast；
- 按钮**只在列表超过 10 页（72 条）时显示**（`CW_MIN_ITEMS = 73`）；
- 全部选项集中在**设置菜单 →「虚拟 ClickWheel」子页**（NVS 持久化，
  三项均**点击立即生效**、无需重启）：
  - **开启 / 关闭**（配置 `click_wheel`，**默认开**）；
  - **滑动方向：正常 / 反转**（配置 `click_wheel_rev`，默认正常）；
  - **速度：普通 / 快速 / 极速**（配置 `click_wheel_spd`，默认普通；点击**循环**
    切换，系数 0.25 一档均匀变化：×1.00 / ×0.75 / ×0.50）。

## 2. 关键设计

### 2.1 按钮挂在 `lv_scr_act()`（屏幕层，tabview 的兄弟），必须断开滚动链

**第一版把按钮放在 tab 页容器里，实测无法画圈**：从按钮起手拖动时，LVGL 的
滚动链（`SCROLL_CHAIN_HOR/VER`，`lv_indev_scroll.c: find_scroll_obj`）会从
按下的对象一路向上找到**可横向滚动的 tabview**，把这次按压判定为"翻页
滚动"，并给按钮发 `LV_EVENT_PRESS_LOST` → 画圈状态被清掉，表现为
"拖动变成了页面左右滑动"。

修正（两个要点）：

1. **父对象改为 `lv_scr_act()`**（当前屏幕，即 tabview 的**兄弟**，比 tabview
   晚创建 → 位于整个主界面之上）：
   - 按钮不在 tabview 的祖先链上，滚动链找不到它，事件不会外泄；
   - 屏幕层对象不随页面横向滑动，按钮位置稳定；
   - **同层内靠创建顺序定 Z 轴**：下拉控制中心面板同样建在屏幕层且创建更晚
     → 展开时自然遮挡按钮并接管其区域触摸（见踩坑 8）。
   - ⚠ **不要改挂 `lv_layer_top()`**：那样按钮会浮在下拉控制中心之上，而
     "把面板也挂顶层"的修法会破坏 LVGL 的覆盖判定
     （`lv_refr_get_top_obj()` 只在当前屏幕的子对象树里查找覆盖绘制块的
     不透明对象），使被面板遮住的页面内容每帧重绘 → **下拉控制中心拉到
     一半后严重掉帧**（详见 `DONE_PLANS.md` 第 38 点）。
2. **按钮自身清除 `SCROLL_CHAIN_HOR` / `SCROLL_CHAIN_VER`**（图标 label 同样
   清除）：即使滚动链走到按钮，`SCROLLABLE == false` + 链断开会让
   `find_scroll_obj` 立即 break，不再向父级找滚动对象。

**显隐必须由外部驱动**（本模块不感知当前页面）：
`click_wheel_set_tab()`（主界面切页时调用）+ `click_wheel_set_list()`
（各页列表构建时登记滚动目标）+ `click_wheel_set_overlay()`
（设置菜单开合时调用；下拉控制中心无需此调用——它同在屏幕层且创建更晚，
天然在按钮之上）。

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
  比例缩小（绕按钮一圈 ≈ 屏幕中心 36°），转一圈的滚动量由圈数模型
  （条目数 → 圈数，见 2.3）统一标定；
- 每帧角度差需归一化到 (-180, 180]：

```c
while (delta >  180.0f) delta -= 360.0f;   // 处理 179° → -179° 的跨界
while (delta < -180.0f) delta += 360.0f;
```

未归一化时，手指经过 0°/360° 分界线会产生 ±360° 的跳变，列表会瞬间飞到底。

### 2.3 滚动量换算、方向与**行对齐步进**

```
可滚动行程 range = scroll_top + scroll_bottom      // = 内容总高度 - 视口高度；
                                                   //   与当前滚动位置无关（常量）
圈数 turns = clamp(log_1.618(条目数) - 7, 1, 20)    // 圈数模型，见 1 节
每度滚动量 px_per_deg = range / (turns * 360)       // 转满 turns 圈正好从顶滚到底
连续滚动量 px = -角度差 × px_per_deg                // 内部量，仅累加，不直接交给列表
```

⚠ 距离基准取的是**可滚动行程**（内容总高度 − 视口高度），而不是内容总高度：
"滚完整个列表"的行程就是这一段（`scroll_top + scroll_bottom` 之和恒等于它，
与当前停在哪里无关），所以转满 y 圈刚好停到最后一项贴底。
（`cw_scroll_by_deg` 另需 `range <= 0` 时直接返回：内容不足一屏无需滚动。）

⚠ 取负号的推导（别搞反）：屏幕坐标系里**顺时针 = 角度增大**；而
`lv_obj_scroll_by(dy<0)` 表示**内容上移 = 视口向下**。需求是"顺时针向下滚"，
所以 顺时针（正角度差）→ 需要 dy 为负 → 公式带负号。
（`_lv_obj_scroll_by_raw` 是 `scroll.y += dy` 且子对象整体位移 `dy`，
dy 为正是回滚到更靠前的条目。）

**行对齐步进（2026-09 用户要求，核心手感）**：

```
s_scroll_acc += px;                       // 1. 浮点累加器累加连续像素量（含不足一行的余量）
rows = (long)(s_scroll_acc / item_h);     // 2. 取出已满的整行数（朝 0 截断）
if (rows == 0) return;                    //    不足一行：余量留到下次，不再丢弃小角度转动
s_scroll_acc -= rows * item_h;            // 3. 扣掉已交给列表的量，余数继续累积
lv_obj_scroll_by_bounded(list, 0, rows * item_h, LV_ANIM_OFF);   // 4. 只输出 item_h 的整数倍
```

- 行高来自 `virtual_list_get_item_height()`（`virtual_list` 为 ClickWheel 新加的
  只读访问器，仍不触碰其内部结构）；取不到（非 virtual_list）时退化为按像素滚；
- **步进是相对"列表当前位置"的**：每帧只 `scroll_by` 行高的整数倍，**不做任何
  "对齐到整行网格"的吸附**。列表停在什么位置（哪怕手指滑动留下的半行小数相位），
  就从那个样子一格格跳 —— 行槽在屏幕上的位置全程不动，只有里面的条目在换
  （阶梯式翻行，类似实体 ClickWheel 的"哒哒"感），而不是平滑滑动；
  - ⚠ 第一版曾在"首次转动"时把列表吸附到 `round(scroll_y / item_h) * item_h`
    （即按**第一项**建立的整行网格），用户明确否掉：那会让列表被强行拽到
    与第一项对齐的网格上，而不是保持它当前的样子。**不要把这个吸附加回来**；
- 边界：**不做预钳制**，直接由 `lv_obj_scroll_by_bounded()` 自己 clamp——
  滚到最底下时多余的滚动量被列表丢掉，列表正好停在"**最后一项贴底**"
  （顶部同理停在 0，即第一项贴顶）。累加器余量天然有界（上限一行），
  反向转动最多先"还"掉一行就会立刻响应，不会攒账；
- 代价：不再平滑跟手，快速转动时一帧可跨多行（上限 = 1/4 圈，即"每圈行数 / 4"）；
- **附带收益（用户实测发现，见 2.3.2）**：位移精度从"帧率相关"变成"帧率无关"，
  且原本快速滚动时的卡顿感在体感上消失——按格滚动的节奏本来就不均匀，
  掉帧只表现为"这一跳晚一点/这一下多跳几格"，不再被读成画面一顿；
- 单帧钳制 = **1/4 圈对应的滚动量**（`CW_MAX_STEP_DEG = 90°`，随行程与圈数一起
  标定）：触摸坐标偶发跳变（如 180°）时不会乱飞。正常转动每帧约 10~40°，
  远低于 90°，不会削掉有效滚动量。
  ⚠ 这里刻意**不用固定像素值**（原为 `CW_MAX_STEP_PX = 800`）：同一个 800px
  对 73 条（每圈约 34 行）是半个圈，对 10000 条却只是几十分之一圈——长列表
  快速画圈时单帧滚动量被削顶，表现成"要转十几圈才到底"，与圈数模型自相矛盾
  （历史注释里已警告过这一点）；改成按角度钳制后，任何列表规模的保护强度一致；
- 用 `lv_obj_scroll_by_bounded()` 而非 `lv_obj_scroll_by()`：自带边界钳制，
  快速转动到顶/底不会出现过冲回弹；
- `LV_ANIM_OFF`：转动过程要求跟手，动画会累积延迟。

### 2.3.1 为什么必须按整行输出（虚拟列表的配合）

`virtual_list` 的行槽锚定在内容坐标 `index × item_height`，滚动时 LVGL 整体平移
内容，仍在视口内的行**一个都不重绑**，只有滚出/滚入视口的行被回收重绑
（见 `VIRTUAL_LIST_USAGE.md`）。输出恒为行高整数倍时：

- 行槽的屏幕坐标 `index × item_h - scroll_y` 的**小数部分全程不变**（无论列表
  初始停在整行还是半行），所以一格格跳时视觉上"行槽纹丝不动、只有内容在换"；
- 每次跨一行只重绑"出去 1 行 + 进来 1 行"，重建成本恒定为 O(1)，
  与跳跃幅度无关（一帧跨 14 行也只重绑池内的 `row_count` 行）；
- 反之若输出任意像素（含小数），滚动会变成平滑滑动，且每帧都要重绑新进视口的行，
  那种"点击感"和 O(1) 重建都会丢。

### 2.3.2 附带收益：掉帧不再影响位移，卡顿感在体感上消失（意外发现）

这不是需求的一部分，是改完之后**用户实测发现的意外好处**（2026-09），值得记下来，
因为它把这个组件从"手感取舍"变成了"纯赚"：

- **位移精度变成帧率无关**：平滑滚动靠每帧累加位移，丢帧 = 实际少滚一段，
  误差会累积（"y 圈到底"的承诺会被掉帧破坏）；按格滚动里，"格数"由角度差算出、
  与帧数无关，掉帧只让某一格晚来一点点，**总位移不会少**；
- **卡顿感被感知层面吃掉**：格进式把"可见位移"与"帧时序"解耦了。帧变慢时，
  平滑滚动表现为一次明显突进（眼睛读作卡顿），而按格滚动只表现为"这一下多跳了
  几格"——它本来就是离散、不均匀的节奏，所以抖动不再被识别为抖动；
- 于是抖动的表现形式从"画面一顿"降级为"节奏稍慢"，是**更宽容的失败模式**：
  失败模式下限比平滑滚动高得多；
- ⚠ 注意这只改变**观感**，不减少绘制开销：每帧仍然是一次列表区整块重绘，
  与一帧跨几行无关。**底层帧率/CPU 该优化还是要优化**（实测画圈期间 Core0
  约 92~97%、空闲 3~6%），别拿这个特性当性能问题的遮羞布。


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
- 与按钮同层（`lv_scr_act()` 屏幕层），常驻隐藏、按下即显示、松手 / PRESS_LOST
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
void      click_wheel_set_reverse(bool reverse);                 /* NVS 方向，默认正常 */
void      click_wheel_set_speed(uint8_t speed);                  /* NVS 速度档位，默认普通 */
```

- `create`：在 `main_ui_create()` 中调用（四个页面创建之后、首次
  `main_ui_switch_tab` 之前）；
- `set_list`：`dir_list.c`（tab 0）与 `playlist_page.c`（tab 1）在列表
  构建完成后调用，销毁列表前先传 `NULL`；
- `set_tab`：`main_ui.c` 的 `main_tabs_sync()`；
- `set_overlay`：`settings_menu.c` 的 `settings_menu_show/hide()`；
- `set_enabled`：`main_ui_create()` 创建后按 `config_store_get_click_wheel()`
  初始化；设置菜单 →「虚拟 ClickWheel」→「开启」切换后立即调用（关闭时
  隐藏按钮并终止进行中的画圈、收起指示圆点）；
- `set_reverse` / `set_speed`：同样由 `main_ui_create()` 按 NVS 初始化，
  设置菜单子页对应行切换后立即调用（**无需重启**）；`set_speed` 取
  `CLICK_WHEEL_SPEED_NORMAL/FAST/TURBO`（越界钳到普通）；
- 全部接口需持 LVGL 锁。

### 3.1 配置项（NVS，设置菜单 →「虚拟 ClickWheel 选项」子页）

| 配置键 | 内存值 | 取值 | 默认 | 作用 |
|---|---|---|---|---|
| `clkwheel` | `click_wheel` | 开/关 | **开** | 是否启用速滑按钮（关闭即隐藏+终止画圈） |
| `clkwheel_rev` | `click_wheel_reverse` | 正常/反转 | 正常 | 正常 = 顺时针向下滚；反转 = 顺时针向上滚 |
| `clkwheel_spd` | `click_wheel_speed` | 普通/快速/极速 | 普通 | 圈数系数 **×1.00 / ×0.75 / ×0.50**（0.25 一档均匀），点击循环切换 |

- 显隐判定：`启用 && !覆盖层 && 当前页是列表页 && 条目数 ≥ 73`（`cw_sync_visible`）；
- 速度档位只作用在"几圈到底"上：`cw_effective_turns() = cw_turns_for_count() ×
  CW_SPEED_FACTOR[speed]`，例：611 首模型 6.33 圈 → 普通 6.33 / 快速 4.75 /
  极速 3.17 圈（实测单帧跨行数随档位变化）；行对齐步进、单帧 1/4 圈钳制、
  方向换算都不受影响；
- 档位取值在两处定义（`config_store.h` 的 `CONFIG_STORE_CW_SPEED_*` 与
  `click_wheel.h` 的 `CLICK_WHEEL_SPEED_*`，core 层互不依赖），
  `settings_menu.c` 用 `_Static_assert` 校验两者一致，防数值漂移；
- 方向在每帧换算处取符号（`dir_sign = reverse ? +1 : -1`），切页/换列表无需重算；
- `core/ui/click_wheel.c` **不依赖 config_store**（保持 core/ui 与应用配置解耦）：
  三项配置都由 UI 层传入，切换由设置菜单子页回调驱动。

⚠ 设置菜单根页面仍是 6 行（ClickWheel 那行由"点击即开关"改为**子页入口**，
行数不变），实测根页面内容高度 357px、6 行需 426px → **根页面保留滚动条**
（用户确认可接受，不再压缩行距）。子页内 3 个选项行 + **1 张说明卡**。

⚠ 说明卡只讲"这个功能是什么"（2026-09-11 用户要求）：**不写方向、不写速度
的细节**——那属于手感差异，交给用户自己试；文字里也刻意不提转向
（原文末句"（顺时针向下滚）"已删），避免暗示只有一种转法。

⚠ 入口行的写法（2026-09-11 用户要求）：**标题为「虚拟 ClickWheel 选项」、
右侧不显示状态**——`menu_row_create(..., "虚拟 ClickWheel 选项", NULL)` 传
`NULL` 表示不建状态 label，开启状态只在子页第一行「开启」里看（根页面那行
因此比"睡眠定时器/屏幕自动息屏"两行短，视觉上是纯入口）。

## 4. 接入方式（三处）

`main_ui.c`：

```c
/* 四个页面创建完成后 */
click_wheel_create();
click_wheel_set_enabled(config_store_get_click_wheel());          /* 开关（NVS） */
click_wheel_set_reverse(config_store_get_click_wheel_reverse());  /* 方向（NVS） */
click_wheel_set_speed(config_store_get_click_wheel_speed());      /* 速度档位（NVS） */
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

`settings_menu.c`：

- `settings_menu_show()` → `click_wheel_set_overlay(true)`；
  `settings_menu_hide()` → `click_wheel_set_overlay(false)`；
- 「虚拟 ClickWheel」子页三行回调 → `config_store_set_click_wheel*()` 之后
  分别调 `click_wheel_set_enabled()` / `click_wheel_set_reverse()` /
  `click_wheel_set_speed()`（写 NVS + 立即生效，同一行完成）。

## 5. 调试与验证

- 每累计转动 `CW_LOG_STEP_DEG (30°)` 打一条日志（**行对齐步进后格式含跨行数**）：

```
I (6483) click_wheel: rotate 35 deg -> 10 row(s) (560 px, scroll_y=1140)
I (11227) click_wheel: rotate 32 deg -> -9 row(s) (-504 px, scroll_y=636)
```

`scroll_y` 变化方向可确认顺/逆时针与上下滚的对应关系；`px` 恒为行高的整数倍
（dir 页 52、歌曲页 56），`scroll_y` 也恒为行高整数倍（吸附后）。

- 创建日志：`click wheel created on screen layer (56x56 px, margin 32/32, ...)`；
- 列表登记日志：`tab 0 list set: items=171 (3.69 turns to end, wheel eligible)` /
  `tab 1 list set: items=611 (6.33 turns to end, wheel eligible)` /
  `(..., wheel hidden)`——既能确认按钮是否该显示，也能直接读出该列表按
  圈数模型 × 当前速度档位系数算出的"几圈到底"（`cw_effective_turns()`）；
- 设置项切换日志（子页三行各一条，用于确认"立即生效"）：
  `click wheel enabled/disabled`、`click wheel direction: normal/reverse`、
  `click wheel speed: normal|fast|turbo (x1.00|0.75|0.50 -> 6.33|4.75|3.17 turns
  for 611 items)`（当前页面没有绑定列表时省略圈数，打成
  `... speed: fast (x0.75, no list bound)`）；`settings_menu` 侧另有一条
  `virtual click wheel speed -> 快速 (level 1)` 之类的记录；
- ⚠ 日志里的 `rotate %.0f deg` 是**每累计 30° 打一条**，而 `px` 只是**该条日志
  对应那一帧**的输出，两者不可直接相除。要核对手感请按整段行程比：
  611 首、速度=普通时 `range = 611×56 − 392 = 33824px`、6.33 圈 → 14.85px/度；
  实测取 `scroll_y 4670 → 0`（4670px）累计约 323° → 14.5px/度（差约 2%，
  来自日志分组量化与到顶 clamp 丢弃的余量），与模型吻合。档位越灵敏该值越大
  （快速 ≈ 19.8px/度、极速 ≈ 29.7px/度），实测单帧跨行数随档位明显增加。

## 6. 踩坑记录

1. **不能放在 tab 页容器里（第一版失败原因）**：从按钮起手拖动会被 LVGL
   滚动链找到可横向滚动的 tabview → 判定为翻页 → 按钮收到
   `LV_EVENT_PRESS_LOST`，画圈失效（表现就是"拖动变成页面左右滑动"）。
   必须放**屏幕层 `lv_scr_act()`**（tabview 的兄弟，见 2.1）并清除按钮的
   `SCROLL_CHAIN_HOR/VER`。⚠ 第一版曾放 `lv_layer_top()`，后来因 Z 序与
   重绘掉帧问题（第 38 点）改回屏幕层——此处"必须放顶层"是历史残留。
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
8. **同层覆盖层靠创建顺序定 Z 轴（下拉控制中心）**：通知栏面板与按钮
   **同在屏幕层**（`lv_scr_act()`）——`app_ui_create()` 的顺序是
   `main_ui_create()`（内含 `click_wheel_create()`）→ `create_status_bar()`
   → `notification_panel_create()` → `settings_menu_create()`，面板与按钮
   同层且**创建更晚**，因此面板绘制在按钮之上、展开时自然遮挡并接管按钮
   区域的触摸。
   ⚠ **顺序敏感**：把面板提前创建、或把按钮/面板任一方挪到别的层
   （尤其 `lv_layer_top()`），都会破坏这个 Z 序；其中"把面板挪到顶层"的写法
   还会让被面板遮住的页面内容每帧重绘，造成**下拉控制中心严重掉帧**
   （见 `DONE_PLANS.md` 第 38 点）。
   曾用"面板开合时调 `click_wheel_set_overlay()` 隐藏按钮"处理该问题，
   属于治标（要跟踪拖拽/动画状态），已删除。设置菜单也在屏幕层、创建更晚
   （`s_menu_bg` 会盖住按钮），但按钮仍会拦截该区域的触摸（命中测试会穿透
   不可点击的覆盖层），故它的 `click_wheel_set_overlay()` 调用保留（见踩坑 2）。
