# 把 LVGL 热点对象从内部 RAM 放回 PSRAM（ESP32-S31 复测）

> 背景：S3 → S31 迁移后，为了让出内部 RAM 给未来的 WiFi/BT，重新评估
> 2026-10 在 S3 上做的"路径B：把两个滚动列表页的热点 LVGL 对象钉在内部 RAM"
> 是否还值得保留。结论：**关掉，帧率可以接受（真机确认），保留 PSRAM 方案**。

---

## 0. 结论摘要

1. **改动**：新增 Kconfig `CONFIG_UI_LVGL_PIN_HOT_OBJECTS`（默认 **n**）。
   关闭时 `lvgl_mem_set_internal_prefer()` 是空操作，`main_ui.c` / `virtual_list.c`
   的窗口调用全部保留但不生效，**两个滚动列表页的对象（含文本缓冲）一律走 PSRAM**。
2. **内部 RAM**（`mem_monitor` 的 `iram=`，即 `MALLOC_CAP_INTERNAL|8BIT` 空闲）：
   - 稳态：**147,175 B → 167,707 B（+20,532 B，约 +20 KB）**
   - 开机完成最大连续块：**31,744 B → 86,016 B（+54,272 B，约 +53 KB）**
   - DMA 池同步受益（同一块 SRAM）：最大连续块 31,744 → 86,016 B
3. **帧率**（真机、歌曲列表快速连续上下滚动）：**24~36 fps**（多数样本 25~35），
   与改动前文档记录的"约 30fps"同一量级；用户实机确认"帧率可以接受"。
   静止/跑马灯状态下 62fps（刷新周期 16ms 的上限），CPU0 仅 4~5%。
4. **只省 20 KB 而不是 S3 时代的 82.8 KB**：`LVGL_INTERNAL_RESERVE` 是 40 KB 的
   软阈值，窗口内内部 RAM 一接近它就不再钉，实际只钉进去约 20.8 KB。
   换句话说 **S31 上路径B 本来就没有钉满**，关掉它的收益上限也就这么大。
5. **对 WiFi/BT 的意义**：+20 KB 稳态余量、+53 KB 最大连续块。后者比前者更关键
   —— 大栈任务（LVGL 20KB、deep_worker 12KB）与 WiFi/BT 的启动期大块分配都要求
   连续块，而 S3 时代实测内部 RAM 的最大连续块曾低到 3.5 KB（建任务失败）。
6. **回退方式**：`idfc menuconfig` 或 `sdkconfig.defaults` 里把
   `CONFIG_UI_LVGL_PIN_HOT_OBJECTS` 设为 `y` 即可恢复，**不需要改任何代码**。

---

## 1. 背景：为什么当年要钉内部 RAM，现在为什么可以放开

- **S3 时代（`IRAM_AUDIT_LVGL9.md` §12/§13）**：LVGL 对象放内部 RAM 时列表快速滚动
  21fps，放 PSRAM 只有 11fps（对象工作集约 88KB，远超 S3 的 32KB 数据 cache）。
  但**全部**对象放内部 RAM 会直接见底（§11 两次失败：SD 读 512B 失败 → 误判坏曲、
  newlib 建锁 abort），所以才有了"按创建时机圈定"的路径B：只在创建两个滚动列表页
  的那一小段时间把 LVGL 分配钉到内部 RAM。
- **S31（2026-11 迁移后）**：320MHz、512KB SRAM，内部 RAM 比 S3 时代宽裕得多
  （开机 `01-config_store` 时内部堆约 337KB），而后续要接 **WiFi 6 / BT 5.4 LE**
  —— 两者都要吃内部 RAM 与 IRAM（例如 IDF 的 `ESP_WIFI_IRAM_OPT` 默认 y，
  关掉它能省 10KB+ IRAM；BT 侧也有 `BT_RELEASE_IRAM` 之类的取舍）。
  在"无线协议栈还没有接进来、内存账还没算"的时候，先把能省的非必要占用省掉，
  比等接完 WiFi/BT 再回头挤更稳妥。

---

## 2. 改动内容

| 文件 | 改动 |
|---|---|
| `main/Kconfig.projbuild` | 新增 `UI_LVGL_PIN_HOT_OBJECTS`（bool，默认 n，中文 help 说明来龙去脉与回退方式） |
| `main/core/ui/lvgl_mem.c` | 路径B 的窗口/统计/安全阀整段用 `#if CONFIG_UI_LVGL_PIN_HOT_OBJECTS` 包住；关闭时 `lvgl_mem_set_internal_prefer()` 退化为空函数（`LV_UNUSED`），`lv_malloc_core` / `lv_realloc_core` 里的钉住分支不参与编译 |
| `main/core/ui/lvgl_mem.h` | 文档说明默认关闭；调用点无需 `#if` |
| `main/ui/main_ui.c`、`main/core/ui/virtual_list.c` | 只更新注释（调用保留，Kconfig 关闭时是空操作） |
| `main/core/ui/lvgl_port.c` / `.h` | 新增 `lvgl_port_get_refr_count()`：用 `LV_EVENT_REFR_READY` 累计刷新帧数（原来 FPS 只画在屏幕角落，串口抓不到，无法做 A/B） |
| `main/core/utils/performance_monitor.c` | `MEM:` 行末尾新增 `fps=`（每 2s 按刷新计数差值计算；静止时为 0 属正常） |

> 设计取舍：**不删调用点、不删路径B 代码**，只加一个编译期开关。业务代码保持干净，
> 以后若要回退只改一行配置；`fps=` 统计则是这次 A/B 的副产品，对以后调 UI 性能同样有用。

---

## 3. 实测数据（同一台 S31、同一份 debug 构建、同一天）

### 3.1 内部 RAM / DMA

