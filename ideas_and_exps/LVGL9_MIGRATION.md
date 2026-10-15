# LVGL 8.4 → 9.4 迁移记录

> 状态：**已完成，debug 构建实机验证通过**（2026-10-15）
> 目标：把 `lvgl/lvgl` 从 8.4.0 升到 9.4.0，先跑通 debug 构建
> 相关文件：`main/idf_component.yml`、`sdkconfig.defaults`、`main/core/ui/lvgl_port.{c,h}`、
> `main/core/ui/lvgl_mem.c`（新增）、`main/core/ui/global_style.{c,h}`、
> `main/core/image/*`、`main/CMakeLists.txt`、`main/ui/*`

---

## 0. 结果与验证

### 0.1 结论

- 依赖：`lvgl/lvgl` **8.4.0 → 9.4.0**（ESP Registry，`dependencies.lock` 已更新；无额外依赖）
- debug 构建（`cs43131.debug`）：**通过，0 error / 0 warning**（LVGL 组件自身那几条
  `-Wattributes` 属上游代码，未处理）
- 固件体积：`0x1553d0`（约 1.34 MB）
- 实机验证（烧录 COM12，串口监视 + 用户手动操作）：**全部通过**
  - 页面切换（四页滑动）、大列表速滑（ClickWheel）、EQ 切换、设置页九宫格、
    设置菜单与子页、**重新扫描**（含关缓存深链）、息屏/亮屏、播放/切歌

### 0.2 实测证据（串口日志）

| 观测项 | 迁移前（LVGL 8.4） | 迁移后（LVGL 9.4） |
|---|---|---|
| `app_ui_create()` 之后的内部 RAM 空闲 | ~4~5 KB（历史记录） | **42.3 KB** |
| 同上，DMA 空闲 | ~3 KB（历史记录，堆毒化时代） | **34.6 KB** |
| `lvgl_mem.c` 落地（PSRAM 对象堆）前的最差值 | —— | **内部剩 23 B、DMA 剩 19 B、最大连续块 0** |
| LVGL 任务栈高水位 | ~3.3 KB / 6 KB | **5532 B / 12 KB** |
| 崩溃/断言（两轮启动 + 约 2.7 分钟操作） | —— | **0**（只有 1 次 SD 瞬时失败自恢复，见 §11） |

---

## 1. 迁移面评估（先量后改）

| 项目 | 数量 |
|---|---|
| 用到 LVGL 的源文件 | 26 个（`main/` 下，不含字体资源） |
| 依赖 v8 API 的调用点 | 约 1000 处（`lv_obj_set_style_*` 359、`lv_font_*` 203、`lv_anim_*` 102 等） |
| **实际需要改的调用点** | **约 40 处** |

之所以只改 40 处：**LVGL 9 自带的 `src/lv_api_map_v8.h` 默认被 `lvgl.h` 包含**
（除非定义 `LV_DISABLE_API_MAPPING`），`lv_obj_clear_flag` / `lv_scr_act` / `lv_img_set_src` /
`lv_disp_get_hor_res` / `lv_obj_del` / `lv_timer_del` / `lv_anim_set_time` 等大批改名
都是宏别名，**不需要动**。真正要改的是：

1. 兼容层**没有**覆盖的改名（menu / tabview / button class / image class / image 描述符…）；
2. 签名真变了、宏挡不住的（display/indev 驱动、`lv_obj_set_style_size`、`lv_event_send`）；
3. 结构体变不透明（`lv_timer_t`、`lv_indev_t`、`lv_hit_test_info_t`）；
4. Kconfig 项改名/删除；
5. **能编过但语义变了**的坑（最危险，见 §7）。

---

## 2. Kconfig 配置项对照（`sdkconfig.defaults`）

