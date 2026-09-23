# 全局跑马灯（marquee）统一样式实现记录

> 对应 `FUTURE_PLAN.md` 第 36 点：「试着调整各个跑马灯的动画？（例如只重复三次，
> 每次之间要有 1 秒停顿）」。
> 实现日期：2026-09（debug 构建，COM12 实测）。

## 1. 需求

全项目的文本跑马灯（超长文本滚动）原本是 LVGL 默认行为：**无限循环、立即开始、
一轮接一轮没有停顿**。需求是把它统一成"有节奏、滚几轮就停"：

- 先停顿一下（让用户先读到标题开头），
- 滚动 N 轮（默认 **3 轮**），
- 每轮之间停顿（默认 **1000ms**），
- 滚完就停在原位，不再无限晃动。

并且希望像计划里说的那样：**一处配置、全局生效**（放一个 `global_style`）。

## 2. 研究结论：LVGL 8.4 里"动画模板"只支持一半

计划中引用的资料（智谱清言整理）说 8.2+ 可以用 `LV_STYLE_ANIM` 样式属性给 label
塞一个动画模板来控制速度、起始延迟、轮间停顿、重复次数。**读 8.4.0 源码后确认：
只有前一部分成立。**

源码位置：`managed_components/lvgl__lvgl/src/widgets/lv_label.c` →
`lv_label_refr_text()`。要点：

1. **模板只对 `LV_LABEL_LONG_SCROLL_CIRCULAR` 生效**。
   `LV_LABEL_LONG_SCROLL`（来回滚）那条分支根本不看模板，速度/延迟全按默认值来。
2. **模板里只有两个字段被读取**：

   ```c
   const lv_anim_t * anim_template = lv_obj_get_style_anim(obj, LV_PART_MAIN);
   ...
   if(anim_template) {
       a.act_time     = anim_template->act_time;      /* 起始延迟（set_delay 的负值） */
       a.repeat_delay = anim_template->repeat_delay;  /* 轮间停顿 */
   }
   ```

   模板里的 `time`、`repeat_cnt` **一概被忽略**（速度仍由 `LV_STYLE_ANIM_SPEED`
   / DPI 决定）。
3. **`repeat_count` 被 lv_label 写死为无限**：

   ```c
   lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);   /* ← 在 CIRCULAR 分支开头 */
   ```

   所以官方示例 `lv_example_label_5` 里设的"只重复 5 次"在 8.4 上**完全无效**。
   这就是本模块必须存在的理由。

> 备选方案（未采用）：直接改 `managed_components` 里的 `lv_label.c`，让它在有模板时
> 尊重 `repeat_count`（两行改动，也是最接近上游修法的做法）。**不采用的原因**：
> `managed_components/` 是组件管理器拉取的第三方代码，重新拉取/换机器就会被覆盖，
> 属于"静默失效"的坑；本项目一贯不改 vendor 代码。若将来升级 LVGL 且上游修好了
> 这一点，本模块可以只保留"模板 + attach"，删掉"接管"部分即可。

## 3. 实现方案（不修改 LVGL 源码）

新增模块 `main/core/ui/global_style.{h,c}`（"全局风格/动画统一配置"，目前只装跑马灯），
它由三部分组成：

### ① 全局动画模板 → 负责"起始延迟 + 轮间停顿"

```c
static lv_anim_t s_anim_template;              /* static：样式属性只存指针 */
lv_anim_set_delay(&s_anim_template, 起始延迟);
lv_anim_set_repeat_delay(&s_anim_template, 轮间停顿);
...
lv_obj_set_style_anim(label, &s_anim_template, 0);   /* 挂在每个跑马灯 label 上 */
```

好处：这两个参数由 **LVGL 自己**在每次重建滚动动画时套用（文本变化、尺寸变化、
样式变化都会重建），不需要我们反复插手。

### ② 就地"接管"动画 → 负责"有限轮数"

```c
lv_anim_t *a = lv_anim_get(label, NULL);   /* exec_cb 传 NULL：取该对象名下的任意动画 */
a->repeat_cnt   = GLOBAL_STYLE_MARQUEE_ROUNDS;   /* LVGL 语义：repeat_cnt = 总轮数 */
a->repeat_delay = GLOBAL_STYLE_MARQUEE_PAUSE_MS;
a->user_data    = (void *)&s_takeover_mark;      /* 打标记，避免反复改 */
```

**关键设计：改 LVGL 刚建的那条动画，而不是另建一条自己的动画。** 理由：

- 同一个 label 上永远只有一条滚动动画，不会出现"两条动画抢写 `label->offset.x`"
  （`lv_anim_start` 只按 `var + exec_cb` 去重，exec_cb 不同就两条并存，后插入的每帧
  先执行 → 我们那条会赢，但 LVGL 那条依然在跑，行为不可预测）；
