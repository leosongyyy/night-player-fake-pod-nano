# 双固件方案：主播放器 + WiFi对时固件（2026-11-15 已落地并真机验证）

> ⚠ **本文是历史记录（旧版「WiFi 对时固件」方案）**：2026-11 该固件已升级为
> **「里世界」固件**（构建变体仍是 `sync`，界面 = WiFi对时 + **USB传歌** + 回到表世界）。
> 现行方案、代码结构、烧录命令与实测数据请看 **`INNER_WORLD.md`**。
> 本文保留的价值：记录"为什么用 twin-app / otadata 而不是自定义 bootloader"、
> 旧对时固件的往返实测日志与当时的坑；其中"对时固件跑完自动重启""结果经 NVS
> 交给播放器 Toast"等行为**已经废弃**，不要照做。

> 动机：WiFi 对时那套协议栈（esp_wifi/lwip/wpa_supplicant）常驻要多吃约
> **27KB 内部 RAM（静态）+ 65KB（运行时峰值）**，而内部 RAM 与 IRAM 是同一块
> SRAM —— 蓝牙接入之后它就成了 UI 帧率的主要挤压来源（见 `WIFI_TIME_SYNC.md` §9）。
> 但播放器其实只在对时那一瞬间需要网络。
>
> 做法：flash 里放**两个 app**。日常跑 `factory` 里的播放器（不含 WiFi 协议栈），
> 需要网络对时时切到 `ota_0` 里的**最小对时固件**（含 WiFi，不编播放器那一套），
> 对时固件写完 PCF85063A 就切回播放器。**PCF85063A 是外置芯片，天然跨固件共享**
> —— 这是整个方案成立的前提。
>
> 结论：**不需要自定义 bootloader**，IDF 标准 bootloader 的 OTA 槽 + `otadata`
> 就够；2026-11-15 已实现并在真机跑通"播放器 → 对时 → 播放器"往返（含连续多轮），
> 播放器固件的内部 RAM 回到未接 WiFi 时的水平（`iram=109,987`，fps 61~63）。

---

## 1. 为什么不需要自定义 bootloader

依据 IDF 6.1 源码（`components/bootloader_support/src/bootloader_utility.c`、
`components/app_update/esp_ota_ops.c`）：

| 行为 | 依据 |
|---|---|
| otadata 有效 → 启动它指向的 OTA 槽 | `bootloader_utility_get_selected_boot_partition()` |
| otadata 空/无效 → 有 factory 就启动 factory，没有就 "No factory image, trying OTA 0" | 同上 |
| 切启动分区前会**校验目标镜像**（`ESP_IMAGE_VERIFY`），不通过返回 `ESP_ERR_OTA_VALIDATE_FAILED` | `esp_ota_set_boot_partition()` |
| 目标是 factory 时，`esp_ota_set_boot_partition()` = **擦掉 otadata**（等价"下次回 factory"） | `esp_ota_set_boot_partition_internal()` |
| otadata 按 `type=data, subtype=ota` 查找，**位置任意**（不必固定在 0xE000） | `read_otadata()` 里的 `esp_partition_find_first()` |

⚠ **一个必须知道的例外**：本机 `sdkconfig.defaults` 开了
`CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON/ALWAYS=y`（换开机速度，不校验镜像）。
所以"镜像坏了 bootloader 自动回退 factory"这条保险**在本机不生效**，安全完全靠
**"对时固件开机第一件事就擦 otadata"** 这条纪律 + `esp_ota_set_boot_partition()`
写之前自己会校验目标镜像。**不要绕过 `fw_switch_*` 直接写 otadata。**

---

## 2. 最终分区布局

原则不变（见 `partitions.csv` 顶部注释）：**pldata 留在最末尾、font/pldata 按 64KB
对齐、nvs/phy_init/font/pldata 的偏移一律不动**——所以这次改动**不影响**播放列表、
字体镜像与用户配置（不需要重刷字体、不需要重扫列表）。