| LVGL 8.4 | LVGL 9.4 | 说明 |
|---|---|---|
| `CONFIG_LV_COLOR_16_SWAP=y` | **已删除** | 改在 flush 回调里调 `lv_draw_sw_rgb565_swap()`（官方迁移指南明确指定），见 §4 |
| `CONFIG_LV_COLOR_SCREEN_TRANSP=y` | **已删除** | v9 由屏幕/图层的 `bg_opa` 表达，本项目不用透明屏 |
| `CONFIG_LV_MEM_CUSTOM=y` | `CONFIG_LV_USE_CLIB_MALLOC` / **`CONFIG_LV_USE_CUSTOM_MALLOC`** | 见 §10：本项目选 **CUSTOM**，对象堆整体挪到 PSRAM |
| `CONFIG_LV_MEMCPY_MEMSET_STD=y` | `CONFIG_LV_USE_CLIB_STRING=y` | 字符串函数仍用标准 C |
| `CONFIG_LV_DISP_DEF_REFR_PERIOD=33` | `CONFIG_LV_DEF_REFR_PERIOD=33` | 改名 |
| `CONFIG_LV_INDEV_DEF_READ_PERIOD=33` | **已删除** | v9 输入设备与显示刷新**共用** `LV_DEF_REFR_PERIOD`（`lv_indev.c` 里 `lv_timer_create(..., LV_DEF_REFR_PERIOD, ...)`） |
| `CONFIG_LV_USE_LARGE_COORD=y` | **已删除** | v9 的坐标天然是 `int32_t`（`lv_coord_t` 只剩兼容 typedef），600+ 长列表不再需要该开关 |
| `CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18=y` | 同名（v9 是 choice） | 不变 |
| `CONFIG_LV_FONT_FMT_TXT_LARGE=y` | 同名 | 不变 |
| `CONFIG_LV_DPI_DEF=160` / `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` / 字体 12~48 | 同名 | 不变 |
| `CONFIG_LV_USE_PERF_MONITOR=y` | 同名 **但新增前置依赖 `CONFIG_LV_USE_SYSMON=y`** | ⚠ v9 把性能监视器挂在 SYSMON 之下，且 **SYSMON 默认是 `n`**；不显式打开，`LV_USE_PERF_MONITOR` 会被 Kconfig **静默丢弃**（屏幕角落的 FPS 就没了，且不报错） |

---

## 3. 移植层重写（`core/ui/lvgl_port.c` / `.h`）

v9 把静态驱动结构体换成了运行时对象，必须整体重写这一段：

| v8 | v9 |
|---|---|
| `lv_disp_draw_buf_t disp_buf` + `lv_disp_drv_t disp_drv` | `lv_display_t *s_disp` + 两块裸 `uint8_t *` 缓冲 |
| `lv_disp_drv_init/register(&disp_drv)` | `lv_display_create(w,h)` + `lv_display_set_flush_cb/buffers/user_data` |
| `lv_indev_drv_t indev_drv` + `lv_indev_drv_register` | `lv_indev_create()` + `lv_indev_set_type/read_cb/user_data` |
| `flush_cb(lv_disp_drv_t*, area, lv_color_t*)` | `flush_cb(lv_display_t*, area, uint8_t*)`（像素数据是无类型字节流） |
| `disp_drv.rounder_cb` | `lv_display_add_event_cb(disp, cb, **LV_EVENT_INVALIDATE_AREA**, NULL)`，回调里 `lv_event_get_param(e)` 取 `lv_area_t*` |
| `lv_disp_draw_buf_init(&buf, b1, b2, 像素数)` | `lv_display_set_buffers(disp, b1, b2, **字节数**, LV_DISPLAY_RENDER_MODE_PARTIAL)` |

### 3.1 两个必须记住的点

**① 缓冲区大小单位变了：像素数 → 字节数。**
**② 每像素字节数必须显式取 2，不能再用 `sizeof(lv_color_t)`**：

> ⚠ **v9 的 `lv_color_t` 恒为 RGB888（3 字节结构体），与 `LV_COLOR_DEPTH` 无关**
> （官方迁移指南原文："In v9 `lv_color_t` is always RGB888 regardless of `LV_COLOR_DEPTH`"）。
> 照抄 v8 的 `EXAMPLE_LCD_H_RES * BUF_H * sizeof(lv_color_t)` 会按 3 字节/像素要内存，
> 双缓冲多要 50% 的 DMA。**实测现象**：启动即
> `assert failed: lvgl_port_init lvgl_port.c:286 (s_draw_buf2)`（第二块缓冲分配失败）。
> 正确写法：`LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)`（= 2）。

### 3.2 flush 完成回调的上下文传递

`display_init()` 早于 `lvgl_port_init()`，而 v9 的显示对象要等 `lv_display_create()`
才存在，所以原来的 `lvgl_port_get_disp_drv()`（返回静态结构体地址）失效。
新做法：交出**"保存显示对象指针的变量"的地址**：

