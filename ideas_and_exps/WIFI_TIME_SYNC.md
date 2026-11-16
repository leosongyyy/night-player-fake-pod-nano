# 网络对时（WiFi + SNTP）与「时间与日期」设置

> 2026-11-14 接手「两颗新芯片（AXP2101 / PCF85063A）适配」之后的新需求：
> 在设置菜单里加入 **时间与日期**、**电源选项（先占位）** 两个界面，时间页
> 支持手动改日期/时间、时区设置与**网络对时**。
> 本文记录设计、实现位置、内存/构建影响、真机验证方式与已知限制。

---

## 1. 需求与最终交互

设置菜单根页面（在「按键帮助」之前插入两行）：

```
睡眠定时器 / 屏幕自动息屏 / 虚拟 ClickWheel 选项 /
时间与日期 / 电源选项 / 按键帮助 / 关于 / 重新扫描
```

- **时间与日期**（子菜单，五项，右侧显示当前值）：

  | 行 | 右侧状态 | 点击 |
  |---|---|---|
  | 设置日期 | `2026-11-14` | 弹出「年 / 月 / 日」三滚轮弹窗，保存后写 RTC |
  | 时间设置 | `12:41:05`（每秒刷新） | 弹出「时 / 分 / 秒」三滚轮弹窗，保存后写 RTC |
  | WiFi对时 | — | 全屏进度界面：读 wifi.txt → 连 WiFi → NTP → 写 RTC |
  | 时区设置 | `UTC+8` | 弹出单滚轮时区列表，保存后平移 RTC 并持久化 |
  | 时间格式 | `24 小时制` / `12 小时制` | 点击切换（12h 不显示上下午），影响状态栏/下拉栏时钟 |

- **电源选项**（2026-11 当天晚些已补齐，不再是空白页）：电池信息四项
  （典型容量 / 当前电量 / 充电中 / 电池电压，后三项每秒刷新，仅本页可见时读 PMU）
  + **电池健康保护**开关（软件限充，停充点见 `BATTERY_CARE_STOP_MV`，点击立即生效）+ 一条说明；PMU 不可用或
  读数越界时统一显示"未知"，不显示假的 0。低电策略已按电量百分比接回（开机 <2%、使用中 <5%·60s，见 §8）。

- 网络对时全屏进度界面：黑底、标题「WiFi对时」、当前阶段（第 n/N 次）、
  进度条 + 百分比、「取消」按钮；失败显示具体错误与「关闭」；成功后
  2.5 秒自动关闭。进度期间 `power_service_set_hold_awake(true)` 抑制自动息屏
  （流程最长可达一分多钟，否则 30 秒就会黑屏）。

## 2. wifi.txt 格式

路径固定：**`/sdcard/.ecov/wifi.txt`**（UTF-8；与 `.npd` 侧车文件同目录）

```
第一行：WiFi SSID
第二行：WiFi 密码（开放网络留空）
第三行：可选，NTP 服务器域名 / IP（缺省 ntp.aliyun.com）
```

实现细节（`core/network/wifi_time_sync.c` 的 `read_wifi_config`）：

- 容忍 UTF-8 BOM 与 CRLF；最多读 3 行，之后内容忽略；
- SSID 与 NTP 服务器**裁掉两端空白**；密码原样保留（只去行尾 `\r`），
  避免误伤以空格开头/结尾的密码；
- SSID 空 / 超过 32 字节、密码超过 64 字节、服务器域名超过 63 字节 →
  `ERR_BAD_FILE`；SD 未挂载 → `ERR_NO_SD`；文件不存在 → `ERR_NO_FILE`。

## 3. 对时流程与失败语义