- 动画的 `exec_cb` 仍是 LVGL 自己的 `set_ofs_x_anim`，所以 **LVGL 自己的既有逻辑
  全部不受影响**，例如：切到 `LV_LABEL_LONG_CLIP` 时 `lv_label_set_long_mode()`
  按 exec_cb 删动画并复位偏移 → 跑马灯正常停下；
- 不需要访问 `lv_label_t` 的内部字段（不碰 `label->offset`）。
  "滚动结束后停在终点"这件事**不需要**额外处理：CIRCULAR 模式会把文本复制一份接在
  后面（`lv_label.c` 的 `label_draw_dsc.ofs_x += size.x + 空格宽×3`），
  终点画面与起点画面**逐像素一致**，所以停在哪一端视觉上等价。

`repeat_cnt` 语义（`lv_anim.c` 的 `anim_ready_handler` / `lv_anim_get_playtime`）：
**`repeat_cnt` = 总轮数**（含第一轮），`LV_ANIM_REPEAT_INFINITE`(0xFFFF) 才是无限。

### ③ 守护定时器兜底（200ms）

LVGL 在文本/尺寸/样式变化时重建动画（重建后 `repeat_cnt` 又变回无限），
所以有一个低频 `lv_timer` 复查所有登记过的 label：只要它处于 CIRCULAR 模式、
且动画没被接管过（`user_data` 标记不一致，`lv_anim_init` 会 memset 整个结构，
所以 LVGL 重建后标记自动失效），就补一次接管；已接管的立即返回，开销可忽略。

### 对外接口

```c
/* 登记 + 挂全局模板 + 按全局节奏（重新）开始滚动 */
void global_style_marquee_attach(lv_obj_t *label);

/* 改跑马灯文本：内容没变则一个字都不动，变了才 set_text + 重跑（推荐用它代替裸 set_text） */
void global_style_marquee_set_text(lv_obj_t *label, const char *text);
```

- `attach()` 幂等：可反复调用，每次都会让跑马灯从头重跑；
- 两者都只接管**处于 `LV_LABEL_LONG_SCROLL_CIRCULAR`** 的 label —— 所以"非当前页不滚动"
  这类开关继续由页面代码用 CLIP 控制，本模块不越权；
- label 销毁时自动从监管表摘除（内部监听 `LV_EVENT_DELETE`）；
- 必须在 LVGL 上下文调用（持 `lvgl_port_lock` 或位于 LVGL 定时器/事件回调内），
  与项目既有的 LVGL 线程安全约定一致。
- **改文本一律用 `global_style_marquee_set_text()`**：它先 `strcmp` 比较，内容相同
  直接返回（动画完全不受影响），内容不同才 `set_text` + 重跑。原因见第 7 节第 1 条。

## 4. 参数（改 `global_style.h` 顶部四个宏即可，不经过 Kconfig）

| 宏 | 默认 | 说明 |
|---|---|---|
| `GLOBAL_STYLE_MARQUEE_ROUNDS` | 3 | 总滚动轮数（含第一轮）；**0 = 无限循环**（退回 LVGL 原生行为，此时不接管） |
| `GLOBAL_STYLE_MARQUEE_PAUSE_MS` | 1000 | 每轮之间的停顿 |
| `GLOBAL_STYLE_MARQUEE_START_DELAY_MS` | 1000 | 首次滚动前的停顿（0 = 立即滚） |
| `GLOBAL_STYLE_MARQUEE_SPEED` | 0 | 滚动速度 px/s；0 = 沿用 LVGL 默认（`DPI/3`，本项目 `LV_DPI_DEF=160` → 约 53px/s） |
| `GLOBAL_STYLE_MARQUEE_POLL_MS` | 200 | 守护定时器周期 |

> 用户明确要求跑马灯参数**不进 Kconfig**（改宏即可，避免为调一个动画节奏而走
> 删 sdkconfig + reconfigure 的流程）。

## 5. 接入点

| 位置 | 改动 |
|---|---|
| `core/ui/virtual_list.c` | 行文本 label 创建时 `attach()` **登记**（登记一次即可）；`vlist_apply_marquee()` 切回 CIRCULAR 时 `attach()`。⚠ `vlist_bind_row()` **不碰跑马灯**（见第 7 节第 1 条） |
| `ui/dir_list.c`、`ui/playlist_page.c` | 绑定回调里改用 `global_style_marquee_set_text(cell->text, ...)` 设置行文本 |
| `ui/now_playing.c` | 标题/副标题的**所有**改文本处用 `global_style_marquee_set_text()`（歌曲变化、封面补写标题）；创建与 `now_playing_set_marquee(true)` 时 `attach()` |

`ui/list_common.c` 的标题栏文字是 `LV_LABEL_LONG_CLIP`（裁剪），本来就不滚动，
不在范围内。

## 6. 开销

- 静态内存：监管表 48 × 4B = 192B；模板 `lv_anim_t` 一个；守护定时器约 40B 堆；
- 每个 label 多一个局部样式属性（`LV_STYLE_ANIM`，label 本来就有局部样式，不额外
  分配样式对象）；