```
nvs       data nvs      0x9000   0x6000     （不动）
phy_init  data phy      0xF000   0x1000     （不动）
factory   app  factory  0x10000  0x300000   # 3MB    主播放器（起始与以前相同）
ota_0     app  ota_0    0x310000 0x280000   # 2.5MB  WiFi对时固件
otadata   data ota      0x590000 0x2000     # 8KB    启动选择
font      data 0x41     0x600000 6M         （不动，4.44MiB 的 NPF1 字库）
pldata    data 0x40     0xC00000 4M         （不动，锚定最尾部）
```

- app 分区必须 64KB 对齐，所以 otadata 放在两个 app **之后**：0x10000 处放 otadata
  会逼着 factory 挪到 0x20000，白丢 64KB。
- 0x592000~0x600000 还剩 0x6E000（约 440KB）没用，留给将来（factory 涨过 3MB 时把
  这块划给它，并等量缩小 ota_0）。
- **为什么 factory = 播放器**：① 播放器保持 0x10000 起始、保持 `factory` 语义 →
  日常 `idfc flash` 的落点不变；② factory 是 bootloader 的最终兜底；③ 从对时固件
  切回来 = 擦 otadata（一个动作，无序号逻辑）；④ 代价只是对时固件必须手工烧到
  0x310000。

### 2.1 构建时的体积检查（看到 Warning 别慌）

IDF 的 `check_sizes.py` 会把**当前构建的 app 镜像**与分区表里**所有** app 分区比较：
全部装不下才报错（Exit 1），只有一部分装不下只打 Warning。于是：

| 构建 | 现象 | 说明 |
|---|---|---|
| 播放器 debug（2.36MB） | 正常 | 两个槽都装得下 |
| 播放器 release（2.66MB） | `Warning: 1/2 app partitions are too small … Part 'ota_0' (overflow 0x29400)` | **预期**：播放器只烧 factory（3MB），永远不进 ota_0 |
| 对时固件（1.59MB） | 正常 | 必须 ≤ 2.5MB（超了会溢出到 otadata） |

---

## 3. 切换流程（关键：先回退，再干活）

播放器侧（设置 → 时间与日期 → WiFi对时，`ui/settings_menu.c`）：

```c
/* ① 先弹确认（"蓝牙与播放会中断"）② 保存播放现场 ③ 切启动分区 ④ 重启 */
player_controller_save_last_position();   /* 歌曲/位置/配置 → NVS */
config_store_flush_pending(true);
esp_err_t err = fw_switch_to_sync();      /* 内部会校验 ota_0 镜像 */
if (err == ESP_OK) { esp_restart(); }
else { ui_toast_show("对时固件未烧录"); }   /* ESP_ERR_OTA_VALIDATE_FAILED */
```

对时固件侧（`core/system/sync_app.c`，`app_main` 第一件事）：

```c
fw_switch_to_player();   /* = 擦 otadata：此后任何复位/断电都回播放器 */
/* 之后才做耗时的初始化 + 联网 + 写 RTC */
```

于是——对时中途断电 / 崩溃 / 看门狗复位 / 用户直接按电源键关机，下次启动都是
播放器（只是没对上时）；不存在"卡在对时固件里反复重启"的状态。

对时结束（成功 2.5s / 失败 8s / 用户点「关闭」）→ 把结果写进 NVS（`tsync/res`）→
`esp_restart()` 回播放器；播放器开机完成时读出来 Toast 一下
（`main.c` 的 `report_sync_result()`），然后清掉。

⚠ **为什么结果传"文本"而不是错误码**：播放器固件里不能引用 `wifi_time_sync.c`
的任何符号——哪怕只引用一个字符串函数，那个 .o 也会被拉进链接（`--gc-sections`
再把其余函数丢掉，行为依赖链接器细节）。索性让对时固件写好"成品文案"
（"对时成功 12:34"），播放器只做"读出来弹 Toast"，两边零符号耦合。

---

## 4. 代码结构（本次新增/改动）