`wifi_time_sync_run(cb, ctx, cancel)` 是**阻塞**接口，由 `ui/time_sync_ui.c`
在独立任务里调用（栈 8KB，**内部 RAM**；WiFi 初始化会写 NVS/PHY 校准，
PSRAM 栈任务会命中 IDF 关缓存断言）。每次 run 完整
`esp_netif_init → esp_netif_create_default_wifi_sta → esp_wifi_init →
esp_wifi_start → connect → SNTP → stop/deinit → destroy netif`，
退出后不保留 WiFi 资源，也不写 WiFi 配置到 NVS（`WIFI_STORAGE_RAM`）。

| 阶段 | 上限 | 单次超时 | 说明 |
|---|---|---|---|
| WiFi 连接 | 3 次（含首次） | 15 s | 认证类失败（密码错/握手超时）**立即终止**，不再浪费剩余次数 |
| 获取 IP | 由连接事件携带 | — | `IP_EVENT_STA_GOT_IP` 即视为连接成功 |
| NTP | 5 次（含首次） | 8 s | 每次重新 `esp_sntp_stop/init`；先做一次 DNS 解析以区分"域名错"与"服务器无应答" |

- 认证失败 → `ERR_WIFI_AUTH`（"WiFi 密码错误或认证失败"）
- 扫不到 AP（含安全模式不匹配）→ `ERR_WIFI_NOT_FOUND`
- 其余连接失败/超时 → `ERR_WIFI_TIMEOUT`
- DNS 失败 → `ERR_NTP_DNS`；5 次无应答 → `ERR_NTP_TIMEOUT`
- 任何阶段都可取消：等待循环以 100 ms 为粒度检查 `cancel`，WiFi/SNTP
  在 cleanup 阶段停止并释放。

## 4. 时间语义（关键设计）

PCF85063A **没有时区概念**，项目约定 **RTC 里存本地墙钟时间**：

- 手动设置日期/时间：用户看到什么就写什么；
- 网络对时：SNTP 给 UTC，写 RTC 时 `local = utc + tz_offset_min`；
- 修改时区：把 RTC 平移 `new_offset - old_offset` 分钟（用 `timegm/gmtime_r`
  做绝对时刻运算），这样"改时区"不会改变绝对时刻；
- 时区默认 **UTC+8**，存 NVS 全局键 `tzoff`（`config_store_get/set_tz_offset_min`，
  编码为 `offset + 1440` 的 u32），不进配置槽位。

> 为什么不做"RTC 存 UTC、显示时加偏移"：现有 RTC 首次校时写的是项目诞生日
> （+0800 的本地时间），而且当前没有任何其它模块读 RTC；直接沿用"RTC = 本地
> 墙钟"能保持既有行为不变，代价只是改时区时要平移一次 RTC。

时区滚轮覆盖 UTC-12:00 ~ UTC+14:00 的常用档位（含 :30 / :45），见
`ui/time_date_popup.c` 的 `TZ_TABLE`；`config_store_set_tz_offset_min` 会把
任意值收敛到 15 分钟粒度。

## 5. 代码位置

```
main/core/network/wifi_time_sync.h/.c   # WiFi + SNTP 核心（阻塞、可取消、无 UI）
main/ui/time_date_popup.h/.c            # 日期/时间/时区三个滚轮弹窗（lv_layer_top，半透明遮罩）
main/ui/time_sync_ui.h/.c               # WiFi对时全屏进度 + 内部 RAM 栈任务（屏幕层）
main/ui/settings_menu.c                 # 根页面两个入口行 + 时间与日期子页
main/core/storage/config_store.*        # tzoff 键
main/core/utils/power_service.*         # power_service_set_hold_awake()
```

UI 约定：

- **日期/时间/时区三个滚轮弹窗**挂 `lv_layer_top()`：它们是"半透明全屏遮罩 + 卡片"，
  必须叠在屏幕层（设置菜单/状态栏）之上，且半透明父对象无法参与 LVGL 的覆盖判定
  （放进屏幕树会丢失调暗），所以只能留在顶层；
