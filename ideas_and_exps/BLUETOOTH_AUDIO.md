# 经典蓝牙音频（A2DP Source / SBC + AVRCP）实现记录

> 2026-11，S31（FakePodNano Pro）固件新增功能。本文记录**设计取舍、内存审计、
> 接线方式、验证状态与已知限制**，方便后续维护/回归。
> 代码位置：`main/core/hardware/bt/`、`main/core/hardware/audio/{bt_audio,resampler}.c`、
> `main/core/storage/bt_store.c`、`main/core/utils/quiet_mode.c`、
> `main/ui/{bt_settings,bt_quick_panel}.c`。

## 1. 需求与实现映射

| 需求 | 实现 |
|---|---|
| 经典蓝牙音频，SBC | A2DP Source + Bluedroid **内部 SBC 编码器**（`BT_A2DP_USE_EXTERNAL_CODEC=n`），应用只喂 PCM |
| 输出重采样到蓝牙格式（esp-dsp） | `resampler.c`：多相窗函数 sinc（Blackman），每个输出样本用 `dsps_dotprod_f32()`（esp-dsp 1.4.12，S31 上为 ANSI C） |
| **EQ 放在重采样之后** | `bt_audio.c`：解码 int16 → 重采样到蓝牙 fs → EQ（系数按蓝牙 fs）→ 回放增益/软件音量 → int16 ringbuf → 协议栈 SBC |
| 最多 5 个设备，设备音量/EQ 独立、优先于配置文件 | `bt_store`（NVS namespace `btdev`，`d0..d4` 五个 key）；连接时 `audio_bt_profile_enter()` 应用设备音量/EQ，断开时 `audio_bt_profile_leave()` 恢复配置槽位 |
| 绝对音量支持/不支持 | 支持：AVRCP CT `SetAbsoluteVolume`（0~127），本机软件增益 1.0；不支持：本机软件衰减（**线性 1/30..1.0**）。耳机侧调音量经 `ESP_AVRC_RN_VOLUME_CHANGE` 同步回档位并写回设备 |
| 睡眠定时到点不断电、只保留蓝牙（安静模式） | `quiet_mode.c`：停播/关屏/挂 DAC/卸 SD，保留 A2DP 链路；12h / 电量 <15% / 蓝牙断开 → PMU 断电；长按电源键重启 |
| 安静模式开关绑定到具体设备、默认关 | `bt_device_t.quiet_mode`，在设备详情页切换；`bt_service_connected_quiet_mode()` 判定 |
| 控制中心睡眠定时开关改为蓝牙开关，长按设备列表 | `notification_panel.c`：`LV_SYMBOL_BLUETOOTH` 圆钮；点击开/关，长按打开**快速面板**（设备列表 + 设置入口） |
| 设置页单独蓝牙设置（配对/删除） | 设置菜单根页新增「蓝牙」入口 → `ui/bt_settings.c`（lv_menu，与设置菜单同规格） |
| 播放线程声明性能需求；开蓝牙熄屏提高 CPU 频率 | `power_service_perf_set(POWER_PERF_BT_AUDIO, …)`：熄屏 80MHz → **160MHz**（A2DP 流开始/结束时切换） |

## 2. 架构与线程

```
MP3 解码（mp3_play，Core1）
  └─ int16 PCM（源采样率 fs_src）
      └─ audio_pcm_write()
          ├─ 输出 = DAC：EQ(fs_src) + 回放增益 → int32 → I2S/CS43131（原路径）
          └─ 输出 = BT：bt_audio_write()
                └─ 重采样 fs_src→fs_out → EQ(fs_out) → 增益/软件音量
                   → int16 ringbuf（PSRAM，96KB ≈ 0.5s）
                      └─ Bluedroid A2DP source 任务拉 PCM（bt_audio_data_cb）
                         → 内部 SBC 编码 → L2CAP → 耳机
```

- **bt_svc 任务**（`bt_service.c`，内部 RAM 栈 6KB，Core1，prio 5）：所有协议栈操作
  （启动/关闭、连接/断开、扫描、配对、起流/挂流）串行执行；Bluedroid 回调只做轻量
  状态更新 + 投递命令，不在回调里做重活。