```c
void *lvgl_port_get_flush_ctx(void) { return (void *)&s_disp; }   /* display.c 传给 esp_lcd */

bool lvgl_port_notify_flush_ready(..., void *user_ctx) {
    lv_display_t **disp = (lv_display_t **)user_ctx;
    if (disp && *disp) lv_display_flush_ready(*disp);   /* 未创建时判空 */
    return false;
}
```

---

## 4. 颜色字节序（最容易搞错的一项）

### 4.1 原理

v8 的 `CONFIG_LV_COLOR_16_SWAP=y` 是**让整条渲染管线直接产出"高字节在前"的 RGB565**。
v9 删除了该选项，官方方案是在 flush 回调里对整块缓冲做一次交换：

```c
#if LCD_BIT_PER_PIXEL == 16
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)lv_area_get_size(area));
#endif
```

**为什么不直接把显示格式设成 `LV_COLOR_FORMAT_RGB565_SWAPPED` 免掉这一遍？**
因为 ESP-IDF 的 LVGL Kconfig **没有** `LV_DRAW_SW_SUPPORT_RGB565_SWAPPED` 这一项，
`lv_conf_internal.h` 在 Kconfig 模式下把它求值为 **0**，于是 `lv_draw_sw_blend_to_rgb565_swapped.c`
整个不参与编译（`lv_draw_sw_blend_to_rgb565.c` 里 `case LV_COLOR_FORMAT_RGB565_SWAPPED`
也被同一个宏挡掉）。用不支持的格式作渲染目标是走不通的。
代价：每次刷屏多一遍 ~2 字节/像素的遍历（460×46 ≈ 2.1 万像素，远小于 33ms 预算）。

### 4.2 连带修改：所有"已经按高字节在前存放"的资源都要翻转

交换点在 flush，意味着**渲染缓冲里必须是本机（小端）字节序**，所以：

| 位置 | 改动 |
|---|---|
| `core/image/cover_jpeg.c` | `esp_jpeg` 的 `swap_color_bytes` **1 → 0**（改为输出本机字节序） |
| `core/image/default_cover_flash.c` | 423200 字节（460×460×2）的 RGB565 像素数据**逐对翻转**（脚本 `ideas_and_exps/swap_cover_bytes.ps1`，含逐 token 校验：交换后第 k 个 token == 交换前第 k^1 个） |
| `core/image/default_cover.c` | 2bit 索引色，调色板是 ARGB8888，**不受字节序影响**，无需翻转 |
| `core/hardware/display.h` 的 `LCD_BIT_PER_PIXEL==24` 分支 | 顺手改成按字节就地交换（原写法依赖 v8 的 `lv_color_t.ch` 字段；本项目固定 16bpp，该分支仅保留可能性、未实测） |

> 结论是**逻辑推导**出来的：v8 在 `LV_COLOR_16_SWAP=1` 下，图片混合是"同格式 `full` 值对拷"，
> 面板收到的就是素材原样的字节串；v9 改成本机字节序渲染 + flush 交换，要让面板收到同一串字节，
> 素材本身就必须是本机字节序。

### 4.3 字节序的交叉验证（不用看屏幕）

翻转对不对，可以拿项目里**两个不同格式的同一张默认封面**互相印证：

| 素材 | 左上角逐像素 |
|---|---|
| `default_cover.c`（2bit 索引色） | 数据首字节 `0xff` → 4 个像素都是**索引 3**；调色板第 3 项字节为 `0x18,0x1a,0x1a,0xff`（`lv_color32_t` = B,G,R,A）→ RGB(26,26,24) → 量化成 RGB565 = **0x18C3** |
| `default_cover_flash.c`（翻转后） | 前若干像素均为字节 `0xc3, 0x18` → 按本机小端读作 uint16 = **0x18C3** |

两者**逐位相同**，说明翻转方向正确（若没翻转，flash 版读出来是 `0xC318` = 浅灰 RGB(197,198,197)，
与索引色版的近黑完全对不上）。实机确认默认封面与 JPEG 封面颜色正常。

> JPEG 那条链路同理：v8 用 `swap_color_bytes=1` 生成"高字节在前"以匹配 `LV_COLOR_16_SWAP`，
> v9 改为 `0`（本机字节序）+ flush 交换，两条链路的字节序约定从此统一。

---