- **对时进度界面**是**全屏不透明**覆盖层，挂**屏幕层**（`lv_screen_active()`）：
  顶层对象不在 `lv_refr_get_top_obj()` 的搜索范围内，会让被盖住的整屏内容每帧重绘
  （见 `DONE_PLANS.md` 第 38 点）。对时固件里屏幕本身是空的，这一改动主要是防止
  以后复用时踩坑；
- 弹窗点击面板外 = 取消；
- 时间页面有一个 1 s 的 `lv_timer` 刷新"时间设置"右侧秒数，只在
  `lv_obj_is_visible(page)` 为真时读 RTC（I²C 读一次约 1 ms）；
- 弹窗保存/对时成功后经 `time_date_popup_notify_changed()` 回调设置菜单刷新
  日期/时间/时区三行文字。

## 6. 构建与内存影响

### 6.1 组件白名单

为接入网络，根 `CMakeLists.txt` 的 `COMPONENTS` 重新加入：

```
esp_wifi  esp_netif  esp_event  lwip  wpa_supplicant
```

`sdkconfig.defaults` 同时补：

```ini
CONFIG_ESP_NETIF_TCPIP_LWIP=y          # 旧 sdkconfig 残留了 "not set" + LOOPBACK=y
# CONFIG_ESP_NETIF_LOOPBACK is not set
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n       # 只用 STA
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n   # 不连企业级 WiFi
CONFIG_LWIP_DHCPS=n                    # 不做 AP，不需要 DHCP 服务器
CONFIG_LWIP_SNTP_STARTUP_DELAY=n       # 否则 init 后白等最多 5s

# WiFi 只用于一次性的自动对时：库从 IRAM 挪回 Flash、收发缓冲缩到 6/4/8
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
CONFIG_ESP_WIFI_EXTRA_IRAM_OPT=n
CONFIG_ESP_WIFI_SLP_IRAM_OPT=n
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8
```

### 6.2 实测（debug 构建，2026-11-14）

| 项目 | 数值 |
|---|---|
| 固件体积 | 0x209940 ≈ **2.14 MB**（factory 5.9375 MB，余 66%） |
| 开机完成内部 RAM | **112,011 B free / 47,104 B 最大连续块**（`ui:after app_ui_create`） |
| 开机完成 DMA | 80,019 B free / 47,104 B 最大连续块 |
| `esp_wifi_init` 后 | 内部 RAM 63,735 B free / 31,744 B 最大连续块；DMA 31,743 / 30,720（≈ WiFi 吃掉 ~48KB 内部 RAM） |

> ⚠ 无 WiFi 组件时开机内部 RAM 约 167KB（见 `IRAM_OBJECTS_PSRAM_S31.md`）。
> 加入 WiFi/SNTP 库（IRAM 优化全关）后掉到 112KB，仍足够：WiFi 初始化后
> 还有约 61KB 内部 RAM / 30KB 连续块，SD 读与 LVGL 绘制不受影响。

### 6.3 踩坑：默认 WiFi 缓冲在真机上初始化失败

第一版按 IDF 默认配置（静态 RX 10 / 静态 TX 16 / 动态 RX 32）真机点「WiFi对时」
报：

```
W (87331) wifi:malloc buffer fail
E (87342) wifi:Expected to init 10 rx buffer, actual is 0
E (87379) wifi_time_sync: esp_wifi_init: ESP_ERR_NO_MEM
```

原因：WiFi 初始化要一次性申请约 23KB 连续内部 RAM（DMA）作为静态 RX 缓冲，
开机后内部 RAM 最大连续块只剩 ~31KB，菜单/对时 UI 再占用一点就失败。
把静态 RX/TX 降到 6/4 后，一次成功（probe：`esp_wifi_init -> ESP_OK`，
之后内部 RAM 63.7KB free / 31.7KB 连续块）。**这是本项目的第一条 WiFi
内存红线**：以后若再给 WiFi 加缓冲/功能，先看 `esp_wifi_init` 的返回值与
`MEM:` 行，不要只看总剩余量，**最大连续块才是关键**。

