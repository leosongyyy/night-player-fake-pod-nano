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

- **时间与日期**（子菜单，四项，右侧显示当前值）：

  | 行 | 右侧状态 | 点击 |
  |---|---|---|
  | 设置日期 | `2026-11-14` | 弹出「年 / 月 / 日」三滚轮弹窗，保存后写 RTC |
  | 时间设置 | `12:41:05`（每秒刷新） | 弹出「时 / 分 / 秒」三滚轮弹窗，保存后写 RTC |
  | WiFi对时 | — | 全屏进度界面：读 wifi.txt → 连 WiFi → NTP → 写 RTC |
  | 时区设置 | `UTC+8` | 弹出单滚轮时区列表，保存后平移 RTC 并持久化 |

- **电源选项**：本轮**完全空白页**（按需求"先空着，别的会话里继续写"），
  只建 `lv_menu_page_create("电源选项")`，不放任何行/说明。

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
main/ui/time_date_popup.h/.c            # 日期/时间/时区三个滚轮弹窗（lv_layer_top）
main/ui/time_sync_ui.h/.c               # WiFi对时全屏进度 + 内部 RAM 栈任务
main/ui/settings_menu.c                 # 根页面两个入口行 + 时间与日期子页
main/core/storage/config_store.*        # tzoff 键
main/core/utils/power_service.*         # power_service_set_hold_awake()
```

UI 约定：

- 三个弹窗与进度界面都挂 `lv_layer_top()`，覆盖设置菜单（屏幕层）与状态栏；
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
- 电源选项页本轮为空，后续会话按 PMU（AXP2101）能力补低电策略/充电限制/
  关机行为等。