| 时点 | 改动前（路径B 开） | 改动后（路径B 关） | 差 |
|---|---|---|---|
| `ui:before app_ui_create` | 193,347 / 129,024 | 193,363 / 129,024 | ≈0 |
| `ui:after app_ui_create` | 135,127 / 33,792 | 155,963 / 90,112 | **+20,836 / +56,320** |
| `17-perf_monitor`（开机完成） | 130,159 / 31,744 | 150,691 / 86,016 | **+20,532 / +54,272** |
| 稳态 `MEM: iram=` | **147,175** | **167,707** | **+20,532** |
| 稳态 DMA 最大连续块 | 31,744 | 86,016 | **+54,272** |

（格式：内部 RAM 空闲 / 最大连续块，单位字节。`iram=` 是
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`，不是指令 RAM；
S31 上 D/IRAM 仍是同一块 SRAM 的别名，代码进 IRAM 与对象占内部 RAM 在同一个池子里对冲。）

**UI 创建段消耗**：路径B 开 = 58,220 B，关 = 37,400 B，差 **20,820 B** —— 这就是
S31 上实际被钉住的量。对照 S3 时代的 82.8 KB，说明 `LVGL_INTERNAL_RESERVE`（40KB）
在 S31 的 UI 创建过程中很早就触发了，**路径B 并没有把两个列表页全部钉进去**。

### 3.2 帧率（真机）

| 场景 | fps | CPU0 |
|---|---|---|
| 歌曲列表快速连续上下滚动（约 10s） | **27 / 24 / 25 / 35 / 34**（相邻 2s 采样） | 92~98% |
| 滚动结束后静止（跑马灯动画仍在跑） | 62（=1000/16ms 上限） | 4~5% |

- fps 由 `lvgl_port_get_refr_count()` 的 `LV_EVENT_REFR_READY` 计数换算，
  与 LVGL 屏幕角落的 perf monitor 同一口径；
- 滚动时的 CPU0 已经跑到 92~98%，说明限制来自 **UI 单核（LVGL 任务）**
  而不是"没有内容可刷"；
- 用户实机确认"**帧率可以接受**"，决定保留 PSRAM 方案。

> 参考：`sdkconfig.defaults` 里记录的 S31 迁移实测（路径B 开、40MHz QSPI）
> 为"列表滚动已能满 30fps"。本次关闭路径B 后 24~36fps 属同一量级，
> 代价在肉眼可接受范围内。

---

## 4. 为什么是 +20KB，而不是 S3 的 +82.8KB

`lvgl_mem.c` 的窗口分配条件是：

```c
if (s_internal_prefer_depth > 0 &&
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
        LVGL_INTERNAL_RESERVE + size) { ... }
```

`LVGL_INTERNAL_RESERVE = 40KB`。UI 创建开始时内部 RAM 约 193KB，但 LVGL 对象
创建得很快，窗口内还有封面/其他分配穿插，`ui:after` 时内部 RAM 已降到 135KB；
实际钉住 20.8KB 后就停止（否则 S3 时代也不会在 `ui:after` 只剩 11KB）。

这也解释了为什么**不要指望关掉路径B 能省出 80KB**：S31 上它本来就没钉满。
真正的大头仍是 LVGL 绘制缓冲（DMA，约 84.6KB）、任务栈与静态段；
如果要继续给 WiFi/BT 腾内部 RAM，应该按 `IRAM_AUDIT_LVGL9.md` §5 的顺序
（绘制缓冲高度、音频静态缓冲、任务栈）继续做，而不是继续在对象位置上打转。

---

## 5. 复现方法

```powershell
# 1) 关闭路径B（默认即关闭）
idfc build                       # 改 Kconfig 后会自动 reconfigure
idfc flash

# 2) 看开机各阶段内部 RAM/DMA
idfc monitor --timeout 30000 --grep "MEMTRACE|MEM:"

# 3) 看滚动帧率：MEM: 行末尾的 fps= 就是最近 2s 的平均刷新率
idfc monitor --timeout 60000 --grep "MEM:"
#    然后在设备上快速滚动歌曲列表 10~15s
```

判定要点：

- `MEM:` 行里 `iram=` 应比改动前高约 20KB；
- `fps=` 在滚动时应有 20~40 的读数，静止时降到 0（或跑马灯导致的 60 左右）；
- 若出现 `allocate_dma_buf: not enough mem` / 判坏曲跳曲，说明内部 RAM/DMA 又见底，
  先看 `MEM: iram=` 与最大连续块，再考虑把 `UI_LVGL_PIN_HOT_OBJECTS` 改回 y。

---

## 6. 注意点（留给以后）

1. **`fps=` 静止时为 0 是正常的**：它统计"真的发生刷新"的帧；界面完全静止时
   LVGL 不重绘。跑马灯/动画在跑时会有读数。
2. **不要用 `LV_USE_PERF_MONITOR_LOG_MODE` 来做这件事**：那会把屏幕角落的 FPS
   标签隐藏掉；本次的 `lvgl_port_get_refr_count()` 与它互不影响，屏幕标签照常显示。
3. **回退只改 Kconfig**：`CONFIG_UI_LVGL_PIN_HOT_OBJECTS=y`；不要修改
   `main_ui.c` / `virtual_list.c` 的调用结构（窗口嵌套与失败路径关闭都在原实现里）。
4. 若以后 S31 接上 WiFi/BT 后发现内部 RAM 仍紧张，优先级建议：
   ① 绘制缓冲高度（拿帧率换内存，需实测）
   ② 音频静态缓冲搬 PSRAM（`read_buf` 16KB / `silence_flush` 11.25KB 等）
   ③ 任务栈（LVGL 20KB 有约 8KB 余量，可按 `MEM: stk:` 实测收紧）。