| 文件 | 作用 |
|---|---|
| `partitions.csv` | factory 5.9375M→3M，新增 `ota_0`(2.5M) 与 `otadata`(8K)；font/pldata/nvs 不动 |
| `main/Kconfig.projbuild` | `FIRMWARE_ROLE_SYNC`（构建对时固件）、`FIRMWARE_SYNC_AUTOSTART`（调试：开机自动切过去，默认 n） |
| `sdkconfig.defaults.sync` | 新构建变体的差异配置：ROLE_SYNC=y、BT 关、性能监视/内存快照/极速封面关、主任务栈 16KB |
| `idfc_buildconfig.json` | 新增构建变体 `sync`（`idfc build -t sync`，输出 `build/sync`） |
| 根 `CMakeLists.txt` | `esp_wifi/esp_netif/esp_event/lwip/wpa_supplicant` **重新进白名单**，并新增 `app_update`（见 §4.1）；bt 组件的编译选项加了 `if(CONFIG_BT_ENABLED)` 保护 |
| `main/CMakeLists.txt` | 源码拆成 `MAIN_SRCS_SHARED`（两固件共用）/ `MAIN_SRCS_PLAYER` / `MAIN_SRCS_SYNC`，按角色拼装；播放器列表与拆分前逐项一致 |
| `main/main.c` | `app_main()` 只做角色分派：对时固件 → `sync_app_run()`，播放器 → 原流程 `player_app_main()`；新增 `report_sync_result()` 与调试用 autostart 钩子 |
| `main/core/system/fw_switch.{h,c}` | **新增**：角色判定、运行分区标签、`fw_switch_to_sync()/to_player()`、跨固件结果（NVS `tsync/res`） |
| `main/core/system/sync_app.{h,c}` | **新增**：对时固件的最小启动流程（先回退 → 最小初始化 → 点亮屏幕 → 起对时任务 → 结果 → 重启） |
| `main/ui/time_sync_ui.{h,c}` | 新增 `time_sync_ui_set_done_cb()`（结果出来后的去向：重启回播放器）、`time_sync_ui_start()` 改为返回 bool、去掉对 `power_service` 的硬依赖（对时固件不跑电源服务） |
| `main/ui/settings_menu.c` | "WiFi对时"入口从"弹暂未实现"改为**确认弹窗 → 切固件 → 重启** |

### 4.1 白名单里那 5 个 WiFi 组件为什么不占播放器的内存

`COMPONENTS` 白名单写在 `include(project.cmake)` **之前**，那时 Kconfig 还没加载，
拿不到 `CONFIG_FIRMWARE_ROLE_SYNC`，所以只能**无条件**列着。这不影响播放器：
"被编进构建"与"被链接进镜像"是两件事，静态库成员只有被引用时才拉进镜像。

实测证据（`build/debug/FakePodNanoNightPlayer.map`）：

- `LOAD esp-idf/esp_wifi/libesp_wifi.a` 等只是**链接输入**；
- 来自 `esp_wifi/lwip/wpa_supplicant` 的 `.obj`：**0 个**；
- 镜像 2,454,848 B（下线时）→ 2,471,888 B（本次，+17KB 全是 app_update +
  fw_switch + 确认弹窗），静态内部 RAM 无变化（`iram=109,987` vs 下线时 110,479）。

代价只有"播放器构建时多编译这 5 个组件"（全量冷编译多几分钟，增量构建无感）。

---

## 5. 构建与烧录

```powershell
# ① 播放器（日常固件）—— 与以前完全一样；它会顺手把新分区表写进去
idfc build ; idfc flash

# ② 对时固件（只在改了对时相关代码时才需要重烧）
idfc build -t sync        # 输出 build\sync\FakePodNanoNightPlayer.bin（必须 ≤2.5MB）

# ③ 把对时固件烧到 ota_0（⚠ 不能用 idfc flash -t sync！）
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\esptool.exe' --chip esp32s31 -p COM3 `
    --before default-reset --after hard-reset write-flash 0x310000 `
    build\sync\FakePodNanoNightPlayer.bin
```