- BT 控制器/host 任务通过 `CONFIG_BT_CTRL_PINNED_TO_CORE_CHOICE_1` /
  `CONFIG_BT_BLUEDROID_PINNED_TO_CORE_1` 固定 Core1（与解码同核，避开 LVGL 的 Core0）。
- 音频输出路由（DAC/BT）在 `audio.c`：`s_bt_active` 决定 `audio_pcm_write` 走哪条链；
  BT 连接成功即切到蓝牙（DAC 静音/停 I2S），断开自动回 DAC 并继续播放。
- **EQ 系数只维护一套**：DAC 路径按源 fs，蓝牙路径按协商出的蓝牙 fs；切换输出时
  `eq_notify_sample_rate()` 重算（满足"EQ 在重采样之后"）。

## 3. 蓝牙参数与协商

- 本机名：`NightPlayer FPNPro`。
- Classic only：`BTDM_CTRL_MODE_BR_EDR_ONLY`，BLE 关闭（省 RAM/Flash）。
- SBC 偏好：**48kHz 优先，其次 44.1kHz**（用户在确认时指定），joint stereo / block 16 /
  8 subbands / loudness / bitpool ≤53。耳机两种都不支持时置
  `bt_service_codec_unsupported()`，UI 提示"不支持 SBC 48k/44.1k"。
- 连接流程：扫描（inquiry，约 12.8s）→ 点击结果 → `esp_a2d_source_connect()` →
  SSP Just Works（legacy PIN 回 0000）→ CONNECTED → 读 codec caps → `set_pref_mcc()`
  → 播放时 `CHECK_SRC_RDY`/`START`；暂停时 `SUSPEND`。
- ⚠ **实际采样率以 `ESP_A2D_AUDIO_CFG_EVT` 为准，不要用偏好值**：真机（ROSE EarFeel i7）
  caps 里有 48k、我们也用 `set_pref_mcc()` 请求了 48k，但协议栈最终上报
  `SBC sf=0x2 -> 44100 Hz` 并以 44.1k 编码。首版按 48k 喂 PCM，结果播放速度比正常快
  约 8.8%、音调偏高；现在 `s_out_fs` 只由 AUDIO_CFG 事件写入，偏好只表达意图。
  对多数 44.1k MP3 反而免重采样。
- 开机行为：**记忆蓝牙开关状态，不主动回连**（`bt_store` 的 `en`）。
- **耳机主动回连**：蓝牙开着且**未连接**时空闲态固定为 `ESP_BT_CONNECTABLE + ESP_BT_NON_DISCOVERABLE`
  （page scan 开、inquiry scan 关）——已配对耳机开机后会按自己记的地址直接 page 我们，
  连接由 Bluedroid 的 `BTA_AV_PENDING_EVT` 路径自动接受并走到 `ESP_A2D_CONNECTION_STATE_EVT`；
  只有点"配对新设备"扫描期间才临时加 inquiry scan（`GENERAL_DISCOVERABLE`），扫描结束恢复
  可连接不可发现。主机侧仍然不会主动 `esp_a2d_source_connect()`，所以"开机不自动抢占音频"的
  行为不变；但耳机自己开机/回连是支持的（真机已验证）。
  **已连接期间 page scan 关闭**（`ESP_BT_NON_CONNECTABLE`）：单链路设计下，若保持可连接，
  第二副已配对耳机会不断 page，Bluedroid 在"已有 AV SCB"时会报
  `av scb not available` / `BTA_AV_OPEN_EVT::FAILED` / `Out of ccbs`，既刷屏又可能产生
  过期的 AVRCP 状态；断开时 `bt_link_down()` 会恢复可连接。
- **多设备状态过滤**：`a2d_cb` / `avrc_ct_cb` 的连接状态事件都按 `remote_bda` 校验，
  只处理当前 `s_peer_bda`；"从未连上"的 `DISCONNECTED` 直接忽略，避免第二副耳机
  连接失败时把正在播放的耳机一起 `link_down`。
- 设备上限 5：配对第 6 个时 UI 提示先删除；设备删除会同时移除系统 bond。多副耳机
  同时尝试回连时只接受第一副，其余在 controller/AVDTP 层被拒绝，属预期单链路行为。