## 5. 图片描述符（`lv_img_dsc_t` → `lv_image_dsc_t`）

v8 的 `LV_IMG_CF_*` 全部删除，`header` 结构也变了：

| v8 | v9 |
|---|---|
| `LV_IMG_CF_TRUE_COLOR` | `LV_COLOR_FORMAT_RGB565` |
| `LV_IMG_CF_INDEXED_2BIT` | `LV_COLOR_FORMAT_I2` |
| `.header.always_zero` / `.header.reserved` | **已删除**（换成了 `magic` / `flags` / `stride` / `reserved_2`） |
| — | 必须补 `.header.magic = LV_IMAGE_HEADER_MAGIC`、`.header.flags = 0`、`.header.stride` |

`stride` 的取值（也是本项目实测确认的）：
- RGB565：`460 × 2 = 920`
- I2（2bit 索引）：`ceil(460 × 2 / 8) = 115`；数据区开头是 4 个 `lv_color32_t` 调色板项
  （v9 的索引格式同样"调色板在数据最前面"，见 `lv_draw_buf.h` 的 `LV_DRAW_BUF_SIZE` 宏里那项
  `LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t)`），故 `data_size = 16 + 460×115 = 52916`（与 v8 相同）

`cover_jpeg.c` 是**运行时**构造描述符，除了上面几项，还要注意解码缓冲在 PSRAM 时
v9 会走"非 DMA 直拷"路径 —— 本项目缓冲在 PSRAM、只用 CPU 混合，没问题。

> 顺手补充：`lv_image_decoder` 对 `LV_IMAGE_SRC_VARIABLE` 会**原样 memcpy 描述符的 header**
> （不做 magic 校验，只有 `LV_IMAGE_SRC_FILE` 才有 v8 兼容回退），所以 magic 严格来说可选；
> 但还是按官方资产格式补上，避免以后换解码器踩坑。

---

## 6. 字体：**无需重新生成**

生成脚本已经带了版本分支，实测直接可用：

- 描述符里有 `#if LVGL_VERSION_MAJOR == 8 .cache = &cache #endif` —— v9 的
  `lv_font_fmt_txt_dsc_t` 没有 `cache` 字段（多了 `stride`），恰好落在 `#if` 之外；
- `get_glyph_bitmap` 在 v9 换了签名（`(lv_font_glyph_dsc_t*, lv_draw_buf_t*)`，不再接
  `lv_font_t*`/`uint32_t`），但生成代码只是**取函数地址**赋给函数指针，签名由头文件决定，故自动适配；
- 新字段 `.stride` 未赋值即为 0，v9 在 `lv_font_fmt_txt.c` 里把 0 解释为"不做字节对齐、
  位连续打包"，与 v8 的 `line_rem = box_w` 语义**完全一致**，所以字形位图解析方式没变。

---

## 7. 能编过但会跑飞的坑（本次唯一一处）

```c
lv_event_send(back, LV_EVENT_CLICKED, NULL);        /* v8 写法 */
```

v9 的 `lv_event_send()` **已经是底层接口**：`lv_result_t lv_event_send(lv_event_list_t *list, lv_event_t *e, bool preprocess)`
（见 `src/misc/lv_event.h`）。v8 那个"给对象发事件"的重载在 v9 叫 **`lv_obj_send_event()`**。
两者**参数个数相同**，所以只是警告（指针↔整数转换），能编过，但运行时会拿字符串指针当事件链表用——必炸。

已改为 `lv_obj_send_event(back, LV_EVENT_CLICKED, NULL)`（`ui/settings_menu.c`）。

> 教训：迁移时不能只看"编译过不过"，对**同名函数**要逐个确认签名。官方迁移指南开头就警告过
> 这类"不报编译错误但 LVGL 不工作"的改动。

---

## 8. 结构体变不透明：改走 getter

| 代码 | v8 | v9 |
|---|---|---|
| `timer->user_data` | 公开字段 | **`lv_timer_get_user_data(timer)`**（`ui/boot_flow.c`） |
| `indev->proc.state` | 公开字段 | **`lv_indev_get_state(indev)`**（`ui/now_playing.c`） |
| `hit_info->res = false`（点击穿透） | `lv_hit_test_info_t` 公开 | 见 §9 |

---

## 9. 点击穿透换实现（涉及 4 个文件）

