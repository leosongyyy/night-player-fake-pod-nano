# 双固件方案调研：主播放器 + 最小对时固件（切换启动）

> 2026-11-15。动机见 `WIFI_TIME_SYNC.md` §9：WiFi 对时常驻约 27KB 内部 RAM
> （外加 ~590KB Flash），是蓝牙之后 UI 帧率被挤压的主要来源。
> 设想：开两个 app 分区，一个主播放器（带蓝牙），一个只做 WiFi 对时的最小固件，
> 靠改"下次启动哪个分区"来回切；PCF85063A 是外置 RTC，天然跨固件共享。
>
> **结论：可行，且不需要自定义 bootloader。** IDF 自带 bootloader 的 OTA 槽 +
> `otadata` 就够用；推荐布局 `factory` = 播放器、`ota_0` = 对时固件。
> 本文记录源码依据、布局、切换流程、构建/烧录方式、风险与工作量。
> ⚠ **尚未实机验证**，所有数字是链接期实测或按现有镜像推算。

---

## 1. 为什么不需要自定义 bootloader

依据 IDF 6.1 源码（`components/bootloader_support/src/bootloader_utility.c`、
`components/app_update/esp_ota_ops.c`）：

| 行为 | 依据 |
|---|---|
| otadata 有效 → 启动它指向的 OTA 槽 | `bootloader_utility_get_selected_boot_partition()` |
| otadata 空/无效 → 有 factory 就启动 factory，没有就 **"No factory image, trying OTA 0"** | 同上 |
| 选中的镜像校验失败 → 先往回退到 factory，再往后试所有 OTA 槽，全不行才复位 | `bootloader_utility_load_boot_image()` 的两段循环 |
| 切启动分区前会**校验目标镜像**（`ESP_IMAGE_VERIFY`），不通过返回 `ESP_ERR_OTA_VALIDATE_FAILED` | `esp_ota_set_boot_partition()` |
| 目标是 factory 时，`esp_ota_set_boot_partition()` = **擦掉 otadata**（等价"下次回 factory"） | `esp_ota_set_boot_partition_internal()` |
| otadata 分区按 `type=data, subtype=ota` 查找，**位置任意**（不必固定在 0xE000） | `read_otadata()` 里的 `esp_partition_find_first()` |

也就是说：**"切过去 / 切回来 / 坏镜像自动回退"这三件事，标准 bootloader 全都自带**。
只有在这些需求下才值得自己写 bootloader：开机按键进对时模式、按电量选固件、
彻底不写 otadata（怕磨损/怕断电）等。本方案都不需要。

---

## 2. 推荐分区布局

原则不变（见 `partitions.csv` 顶部注释）：**pldata 留在最末尾、font/pldata 按 64KB 对齐、
nvs/phy_init 不动**——这样播放列表、字体镜像、用户配置全都不失效。

```
nvs       data nvs      0x9000   0x6000     （不动）
phy_init  data phy      0xF000   0x1000     （不动）
factory   app  factory  0x10000  0x380000   # 3.5MB  主播放器（起始与今天相同！）
ota_0     app  ota_0    0x390000 0x200000   # 2MB    对时固件
otadata   data ota      0x590000 0x2000     # 8KB    启动选择
font      data 0x41     0x600000 6M         （不动）
pldata    data 0x40     0xC00000 4M         （不动）
```

- app 分区必须 64KB 对齐（`gen_esp32part.py` 会检查），所以 **otadata 放在两个 app 之后**
  而不是紧跟 `phy_init`：0x10000 处放 otadata 会逼着 factory 挪到 0x20000，白丢 64KB。
- 0x590000..0x600000 还剩 0x6E000（约 440KB）空着，留给将来调整。
- 三个分区的边界都落在 0x10000 的整数倍上（0x390000、0x590000）。

**为什么选 factory=播放器、ota_0=对时**（而不是两个 OTA 槽）：

1. 播放器保持 0x10000 起始、保持 `factory` 语义 → 日常 `idfc flash` 的烧录目标不变；
2. factory 是 bootloader 的**最终兜底**：对时固件再怎么坏，往回退一定落到播放器；
3. 从对时固件切回来 = 擦 otadata（一个动作、无序号逻辑）；
4. 代价：只多一个 OTA 槽 → 对时固件必须手工烧到 0x390000（见 §5）。

尺寸核算：当前播放器镜像 **2,454,848 B（2.34MB）**，槽 3.5MB，余 1.16MB；
对时固件按"WiFi 栈 ~650KB + FATFS/stdio ~430KB + LVGL ~270KB + 应用代码 ~150KB"
估算 **1.3–1.6MB**，槽 2MB（若砍掉 LVGL 改手绘进度条，可压到 ~1.2MB）。