- **设备切换**：在快速面板/设备详情里点另一台设备时，`bt_request_connect_bda()` 会
  **先断开当前设备**，等 `link_down` 收尾（约 200ms）后再连目标；目标连接失败就保持
  断开，**不会回退旧设备**。切换请求是异步的，5s 内旧设备没有真正断开就自动取消
  （避免"很久以后关掉 A 却突然连 B"）。旧设备的 ACL 允许保留（控制器 BR/EDR 支持
  2 条 ACL），不影响新设备连接。

### 重采样系数生成（"首连等 5 秒"坑）

`resampler_init()` 里的 `build_coeffs()` 要生成 `L×taps` 个系数（48k→44.1k 是
147×32 = 4704 个）。**首版用 `double` + 每系数 `sin()/cos()`，在 S31 上双精度是
软件模拟，实测建一次系数约 5 秒**：表现是首次连接后要等约 5 秒才出声（后续连接
如果源是 44.1k→44.1k 走 bypass 就很快），同时 bt_svc 占满 Core1 触发
`task_wdt: IDLE1 (CPU 1)` 看门狗告警（不是重启，但会打印 register dump）。

现改为 **float + 每相位只算一次 `sinf/cosf` 起点，内层按固定角步长做旋转递推**
（sinc 步长 `π·fc`、Blackman 步长 `2π/(taps-1)`），内层只剩乘加和一次除法。
真机实测建系数 **约 80ms（debug）**，连接→出声约 170ms，看门狗不再触发。
系数已与旧 double 版本逐点对比：最大绝对误差 **3e-6**（float32 精度内），
DC 增益仍为 1。⚠ 旋转公式注意方向：`cos(φ-δ)=cosφcosδ+sinφsinδ`、
`sin(φ-δ)=sinφcosδ-cosφsinδ`，写反会让窗函数反向而听不出明显异常、但频响变差。


### 音量映射（蓝牙与 DAC 解耦）

- **DAC（CS43131）**：保持原有 35%~85% 的**特调曲线**，不动（`audio_volume_level_to_percent` 只服务 DAC）。
- **蓝牙 + 绝对音量**：**线性满范围** `1..30 → 1..127`（0 = 0/静音），不再复用 DAC 曲线；
  反向 `127 → 30` 同步耳机侧调音量。真机反馈（ROSE EarFeel i7）证明原来的 35% 起点
  （1 档 = 44/127）对高灵敏度耳机偏响。
- **蓝牙 + 无绝对音量（软件音量）**：**线性** `1..30 → 1/30..1.0`（0 = 静音），与 DAC 曲线无关。

## 4. 内存审计（关键）

BT 控制器 + Bluedroid 常驻会吃掉约 **13KB 内部 DMA 池**。首版直接导致
`sdmmc_cmd: allocate_dma_buf: not enough mem`，所有歌被判"坏曲"。为此做了以下调整
（都在真机验证过）：