v8 的写法是"`lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE)` + 挂 `LV_EVENT_HIT_TEST`
回调把 `res` 置 false"双保险。v9 里 `lv_hit_test_info_t` 定义挪进了私有头
`src/core/lv_obj_event_private.h`，公开 API 只有返回指针的
`lv_event_get_hit_test_info()`，**无法再访问 `->res`**。

**v9 其实不需要那个回调**：`lv_obj_hit_test()`（`src/core/lv_obj_pos.c`）第一句就是
`if(!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) return false;`，
而且 `LV_EVENT_HIT_TEST` 只在对象带 `LV_OBJ_FLAG_ADV_HITTEST` 时才派发。
所以只保留 `clear_flag(CLICKABLE)` 即可，回调全部删除：

| 文件 | 删除内容 |
|---|---|
| `core/ui/touch_test.c` | 1 个回调 + 1 处注册 |
| `core/ui/virtual_list.c` | 1 个回调 + 3 处注册（占位/图标/文本） |
| `core/ui/click_wheel.c` | 1 个回调 + 2 处注册（图标/指示圆点） |
| `ui/toast.c` | 1 个回调 + 2 处注册 |
| `ui/notification_panel.c` | 1 个回调 + 4 处注册（三列/header/内容区/把手） |

---

## 10. 内部 RAM 危机与"LVGL 对象堆进 PSRAM"（本次最大的一块）

### 10.1 症状

`lvgl/lvgl` 换成 9.4 后、其它都还没改完时就已经出现：

```
E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
abort() was called at PC ... lock_init_generic     ← newlib 建递归锁失败
  __sfp → _fopen_r → fopen → song_meta_load → cover_load_task
```

`MEMTRACE: cover:req  int 23/0(1196) | dma 19/0` —— **内部 RAM 剩 23 字节、DMA 剩 19 字节、
最大连续块 0**。`fopen` 建锁失败只是"压死骆驼的最后一根稻草"。

### 10.2 根因

`app_ui_create()` 一次性创建上千个 LVGL 控件对象，实测吃 **66 KB**。v8 时代这些对象走
`LV_MEM_CUSTOM=stdlib` → `malloc()` → **内部 RAM**，而 ESP32-S3 上 `MALLOC_CAP_DMA`
查询到的堆与内部 DRAM 堆高度重叠 —— 也就是说 **LVGL 对象一直在和绘制缓冲抢同一池子**。
v8 能勉强活着，v9 的对象/样式/绘制结构更大，直接见底。

### 10.3 方案：v9 的 `LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM`

v8 **没有**把对象堆换到外部内存的能力（只有"自带静态堆"或"标准 malloc"两选），
**v9 正式提供了**：Kconfig → `Malloc functions source` → `Implement the functions externally`，
由用户实现 `*_core` 系列。新增 `main/core/ui/lvgl_mem.c`：

```c
#define LVGL_MEM_PREFER   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)   /* 首选 8MB PSRAM */
#define LVGL_MEM_FALLBACK (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /* 失败回退内部 RAM */

void *lv_malloc_core(size_t size);        /* heap_caps_malloc(size, PREFER→FALLBACK) */
void  lv_free_core(void *p);              /* heap_caps_free：按指针所属堆区释放，两种来源通用 */
void *lv_realloc_core(void *p, size_t n); /* heap_caps_realloc：跨堆区自动搬移，失败时原指针有效 */
void  lv_mem_monitor_core(lv_mem_monitor_t *);   /* 汇报 PSRAM 堆状态 */
lv_result_t lv_mem_test_core(void);
void  lv_mem_init(void);   void lv_mem_deinit(void);        /* CUSTOM 模式没有自带 core 文件，必须自己给 */
lv_mem_pool_t lv_mem_add_pool(...);  void lv_mem_remove_pool(...);   /* 不支持，返回 NULL/空实现 */
```

整个文件用 `#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM` 包住：改回 CLIB/BUILTIN 时
LVGL 自带的 `stdlib/*/lv_mem_core_*.c` 会提供同名符号，不包就会重复定义。

**为什么安全**：

1. **刷屏缓冲不受影响**：两块 DMA 绘制缓冲由 `lvgl_port.c` 用
   `heap_caps_aligned_alloc(MALLOC_CAP_DMA, ...)` 自己分配，不走 `lv_malloc`，仍在内部 DMA RAM；
   本文件只接管 LVGL 内部对象与中间绘制层。