---

## 3. 切换流程（关键是"先回退，再干活"）

播放器侧（设置 → 时间与日期 → WiFi对时）：

```c
const esp_partition_t *sync = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
esp_err_t err = esp_ota_set_boot_partition(sync);   /* 内部会校验镜像 */
if (err == ESP_OK) {
    esp_restart();                                  /* 约 1s 后进入对时固件 */
} else {
    ui_toast_show("对时固件未烧录");                 /* ESP_ERR_OTA_VALIDATE_FAILED */
}
```

对时固件侧（`app_main` 第一件事）：

```c
const esp_partition_t *player = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
esp_ota_set_boot_partition(player);   /* = 擦 otadata：此后任何 reset 都回播放器 */
/* 再跑现有的 core/network/wifi_time_sync + ui/time_sync_ui，完事 esp_restart() */
```

**这一步是全方案的关键**：先把启动目标改回播放器，再去做耗时、可能失败、可能被
看门狗打断的联网流程。于是——

- 对时中途断电 / 崩溃 / 看门狗复位 → 下次启动直接是播放器（只是没对上时）；
- 对时固件自己起不来（镜像坏了）→ bootloader 校验失败自动回退 factory；
- 不存在"卡在对时固件里反复重启"的状态。

对时失败（没有 wifi.txt、连不上 AP、NTP 超时）不该闷着：跑完/超时（建议 60–90s）
→ 显示结果 → `esp_restart()` 回播放器。

---

## 4. 共享与隔离

| 资源 | 处理 |
|---|---|
| RTC（PCF85063A @ I2C0） | 外置芯片，天然共享。对时固件写完直接重启，播放器读到的就是新时间。**这就是本方案成立的前提。** |
| NVS | 两固件共用同一分区。对时固件**只读**（读 `tz` 时区偏移，把 UTC 换成墙钟）；不写配置槽位/音量/EQ，避免两套代码的结构漂移。 |
| font / pldata | 共用只读。对时固件不碰 pldata，字体走现有 `font_npf` mmap（0x600000 不变）。 |
| 分区表 | 两个 build **必须用同一份 `partitions.csv`**；bootloader 只认 flash 里那张表，固件不能有第二套假设。 |
| SPI flash 驱动 fork | `components/spi_flash` 的 S31 分支预测补丁两个固件都要（都由依赖图带入）。 |

将来若开 Flash 加密 / 安全启动：两张镜像都要签名，OTAdata 逻辑不变，但
`esp_ota_set_boot_partition` 的镜像校验会连签名一起验。

---

## 5. 构建与烧录（这是最容易踩坑的部分）

### 5.1 同一工程、按 Kconfig 分角色

- `main/Kconfig.projbuild` 增加 `CONFIG_FIRMWARE_ROLE_SYNC`（bool，默认 n："构建对时专用固件"）；
- 新增 `sdkconfig.defaults.sync`：打开 ROLE_SYNC，并关掉 `CONFIG_BT_ENABLED`、
  LVGL 的大件（若对时固件不用 LVGL 则整个组件都别引用）、音频/DAC 等；
- `main.c` 顶部按角色分派：
  ```c
  #if CONFIG_FIRMWARE_ROLE_SYNC
      sync_app_main();
  #else
      /* 现有播放器流程 */
  #endif
  ```
- `main/CMakeLists.txt` 里**可以**按 Kconfig 条件追加源文件（组件 CMake 在 Kconfig 之后执行）：
  ```cmake
  if(CONFIG_FIRMWARE_ROLE_SYNC)
      list(APPEND MAIN_SRCS "sync_app.c" "core/network/wifi_time_sync.c" "ui/time_sync_ui.c")
  endif()
  ```
- ⚠ **顶层 `COMPONENTS` 白名单不能按角色裁剪**（它写在 `include(project.cmake)` 之前，
  那时 Kconfig 还没加载）。所以 `esp_wifi / esp_netif / esp_event / lwip /
  wpa_supplicant / app_update` 必须**无条件**列进白名单。播放器 build 里这些组件
  没人引用 → 归档成员不会被链接，**镜像与 RAM 不受影响**，只多花几分钟编译时间。
  （代价可以接受；真嫌慢再考虑给 idfc 加"按 variant 传 CMake 变量"的能力。）

### 5.2 单独构建目录 + 手工烧录

```powershell
# 对时固件：单独 build 目录，显式给 sdkconfig defaults
idfc idfpy -B build\sync -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sync" `
           -D SDKCONFIG="build\sync\sdkconfig" build