| 调整 | 效果 |
|---|---|
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`（blend 等移出 IRAM，经 `SPIRAM_FETCH_INSTRUCTIONS` 在 PSRAM 执行） | 释放约 **27KB** 内部 RAM；实测帧率仍 61fps（S31 320MHz） |
| LVGL 绘制缓冲 `V_RES/10` → **`V_RES/16`**（双缓冲 83KB → 51KB DMA） | 释放约 **33KB DMA**；帧率约 25fps 量级 |
| mp3 `out_buf`/`stereo_buf`（9.2KB）→ PSRAM | 内部 RAM +9.2KB |
| `audio.c` 的 `silence_flush`（11.5KB）→ PSRAM | 内部 RAM +11.5KB |
| `bt_store` 由 1.3KB 整块 blob 改为每设备 key（`d0..d4`） | 去掉 1.3KB 落盘快照 |
| `eq_popup` 文件名表（1.5KB）→ PSRAM | 内部 RAM +1.5KB |
| BT 扫描表（1.3KB）、命令队列（~0.7KB）→ PSRAM | 内部 RAM +2KB |
| `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y` | Bluedroid host 动态分配优先 PSRAM |

实测（BT 已启动、未连接耳机，开机完成）：

- 内部 RAM 余量约 **49KB**（最大连续块 31KB）；
- DMA 余量约 **17KB**（最大连续块 15KB）；
- SD 读、播放、封面解码、UI 均正常。

> ⚠ 如果改回 `V_RES/10` 或重新打开 LVGL FAST_MEM，BT 控制器初始化会再次因内部
> 连续池不足失败（`orca_ems_env_init malloc pool failed`，随后在 BT 库内 panic）。

## 5. esp-dsp 的来源与裁剪

- 机器可能离线，仓库**直接 vendored** `components/esp-dsp/`（1.4.12，Apache-2.0，
  只保留 `CMakeLists/Kconfig/LICENSE/README/CHANGELOG/modules`）。
- S31 是 **RISC-V** 目标，上游把 Xtensa `.S` 无条件列进 SRCS；本项目只用
  `dsps_dotprod_f32`，因此在 `components/esp-dsp/CMakeLists.txt` 里用
  `list(FILTER srcs INCLUDE REGEX "modules/dotprod/float/dsps_dotprod_f32_ansi\\.c$")`
  只编译 ANSI 版点积（也避开 kalman 等模块在新 C++ 工具链下的编译错误）。
- 升级 esp-dsp 时请保留这段过滤。

## 6. 安静模式（蓝牙伪关机）

进入条件：睡眠定时到点 + 当前连接设备开启「安静模式」（默认关）。

进入动作：停播 → 挂起 A2DP 流（**不断链路**）→ `audio_shutdown()`（DAC pop-free）
→ 息屏/挂起 LVGL → 卸载 SD → 固定 80MHz → 启监控任务（PSRAM 栈，5s 周期）。

退出（任一 → `axp2101_power_off()` 真断电，无 UI）：

- 蓝牙链路断开（≤5s 检测）；
- 持续 > 12 小时；
- 电量 < 15% 且未充电。

用户主动退出：**长按电源键** → `esp_restart()`（短按无反应）。安静模式期间
`key_handler` 只响应上键长按，其它按键/触摸全部忽略（LVGL 已挂起）。

## 7. UI 入口

- **控制中心**：左列上圆钮 = 蓝牙开关（关=深灰、开未连=蓝、已连接=黄）；
  点击开/关，**长按**弹出「蓝牙快速面板」（`ui/bt_quick_panel.c`，380×432 居中卡片 +
  半透明遮罩，点遮罩关闭）：
  - 只有已保存设备列表：点击整行 = 连接/断开（已连另一台时提示先断开）；
  - 底部只有「蓝牙设置」一个入口，进入完整设置页。
- **设置菜单 → 蓝牙**：与设置菜单同规格的 `lv_menu` 页面（`ui/bt_settings.c`）：
  - 主页面：开关、已保存设备（**点击整行进入设备详情**，不再直接切连接状态）、配对新设备；
  - 设备详情：状态/MAC、音量、EQ、安静模式、连接/断开、删除（两步确认）；
  - 扫描页：搜索结果、重新搜索、配对提示。
  - **切换**：在快速面板或详情页点另一台设备 = 断开当前 → 连目标；失败保持断开
    （不回退），逻辑在 `bt_service.c` 的 `bt_request_connect_bda()`。
- 设置页九宫格「EQ」格在蓝牙连接期间自动切到**设备 EQ**（显示/编辑都是设备的）；
  断开后回到配置槽位 EQ。

## 8. 调试开关

- `bt_service.c` 的 `BT_DEBUG_AUTOSTART_TEST`（默认 0）：置 1 时强制开机启动蓝牙并在
  栈就绪后自动扫描，用于无 UI 情况下验证 BT 栈/内存。
- `resampler_selftest()`（`resampler.c`，默认不调用）：44100→48000 注入 1kHz 正弦，
  校验输出帧数/RMS/频率。实测 `out=22292/20480`、`rms=0.353`（期望 0.354）、
  `freq≈1014Hz`（过零法测量精度内）。
- 串口关键日志：`BTDM controller init OK`、`bt_service: stack up`、
  `pref SBC: ...`、`A2DP connected`、`media start ack`、`SetAbsoluteVolume response`、
  `resampler: init ... us`（末项是建系数耗时，正常应在几十毫秒）、`quiet mode: ...`。

## 9. 验证状态 / 已知限制

已验证（真机，ROSE EarFeel i7）：

- BT 控制器/Bluedroid 启动、设备名、扫描（inquiry）结束；
- 内存/DMA 余量与 SD 读（BT 启动下播放正常）；
- A2DP 连接/起流/暂停恢复、SBC 实际 44.1k、音质/速度/无爆音（长时间 `stack pull`
  176.3 kB/s、miss 0.000%）；
- AVRCP 绝对音量（`SetAbsoluteVolume` 1..127 线性映射）与耳机侧调音量同步；
- **首次连接延迟**：修复重采样建系数 5 秒问题后约 170ms 出声；
- **耳机主动回连**（page scan 常开，关闭再开机自动连回）；
- 控制中心长按快速面板（连接/断开 + 设置入口）、完整设置页设备详情；
- 设置页设备 EQ/安静模式/删除的 UI 路径。

**未验证 / 待回归**：

- 安静模式端到端（12h / 电量 15% / 蓝牙断开断电、长按电源键重启）；
- 不支持绝对音量的耳机（软件线性音量 1/30..1.0）；
- 多设备切换（连接 A 时点 B 的提示、删除当前连接设备）；
- 配置槽位切换与蓝牙连接并存时的音量/EQ 交接。

**已知兼容性问题：ZYFDroid_EarFun（apt-X 优先）**

- 现象：host 主动连接（点击设备）时 ACL 能建起来，但控制器报
  `OLM_LMP: acl lmp unpack failed, err:262 / opcode:54`
  （LMP_page_scan_mode_req），随后 `BTA_AV_OPEN_EVT::FAILED status: 3`
  （BTA_AV_FAIL_STREAM），耳机“连接成功”提示音后立刻“断开”提示音。
  耳机自己 page 过来（重新打开耳机仓/回连）则一切正常。
- 已排除：手机同时连接（多设备抢占）、删除后重新配对、host 连接期间关闭
  page scan、重启蓝牙协议栈。ROSE/Ctyx/Sleep A30 的 host 主动连接均正常。
- 线索：该耳机优先 apt-X；S31 用的是 Orca BR/EDR 控制器，失败点在
  ACL/LMP 阶段（早于 A2DP codec 协商），应用层无法改 LMP 行为。
- 当前只能让耳机主动回连（开仓/关机再开）；如需点击连接，需要耳机侧
  关闭 apt-X 优先/升级固件，或由 Espressif 修 Orca 控制器对该 LMP PDU 的解析。

建议回归顺序：配对 → 连接 → 播放（确认 `A2DP audio cfg` 的实际采样率）→
音量键/控制中心调音量（有/无绝对音量各一副）→ 暂停/恢复 → 断开自动回 DAC →
设备 EQ 切换 → 关闭耳机再开机（主动回连）→ 安静模式（可临时把 12h 改小验证）。

## 10. 相关文件

- `main/core/hardware/bt/bt_service.c/.h`：协议栈、GAP/A2DP/AVRCP、状态机。
- `main/core/hardware/audio/bt_audio.c/.h`：BT 输出链路、ringbuf、绝对音量/流回调。
- `main/core/hardware/audio/resampler.c/.h`：多相重采样 + 自测。
- `main/core/storage/bt_store.c/.h`：5 设备配置（NVS `btdev`）。
- `main/core/utils/quiet_mode.c/.h`：安静模式。
- `main/ui/bt_settings.c/.h`：完整蓝牙设置页（主视图/设备详情/扫描）。
- `main/ui/bt_quick_panel.c/.h`：控制中心长按的快速连接面板。
- 修改：`audio.c/.h`（输出路由/设备音量 EQ/绝对音量）、`power_service.c/.h`
  （性能需求/安静模式）、`sleep_timer.c`、`key_handler.c`、`notification_panel.c`、
  `app.c`、`settings_page.c`、`settings_menu.c`、`eq_popup.c`、`toast.c`、
  `config_slots.c`、`main.c`、`sdkconfig.defaults`、根 `CMakeLists.txt`。