- CPU：守护定时器每 200ms 遍历几十个 label，每次只做"读长文本模式 + 取动画"，
  已接管的动画立刻返回；**没有动画在跑时（滚完停住）零开销**；
- 跑马灯本身的 CPU 开销与改造前一致（只是轮数有限，滚完就停下来，反而更省）。

## 7. 已知边界与坑

1. **改跑马灯 label 的文本必须用 `global_style_marquee_set_text()`，不要裸调
   `lv_label_set_text()`。**
   ⚠ 这是本模块**唯一一个真正会咬人的坑**（已实际踩到，见第 8 节第 2 条）：
   `lv_label_set_text()` 即使内容**完全相同**也会走 `lv_label_refr_text()`，
   而 CIRCULAR 分支在有模板时会把 `a.act_time` 强制成模板的起始延迟、
   `early_apply` 又把偏移拉回 0 —— 于是"文本没变，跑马灯却回到起点重新停顿 1 秒"。
   而列表页在**每次切歌/切目录时都会重绑全部可见行**（`virtual_list_refresh()`，
   只为刷新"正在播放"的黄色高亮，文本其实一个字都没变），所以裸调 set_text 的
   表现就是**切歌瞬间整屏跑马灯一起重置**。
   同理：
   - 因此 `vlist_bind_row()` 里**不能**无条件 `attach()`（那等于强制重跑）；
   - 正在播放页也有两处会写同一份标题（`np_refresh_cb` 与封面加载完成后的补写），
     用 `set_text()` 包装后第二次写入被自动跳过（此前会让跑马灯白重启一次）。
   修复方式 = 内容比较（`strcmp`）后再决定是否 set_text + 重跑，
   **真正的换歌依旧是"停顿 1 秒 → 滚 3 轮"，1 秒初始延迟完全保留**。
2. **不要在一个跑马灯 label 上再挂别的 `lv_anim`**：接管用的是"取该对象名下的任意
   动画"（`lv_anim_get(label, NULL)`，因为 LVGL 的 `set_ofs_x_anim` 是 static 的，
   拿不到函数指针），若该 label 上还有别的动画会被误改。
   当前项目满足这一前提（所有其它动画都在状态栏/通知栏/封面/toast/触摸点上）。
3. 新写列表页/绑定回调时：文本请用 `global_style_marquee_set_text()`；若某处漏用而
   直接 `lv_label_set_text()` 设了**新**文本，也不算坏——LVGL 重建的动画同样带着
   模板的"起始延迟 + 轮间停顿"，只是轮数要等守护定时器（≤200ms，此时还在 1 秒
   延迟里，肉眼看不出）补上。
4. `GLOBAL_STYLE_MARQUEE_ROUNDS = 0` 时模块退化为"只挂模板"（无限循环 + 停顿），
   便于对比调试。
5. 速度宏只在 `> 0` 时生效，且必须在动画创建前设好（`attach()` 内部已保证顺序：
   先设 `ANIM_SPEED`，再重设长文本模式触发重建）。

## 8. 验证记录

1. 编译：debug 构建（`.\idfc.exe build`）通过，无新增警告；
2. 启动日志：`gstyle: marquee style: rounds=3, pause=1000ms, start delay=1000ms, speed=0px/s`
   （懒初始化，首次 `attach` 时打印一次），随后整机启动、播放、列表加载全部正常；
3. **设备实测（debug 版烧录到 COM12，用户确认）**：跑马灯如预期工作——
   停顿 → 滚动 3 轮（轮间停 1s）→ 停下；且**停下后文本内容更新（例如切歌）会正确
   从"停顿 → 3 轮"重新开始**。
4. **用户反馈的回归与修复（切歌时所有跑马灯一起重置）**：根因与修法见第 7 节第 1 条，
   修复后重新构建（`0x13c170`）并烧录 COM12 验证启动正常；
   预期表现：切歌时只有正在播放页的标题/副标题重新开始滚动，**列表行的跑马灯不受
   影响**（黄色高亮照常移动）。

### 调试技巧（本模块静默，需要排查时临时加日志）

接管是否真的生效，可以在 `gs_marquee_takeover()` 里临时插一行：

```c
ESP_LOGI(TAG, "TAKEOVER [%s] start=%d end=%d time=%u act=%d cnt=%u pause=%u",
         lv_label_get_text(label), (int)a->start_value, (int)a->end_value,
         (unsigned)a->time, (int)a->act_time, (unsigned)a->repeat_cnt,
         (unsigned)a->repeat_delay);
```

正常应看到 `cnt=3`（总轮数）、`act=-1000`（起始延迟来自模板）、
`end` 为负的滚动距离；若 `cnt=0xFFFF` 说明没接管成功（`lv_anim_get()` 没有拿到
LVGL 那条滚动动画）。