要点：

- 🚨 **绝不能用 `idfc flash -t sync`**：那条命令按当前构建的分区表把 app 写到
  **factory**（0x10000），会把播放器覆盖成对时固件。
- `idfc flash` 会额外写 `ota_data_initial.bin`（**空白 otadata**）到 0x590000，
  所以"重烧播放器"总是安全地落回 factory。
- `idfc flash` 只写 0x10000 起的 app 区段（播放器最坏 2.79MB，结束于 0x2B9400），
  **不会碰 0x310000 的对时固件**——重烧播放器不需要重烧对时固件。
- 镜像顺序：bootloader(0x0) + 分区表(0x8000) + factory(0x10000) + ota_0(0x310000)
  + otadata(0x590000) + font(0x600000，手动) + pldata(0xC00000，运行时生成)。
  换机器/全新板子时**桌面顺序**：`idfc flash` → 刷对时固件 → 刷字体。

### 5.1 无人值守验证（没有触摸输入时）

`CONFIG_FIRMWARE_SYNC_AUTOSTART=y`（默认 n，**只用于调试**）：播放器每次开机完成
都会切到对时固件，对时完自动回 factory，于是形成"播放器 → 对时 → 播放器"循环。
本次验证就是这么做的（临时把它加进 `sdkconfig.defaults.debug` 构建，验证完删除）。

---

## 6. 共享与隔离

| 资源 | 处理 |
|---|---|
| RTC（PCF85063A @ I2C0） | 外置芯片，天然共享。对时固件写完直接重启，播放器读到的就是新时间 |
| NVS | 两固件共用同一分区。对时固件**只读**配置（时区 `tz`），只写一个结果键 `tsync/res`；不碰音量/EQ/配置槽位，避免两套代码的结构漂移 |
| font / pldata | 共用只读。对时固件读 font 分区（NPF 字体），**完全不碰 pldata** |
| 分区表 | 两个 build 用**同一份 `partitions.csv`**；bootloader 只认 flash 里那张表 |
| SPI flash 驱动 fork | `components/spi_flash` 的 S31 分支预测补丁两个固件都要（都由依赖图带入） |
| 蓝牙 / 音频 / 播放列表 | 只编进播放器；对时固件里 `CONFIG_BT_ENABLED=n`，bt 组件退化成 INTERFACE 库 |

---

## 7. 真机实测（2026-11-15，debug 构建）

镜像：播放器 **2,471,888 B**（factory 3MB，余 674KB）；对时固件 **1,666,912 B**
（ota_0 2.5MB，余 36%）。

**播放器（新分区表 + 无 WiFi 协议栈）**：开机 2.17s 到 `App started`，
`iram=109,987`、`fps=61~63`（空闲 4%~5% CPU0）——与"WiFi 对时下线"时
（110,479 / 61~62）持平，说明 WiFi 组件确实一个字节都没进播放器镜像。

**往返一轮（串口实测，时间戳为开机后毫秒）**：

```
[播放器] 1989  result from sync firmware: 对时成功 00:44     ← 上一轮的结果 Toast
[播放器] 4134  fw_switch: switch next boot -> ota_0 @0x310000 (size 0x280000)
[播放器] 4391  fw_switch: otadata updated: next boot = ota_0
[重启]   408   sync_app: === 对时固件启动：role=sync, running partition=ota_0 ===
[对时]   421   fw_switch: switch next boot -> factory @0x010000 (擦 otadata)
[对时]   633   fw_switch: otadata erased: next boot = factory
[对时]   670   sync_app: init: display + lvgl port
[对时]   1122  sync_app: init: PMU / RTC / SD
[对时]   1310  wifi_time_sync: === wifi time sync start ===
[对时]   1345  sync_app: turn on display (brightness 128)
[对时]   1526  sync_app: touch ready（可点「取消」/「关闭」）
[对时]   4357  wifi_time_sync: wifi connected on attempt 1
[对时]   4475  wifi_time_sync: ntp synced on attempt 1: 1794674689 (UTC)
[对时]   4512  wifi_time_sync: === done in 3194 ms: 2026-11-15 00:44:49 (UTC+8:00) ===
[对时]   7036  fw_switch: sync result stored: 对时成功 00:44
[对时]   7037  sync_app: 对时结束（result=0），重启回播放器（factory）
[播放器] 420   app_main: running partition: factory (role=player)
[播放器] 1995  app_main: result from sync firmware: 对时成功 00:44
[播放器] 2078  toast (3000 ms): 对时成功 00:44
```