> 另注：验证时旧固件（默认缓冲）在"设置菜单已打开 + 对时界面已创建"的
> 状态下失败；现在 6/4 缓冲即使在该状态下也有足够余量。

### 6.4 附带修复：S31 分支预测 / cache 竞态（不是 BOYA 颗粒问题）

接入 WiFi 后固件从 1.49MB 涨到 2.14MB，随后开始偶发**开机第一次读 NVS 就
panic（Cache error）**。一开始误以为是 BOYA flash 颗粒，后来用源码对比排除了：
IDF 的 BOYA 驱动与通用驱动除了 `probe()`/`name`/`caps` **实现完全相同**，
不可能修复该 panic。真实根因是 **ESP32-S31 双核分支预测器 / cache 竞态**
（espressif/esp-idf#18948 / IDFGH-18107）：一颗核 suspend cache 做 flash 操作时，
另一颗核在 IRAM 自旋里的分支预测器仍会投机取指到 flash，触发 cache access-fail。
本项目 IDF 6.1.0 快照不含上游修复，因此把 `spi_flash` 组件复制进项目并打了
补丁（`components/spi_flash/cache_utils.c`）。**注意：BOYA 驱动已按用户要求回退，
开机日志仍会有 `Detected boya flash chip but using generic driver`，属正常提示。**

完整的排查过程、芯片对比、DIY 换颗粒兼容性、补丁位置与验证数据见
**`ideas_and_exps/S31_FLASH_CACHE_RACE.md`**。

> ⚠ 这与此前"大范围改写 flash 会出问题"的直觉不同：panic 发生在**读** NVS 时，
> 擦写范围不是触发条件；新固件改变代码布局/分支历史才让它偶发暴露。
> 换其它品牌同容量 flash **不能**规避该 panic。

## 7. 真机验证清单

> **2026-11-14 用户真机验证通过**：WiFi对时已完成"读 wifi.txt → 连接 WiFi →
> NTP → 写 RTC"全流程。时间与日期页最终只保留四个选项 + 一行说明
> 「* WiFi对时可在电脑版工具里配置」（原来的两段长说明已按用户要求删除）。

1. 往 SD 卡 `.ecov/wifi.txt` 写入 SSID / 密码 /（可选 NTP），UTF-8 保存；
2. 设置 → 时间与日期 → 设置日期/时间 → 滚轮改值 → 保存 → 返回后右侧文字立即变化；
3. 时区设置改成 UTC+9 → 保存 → 右侧时间应 +1 小时（绝对时刻不变）；
4. WiFi对时：
   - 正常：进度依次显示"读取配置 / 连接 WiFi / 获取 IP / 等待网络时间 / 写入时间"，
     成功页显示新的本地时间，2.5s 后自动关闭；
   - 断网/密码错/无 wifi.txt/SD 卡拔出：分别显示对应错误，点「关闭」返回；
   - 流程中点「取消」：1s 内退出，WiFi 停止；
   - 对时期间放置 30s+ 不触摸，屏幕不应自动息屏（hold awake 生效）；
5. 对时后重启，RTC 仍走时（PCF85063 有备用电源时）。

## 8. 已知限制 / 后续

- 只支持手动触发，不提供"开机自动对时"开关（需求未要求；当前也没有
  联网待机的功耗预算）。
- 只配一个 NTP 服务器（`CONFIG_LWIP_SNTP_MAX_SERVERS=1`），第三行写什么
  就用什么；不做 NTP 池轮询。
- 时区列表是固定常用档位，不做"按国家/城市选择"或夏令时。
- 电源选项页已实现电池信息 + 电池健康保护（软件限充，阈值见 `battery.h` 的宏）；**低电策略（拒启/自动关机）
  已按电量百分比接回**（开机 <2% 提示 5 秒后关机；使用中 <5% 且未充电持续 60s 关机），见 `PMU_RTC_DRIVERS.md` §7。

## 9. 2026-11-15 暂时下线（换内部 RAM / UI 帧率）

### 9.1 为什么