2. **关缓存窗口安全**：擦写 Flash / `pldata` mmap-munmap 会临时关外部内存缓存（PSRAM 不可访问）。
   这类操作全部经 `deep_worker` 串行执行，而 IDF 关缓存时会关中断并停住另一核，
   期间 LVGL 任务不会运行 —— 实测"重新扫描"整条深链（`LVGL task suspended` →
   `rescan_do_deinit` → 擦表头 → 重启）跑通，无异常。
3. **失败回退**：PSRAM 分配失败自动退回内部 RAM（只打印一次告警），功能不会因内存策略挂掉。

### 10.4 效果（同一时刻对比）

| 时刻 | 内部 RAM 空闲 | DMA 空闲 | 最大连续块 |
|---|---|---|---|
| CUSTOM 之前（`cover:req`，UI 创建中） | **23 B** | **19 B** | **0** |
| CUSTOM 之后（`ui:after app_ui_create`） | **42.3 KB** | **34.6 KB** | 22.5 KB |
| CUSTOM 之后（开机完成 `17-perf_monitor`） | 36.0 KB | 28.5 KB | 27.0 KB |

SD 的 `allocate_dma_buf` 失败与 newlib 建锁 abort 随之消失。

> ⚠ **上表是"加栈之前"的数字**（那时 LVGL 任务栈还是 6 KB）。为修 §11 的栈溢出把栈加到 12 KB
> 之后（任务栈从内部堆分配，而本板 `int` 与 `dma` 两池高度重叠、两者 free 只差固定 7716 B，
> 所以两边会同步下降），2026-10-15 复测**连续 4 次开机**（`idfc monitor --timeout`，每次均 reset）：
>
> | 时刻 | 内部 RAM 空闲 | DMA 空闲 | 最大连续块 |
> |---|---|---|---|
> | `ui:after app_ui_create` | **35.3 KB** | **27.6 KB** | **22.0 KB** |
> | 开机完成 `17-perf_monitor` | **29.5 KB** | **21.3 KB** | **20.0 KB** |
>
> 比上表低约 7 KB，其中 6 KB 就是栈本身（6→12 KB），残余约 1 KB 未逐一归因。
> 4 次开机的**空闲字节与内部最大连续块完全相同**（仅第 3 次的 DMA 最大连续块是 21.0 KB 而非
> 20.0 KB，属驱动侧分配次序的 1 KB 抖动），说明开机内存剖面基本是确定的——
> 将来这个数字一旦漂移，就是排查内存问题的第一现场。
> 结论不变：最大连续块仍 ≥ 20 KB，远高于本项目 4 KB 的底线（SD 读 512 B / 内部任务栈 4 KB）。

### 10.5 ⚠ 链接顺序坑：`undefined reference to 'lv_malloc_core'`

符号由 `main` 组件提供，但 LVGL 组件引用它，而 IDF 生成的最终链接顺序是

```
liblvgl__lvgl.a  →  libmain.a  →  liblvgl__lvgl.a
```

（main 依赖 lvgl，所以 lvgl 被重复一次）。问题在于：

- 第一遍扫 `liblvgl.a`：没有任何"当前未定义"符号需要它，一个 `.o` 都不拉；
- 扫 `libmain.a`：此时未定义集合来自 main 自己的目标文件（`lv_init` 等），
  **不含 `lv_malloc_core`**（main 不直接调它）→ 提供这些符号的 `lvgl_mem.o` 不会被拉进来；
- 第二遍扫 `liblvgl.a` 才拉入 `lv_mem.o`，它这才产生 `lv_malloc_core` 等未定义符号，
  而 `libmain.a` 已经扫过、不会回看 → `undefined reference`。

修法（`main/CMakeLists.txt`）：用 `-u` 在链接开始前就把符号标为未定义，强制扫 main 时拉入该 `.o`：

```cmake
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")
```

（一个 `.o` 里的其它符号随之全进来；改回 CLIB/BUILTIN 时该选项也无害。）

---

## 11. LVGL 任务栈：6 KB → 12 KB

**症状**：开机一切正常、能左滑，但**滑到第 4 页（播放设置）必定**
`***ERROR*** A stack overflow in task LVGL has been detected.` → panic 重启。