即：**一次对时 ≈ 7 秒（对时固件侧）+ 两次重启各 ~1 秒**，其中联网取时只要 3.2s。
连续跑了 4 轮以上，每一轮都回到 `factory`，RTC/配置/播放列表/字体全部正常。

### 7.1 串口里两个"看着吓人其实正常"的现象

1. `WARNING: Checksum mismatch between flashed and built applications. Checksum of
   built application is 047e…` —— 来自 **idf_monitor**（`esp_idf_monitor/base/
   serial_handler.py`），它拿串口打印的 `ELF file SHA256` 去比**当前构建目录**里的
   ELF。启动的是 ota_0 里的对时固件时当然对不上（那份 ELF 在 `build/sync/`）。
   **不是固件问题**，`idfc monitor -t sync` 或忽略即可。
2. 每次重启边界出现几行 `--- 0x…: func at file:line` —— 是对时/重启路径上的栈
   回溯信息（`esp_restart_noos_inner` 等），没有 `Guru Meditation`/`abort` 就是正常。

---

## 8. 出问题怎么退

| 症状 | 处理 |
|---|---|
| 点「WiFi对时」提示"对时固件未烧录" | ota_0 没烧或镜像坏了：按 §5 的 ③ 重烧；播放器不受影响 |
| 对时固件卡住/死机 | 长按电源键关机再开 → 因为 otadata 已被擦，一定回播放器 |
| 想临时关闭这个功能 | 不烧 ota_0 即可（点入口只会提示未烧录）；要彻底删代码见下面的"移除" |
| 误开了 `FIRMWARE_SYNC_AUTOSTART` 导致循环 | 正常 `idfc flash` 一次（会写空白 otadata + 关掉该选项的固件）即可 |
| 想清空对时固件槽 | `esptool erase-region 0x310000 0x280000` |

**完全移除本方案**（回到单固件）：把 `partitions.csv` 的 factory 改回 0x5F0000、
删掉 ota_0/otadata 两行；`sdkconfig.defaults.sync`、`main/core/system/` 可留可删；
`main/Kconfig.projbuild` 的两个选项删掉；`main/CMakeLists.txt` 合回一个列表
（`SHARED + PLAYER`）；`main.c` 去掉 `#if` 分派；`settings_menu.c` 的
`time_date_sync_row_cb` 改回 `time_sync_ui_start()`。⚠ 改分区表必须重刷分区表。

---

## 9. 已知取舍与后续可做

- **对时固件仍有 LVGL**（约 270KB Flash）：换来直接复用现成的进度界面/字体/触摸。
  若要再瘦，可以砍成手绘进度条（省 270KB，但会丢"取消/失败原因"这些文字反馈）。
- **对时固件重复了一套最小初始化**（display/lvgl/font/touch/PMU/RTC/SD）：故意
  不复用 `main.c` 的播放器流程，避免"改播放器顺手弄坏对时固件"。
- **时区只在播放器侧配置**（config_store 的 `tz`），对时固件只读它把 UTC 换算成
  墙钟时间写 RTC —— 与旧版单固件行为一致。
- 若将来要"对时完成后播放器自动弹窗显示新旧时间差"，可在 `tsync/res` 旁边多存
  一个"对时前 RTC 秒数"，播放器侧算差值（现在只存结果文本）。