蓝牙接入后内部 RAM 已经很紧，而 WiFi 协议栈是**常驻**成本：`esp_wifi` 预编译库
+ lwip + wpa_supplicant + esp_netif 的静态 `.bss/.data` 约 27KB，代码进 Flash 后
镜像还大 590KB。它不参与 UI 路径，却一直占着内部 RAM，把 LVGL 绘制缓冲挤到
`V_RES/16`、帧率比加蓝牙前低 20~40%。

双固件（主播放器 + 最小对时固件，按需切换启动）是更彻底的解法，调研见
**`ideas_and_exps/INNER_WORLD.md`**：可行、不需要自定义 bootloader，
但要动分区表 + 两套构建/烧录。**在它落地之前**，先把 WiFi 整个摘掉，把 RAM 还给 UI。

### 9.2 怎么下线（入口保留）

| 位置 | 改动 |
|---|---|
| 设置 → 时间与日期 → **WiFi对时** | **入口保留**；点击只弹 Toast「WiFi对时暂未实现」，不重启、不联网。说明行同步改成「* WiFi对时暂未实现，后续版本恢复」 |
| `main/CMakeLists.txt` | `core/network/wifi_time_sync.c`、`ui/time_sync_ui.c` 两行注释掉（源文件留在树里） |
| 根 `CMakeLists.txt` | `COMPONENTS` 白名单里 `esp_wifi / esp_netif / esp_event / lwip / wpa_supplicant` 五行注释掉，并写明恢复步骤 |

**恢复只要三步**：取消白名单那 5 行 → 取消 `MAIN_SRCS` 那 2 行 →
`settings_menu.c` 的 `time_date_sync_row_cb` 改回 `time_sync_ui_start()`。

### 9.3 实测（debug，链接期 / `idfc size`）

| 项目 | 下线前 | 下线后 | 变化 |
|---|---|---|---|
| **DIRAM 静态占用** | 217,436 B | **186,602 B** | **−30,834 B** |
| · `.bss` | 88,592 | 64,488 | −24,104 |
| · `.data` | 23,620 | 17,996 | −5,624 |
| · `.text`(IRAM) | 105,224 | 104,118 | −1,106 |
| Flash | 2,920,638 B | **2,332,630 B** | **−588,008 B** |
| 镜像 | 0x2C9xxx（≈2.92MB） | **0x257540（2.45MB）** | — |

> DIRAM 是 IRAM 代码 + 内部 DRAM 共享的那个池（S31 上 503,744 B），所以
> "静态少 30.8KB"就是 UI 能多拿到的内部 RAM（还没算本节一起做的栈回收）。

### 9.4 同一批改动里顺带回收的

| 改动 | 回收 | 说明 |
|---|---|---|
| LVGL 任务栈 20KB → **12KB** | 8KB | 位图字体（NPF1）时代实测 HWM 5.5KB；20KB 是 TTF 时代为 FreeType 光栅化池留的 |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 16KB → **8KB** | 8KB（仅开机期间） | 绘制缓冲正是在 `app_main` 里分配的，这个栈活着会直接占连续块 |
| 时区表改**动态生成** | ~1.1KB 静态 | 去掉 `TZ_TABLE`（带城市名）+ `s_tz_opts`，只生成 `GMT±H[:MM]`，见 §9.5 |
| LVGL 绘制缓冲 `V_RES/16` → **`V_RES/10`** | 花掉 33KB DMA | 加蓝牙前的帧率配置；分配失败会自动降档 `/12`、`/16`（不再 assert 挂启动） |

### 9.5 时区表（2026-11-15 改）

原来是一张 `static const` 的"UTC±H[:MM] + 城市名"表（40 档）外加一份
`s_tz_opts[40][28]` 的静态字符串副本（约 1.1KB `.bss`）。现在改成**按需生成**：

- 范围 −12:00 ~ +14:00、步长 15 分钟（与 `config_store_set_tz_offset_min` 的
  15 分钟粒度一致），共 105 档；