**根因**：LVGL 9 的绘制管线比 8.4 深得多（draw task / layer / 各混合函数层层下探）。
设置页是"首次渲染时最重"的页面（九宫格 + flex + 多个 label），刚好把 6 KB 顶穿。
实测迁移前 LVGL 任务栈高水位在 v8 下约 3.3 KB，v9 下达到 **5532 B**（接近 6 KB 上限）。

**处理**：`EXAMPLE_LVGL_TASK_STACK_SIZE` 6 KB → **12 KB**（`core/ui/lvgl_port.c`）。
改后实测高水位仍是 5532 B（余量 6.5 KB）。⚠ LVGL 任务栈不能走 PSRAM（每帧刷屏、
实时性要求），加大栈会吃内部 RAM：加栈前后内部空闲实测差约 6 KB（开机完成 36 KB → 29.5 KB，
最大连续块 27 KB → 20 KB，见 §10.4 的复测表），当前余量仍然充足。

---

## 12. 其它 API 改名清单（已改）

| 位置 | v8 | v9 |
|---|---|---|
| `ui/settings_menu.c` | `lv_btn_class` | `lv_button_class` |
| `ui/settings_menu.c` | `lv_img_class` | `lv_image_class` |
| `ui/settings_menu.c` | `lv_menu_set_mode_root_back_btn` / `LV_MENU_ROOT_BACK_BTN_ENABLED` | `lv_menu_set_mode_root_back_button` / `LV_MENU_ROOT_BACK_BUTTON_ENABLED` |
| `ui/settings_menu.c` | `lv_menu_back_btn_is_root` | `lv_menu_back_button_is_root` |
| `ui/settings_menu.c` | `lv_menu_get_main_header_back_btn` | `lv_menu_get_main_header_back_button` |
| `ui/settings_menu.c` | `lv_event_send(obj, code, param)` | **`lv_obj_send_event(...)`**（见 §7） |
| `ui/main_ui.c` | `lv_tabview_create(parent, dir, size)` | `lv_tabview_create(parent)` + `lv_tabview_set_tab_bar_position/size()` |
| `ui/now_playing.c` | `lv_obj_set_style_size(obj, v, part)` | `lv_obj_set_style_size(obj, w, h, part)` |
| `ui/now_playing.c` | `indev->proc.state` | `lv_indev_get_state(indev)` |
| `ui/boot_flow.c` | `timer->user_data` | `lv_timer_get_user_data(timer)` |
| `core/ui/virtual_list.c`、`ui/list_common.c` | `lv_mem_alloc` / `lv_mem_free` | **`lv_malloc` / `lv_free`**（v8 名字在兼容层里**没有**别名） |
| `core/image/*`、`core/ui/lvgl_port.h` | `lv_img_dsc_t` | `lv_image_dsc_t`（`lv_img_dsc_t` 仍是兼容 typedef，但新写的代码用新名） |

---

## 13. 全局跑马灯（`core/ui/global_style.*`）—— 反而**简化**了

v8 时代这个模块存在的唯一理由是 **8.4 的 `lv_label` 不认动画模板里的轮数**：
`lv_label_refr_text()` 建滚动动画时把 `repeat_cnt` 写死成 `LV_ANIM_REPEAT_INFINITE`，
模板里只有 `act_time`（起始延迟）和 `repeat_delay`（轮间停顿）被读取 —— 所以当时必须
"动画建好后就地改 `repeat_cnt` + 200ms 守护定时器兜底重建 + 48 项 label 监管表"。

**v9 已原生支持**：`lv_label.c` 的 `overwrite_anim_property()` 在每次重建跑马灯动画时会把模板里的
`act_time` / `repeat_cnt` / `repeat_delay` / `completed_cb` **全部抄进新动画**
（`LV_LABEL_LONG_SCROLL` 模式还会多抄 `reverse_delay`），并且会保留旧动画的播放位置
（`dest->act_time > 0` 时不被模板覆盖）。

因此迁移时把就地接管 + 守护定时器 + 监管表**全部删除**，模块缩到 ~40 行：
只维护一个全局 `lv_anim_t` 模板（`lv_anim_set_delay` / `set_repeat_count` / `set_repeat_delay`）
+ `lv_obj_set_style_anim()`，`lv_label_set_long_mode(同值)` 即"按全局节奏从头重跑"
（v9 的 `lv_label_set_long_mode` 会先删旧动画、把偏移复位到 0 再重建）。

顺带两个小坑：