# 烧到 ota_0（⚠ 不能用 idfc flash！它按当前 build 的分区表把 app 写到 factory）
esptool.exe --chip esp32s31 --port COMx write_flash 0x390000 build\sync\FakePodNanoNightPlayer.bin
```

- **必须**先 `idfc flash` 一次播放器，把新的分区表写进去（分区表变了要重刷）；
- 烧对时固件前先看 `esptool` 报的镜像大小 ≤ 0x200000，否则会越过 `otadata` 踩到 font；
- `tools/make_full_firmware.py` 需要扩成"多镜像"：bootloader + 分区表 + factory +
  ota_0 + font；对时固件没烧时播放器点"WiFi对时"只会提示"未烧录"，不会变砖。

---

## 6. 成本、风险与验证

| 项 | 评估 |
|---|---|
| 切换耗时 | 两次重启（播放器→对时 ~1s，对时→播放器 ~1.5s）+ 联网 3–10s；只在手动对时时发生 |
| Flash 磨损 | 每次切换 1 次 otadata 擦写（8KB 分区两扇区）；一次对时 = 2 次写，可忽略 |
| 维护成本 | 两套构建/烧录；驱动或 UI 改动要同时考虑对时固件（它复用同一批源文件，成本主要在"记得编它"） |
| 镜像管理 | 对时固件版本与播放器解耦；建议在关于页/串口打印 `esp_ota_get_running_partition()->label` |
| 主要风险 | ① 对时固件被误烧到 0x10000（覆盖播放器）——靠"只用手工 write_flash"这条纪律规避；② 两固件 NVS 结构漂移——对时固件只读；③ 播放器涨过 3.5MB 要重排分区（pldata/font 不动就行） |

验证清单（落地时逐条过）：
1. 来回切换 10 次，每次都能回到正确固件；
2. 在"已切到对时、还没切回"的瞬间拔电 → 下次启动是播放器；
3. 对时固件分区为空/镜像损坏时点"WiFi对时" → 提示未烧录，播放器不受影响；
4. 对时成功后播放器状态栏时间正确（RTC 共享生效）；
5. 对时前后播放列表、字体、配置槽位无变化；
6. 对时固件里的 `esp_ota_get_running_partition()` 显示 `ota_0`，播放器显示 `factory`。

---

## 7. 与另外三条路的对比

| 方案 | 播放器常驻 RAM | 帧率 | 对时时蓝牙 | 复杂度 |
|---|---|---|---|---|
| 单固件 + WiFi 常驻（本次下线前的状态） | +27KB 静态，运行时峰值另算 | 被挤压（V_RES/16） | 不停 | 低 |
| 单固件 + 对时前临时熄蓝牙 | +27KB 静态（省不掉） | 同上 | **断耳机、停音乐** | 中 |
| **双固件（本文）** | **0** | **可回 V_RES/10** | **不涉及**（对时固件里没有蓝牙） | 中高 |
| 外部对时（电脑工具写 SD / USB 串口） | 0 | 可回 V_RES/10 | 不停 | 低（但要连着电脑） |

结论：**如果 WiFi 对时必须保留、又要满帧 + 蓝牙不断，双固件是唯一同时满足的方案**；
代价是两套构建/烧录与两次重启。若对时能接受"插电脑"，外部对时方案成本最低。

---

## 8. 建议的落地顺序（估 1–2 天）

1. 分区表改造 + 播放器加 `app_update` 与"切换"入口（对时固件还不存在时，点击应提示"未烧录"）；
2. `sdkconfig.defaults.sync` + `sync_app.c`（最小版：显示进度 → 现有 `wifi_time_sync_run` → 写 RTC → 重启）；
3. 烧录脚本 + `make_full_firmware.py` 扩展；
4. 按 §6 的清单验证。

## 9. 未定项

- 对时固件实际镜像大小（需要真构建一次；若 >2MB 就把 ota_0 调到 2.5MB、factory 调回 3MB）；
- 对时固件是否值得保留 LVGL（保留 = 直接复用 `time_sync_ui`，但 +270KB Flash；砍掉要自绘进度条）；
- 播放器侧入口是否要二次确认（"将重启进入对时模式"），以及切换前是否要先把播放位置落盘
  （`player_controller_sync_config()` / 关机前那套保存流程要复用，否则断在切换前会丢现场）；
- 是否需要"对时完成后自动回播放器并 Toast 结果"（跨固件传状态：可写在 NVS 的一个 key 里，
  播放器启动时读一次并清除）。