- 标签只有 `GMT+8` / `GMT+5:30` / `GMT`（**不带城市/地区**）；
- 选项串在打开弹窗时拼到一块 `lv_malloc`（PSRAM）缓冲里，`lv_roller_set_options`
  内部会复制，拼完即释放 → 静态 0，栈也几乎不涨；
- 设置页右侧状态行同步从 `UTC+8:00` 改成 `GMT+8`（`time_date_format_tz`）。
- 若嫌 105 档滚起来太长，把 `TZ_STEP_MIN` 改成 30（常见时区够用，代价是
  +5:45/+8:45/+12:45 会被取整）。

### 9.6 下线后实测（2026-11-15 真机，debug）

- 开机日志确认 `draw buffers: V_RES/10 (42368 B/块, 双缓冲 84736 B, DMA 余量 203163 B)` —— `/10` 一次分配成功；
- **BT 关闭**：`MEM: iram=110,479`，`MEMTRACE 17-perf_monitor` 的 DMA 余量 69,663 B（最大连续块 36,864）；
- **BT 开启并连上耳机、播放中**：`MEM: iram=58,131`、`fps=57~62`（静止帧率上限仍 61~62）；
- LVGL 12KB 栈跑完整个开机（含首次构建四页 UI、封面解码、状态栏/控制中心）无 `stack overflow`。

仍待人工验证：设置菜单**首次打开**（懒加载、最深的渲染路径）、时区弹窗（GMT 列表）、
息屏→亮屏全屏重刷、以及"歌曲列表快速滚动"的 `fps=` 与加蓝牙前的对比。
⚠ 12KB 栈若在设置菜单首开时溢出，按 `BLUETOOTH_AUDIO.md` §4.1 退回 16KB。

WiFi 对时若要回来，优先考虑 `INNER_WORLD.md` 的双固件方案（零常驻），
而不是把 27KB 再放回来。

---

## 10. 2026-11-15 落地：改走双固件（本节是上面 §9 的后续）

§9 的"暂时下线"已经**被双固件方案取代**：WiFi 协议栈不进播放器固件，而是进一个
**独立的里世界固件**（烧在 `ota_0`，矩阵 `role=inner`，2026-11 起同时提供 USB 传歌），
表世界点「WiFi对时」时弹统一确认窗，用 `esp_ota_set_boot_partition()` 切过去；
里世界点「回到表世界」再重启回来。

- 播放器侧内部 RAM 回到未接 WiFi 的水平（`iram=109,987`，fps 61~63），
  **不需要**再把绘制缓冲降到 `V_RES/16`；
- 根 `CMakeLists.txt` 里那 5 个 WiFi 组件**重新进了白名单**（否则里世界固件编不了），
  但播放器不引用它们 → 链接期 0 个 `.obj` 进镜像，Flash/RAM 都不受影响；
- `main/CMakeLists.txt` 的源文件按 `CONFIG_FIRMWARE_ROLE_SYNC` 拆成
  "两个固件共用 / 只表世界 / 只里世界"三份；
- 设置菜单入口不再是 Toast，而是"统一确认弹窗 → 切固件 → 重启 → 里世界功能列表"。
- **2026-11 起表世界不再在开机时 Toast 对时结果**（NVS `tsync/res` 交接已删除），
  对时结果只在里世界列表右侧显示。
- 「时间与日期」页底部的说明行：① WiFi 名称/密码写在 SD 卡 `.ecov/wifi.txt`
  （第 3 行可选 NTP 服务器）；② WiFi对时在「里世界」里完成，点它会重启进去，
  对完点「回到表世界」返回。

**完整方案、构建烧录命令、实测时间线与回退办法见 `INNER_WORLD.md`。**
本文 §1~§8 描述的 `wifi_time_sync.c` 本身（wifi.txt 格式、连接重试、NTP 判定、
UTC→墙钟写入 RTC）**没有改变**，仍然是里世界的核心功能之一。