- `LV_ANIM_REPEAT_INFINITE` 的值从 v8 的 `0xFFFF` 变成 **`0xFFFFFFFF`**，不能再写字面量；
- 样式属性 **`LV_STYLE_ANIM_SPEED` 被删除**（只剩 `anim_duration`，单位从"px/s"变成"毫秒"）。
  本项目原本 `GLOBAL_STYLE_MARQUEE_SPEED = 0`（用 LVGL 默认），故把宏改名成
  `GLOBAL_STYLE_MARQUEE_DURATION_MS = 0`，行为不变。

---

## 14. 构建系统变更

1. `main/CMakeLists.txt`：新增 `core/ui/lvgl_mem.c`；
2. 同文件末尾：新增 `target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")`
   （原因见 §10.5）；
3. `main/idf_component.yml`：`lvgl/lvgl` 版本 `"^8"` → **`"9.4.0"`**（锁定小版本，避免自动跳到 9.5/9.6）。

`managed_components/` 与 `build/` 都在 `.gitignore` 里，**不入库**；组件由 IDF 组件管理器按
`idf_component.yml` + `dependencies.lock` 重新拉取，故升级无需提交组件源码。

---

## 15. 残留观察与后续待办

1. **SD 一次性 DMA 失败**（未解决，自恢复）
   现象：一次开机扫描刚开始时 `E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101` +
   `E diskio_sdmmc: sdmmc_read_blocks failed`，随后扫描正常继续（同一轮扫完 700+ 文件）。
   当时 DMA 空闲 73 KB、最大连续块 31 KB，而失败路径理论上最多只要 512 B，**不像内存压力**。
   该报错在 v8 时代同样存在于本项目记录中（见 AGENTS.md 的 DMA 相关条目）。
   两轮共约 2.7 分钟的操作日志里只出现 1 次，未再复现，暂按"驱动侧瞬时失败"归档。
   2026-10-15 迁移完成后又连测 4 次开机（1×20 s + 3×15 s，含 reset）：**4/4 干净挂载**
   （`ZTPro / SDHC / 40 MHz / bus_width=4`），无 `allocate_dma_buf`、无 `diskio_sdmmc` 失败，
   4 次里唯一的 `W` 是下面第 6 条的 legacy i2c 驱动警告，**0 条 `E`**。
   若后续出现"随机跳曲/误判坏曲"，优先看这里。
2. **`releasecandidate` / `release` 两套配置尚未构建验证**（本次只要求 debug）。
   差异点在 Kconfig 覆盖（字体完整版、封面变体、`LV_USE_PERF_MONITOR` 关）。
   ⚠ `sdkconfig.defaults.release` 里仍是 `# CONFIG_LV_USE_PERF_MONITOR is not set`，v9 下同样有效；
   但若要开 perf monitor，记得同时开 `CONFIG_LV_USE_SYSMON`（见 §2）。
3. **`GLOBAL_STYLE_MARQUEE_DURATION_MS`** 现在是毫秒时长（v8 是 px/s 速度）。
   默认 0（用 LVGL 默认时长）；想调快/调慢时注意单位换算（一轮时长 = 滚动距离 / 速度）。
4. **LVGL 组件自身的编译告警**（未处理）：
   `ignoring attribute 'section (".iram1.N")' because it conflicts with previous ...`
   —— 上游 `lv_color.c` / `lv_math.c` / `lv_color_op.c` 里 `IRAM_ATTR` 的内联/多段冲突，
   由 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` 触发，不影响功能。
5. **可选优化**：现在 LVGL 对象在 PSRAM，样式/布局访问会走外部内存。若后续发现帧率不足，
   可把 `lvgl_mem.c` 改成"小分配走内部、大分配走 PSRAM"的分流策略（v9 的 CUSTOM 模式下改一处即可），
   但要重新量内部 RAM 余量（当前开机完成约 29.5 KB、最大连续块 20 KB）。
6. **每次开机 1 条 legacy i2c 驱动警告**（与本次升级无关，预存在）：
   `W (303) i2c: This driver is an old driver, please migrate your application code to adapt driver/i2c_master.h`
   —— 来自 `components/i2c_bsp` 用的旧版 `driver/i2c.h`（触摸 I²C 总线）。功能无影响；
   想消掉需要把 i2c_bsp 迁到 `driver/i2c_master.h`（独立任务，不在本次迁移范围内）。
