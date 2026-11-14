# PMU / RTC 驱动接入（AXP2101 + PCF85063A）

> 2026-11，master（ESP32-S31 / FakePodNano Pro V1）。
> 内容：把两颗 I²C 芯片接进固件（驱动 + 自检 + 电池百分比），并把**上键（PMU 电源键）
> 与下键（GPIO61）整合成 S3 时代的双键映射**；`fake_shutdown` 名字保留，但已改为
> **AXP2101 软关机真断电**（显示「正在关机...」→保存→写 `0x10[0]=1`）。
> ⚠ 真机断电效果由用户实测；本文 §5 的自检记录是**加入断电流程之前**跑的只读/无损测试。
> 设计依据：[`reference_docs/pmu_axp2101.md`](reference_docs/pmu_axp2101.md)、
> [`reference_docs/rtc_pcf85063a.md`](reference_docs/rtc_pcf85063a.md)
> （原厂 PDF 在 `tempdir/newhwadapter/`，未入库）。

---

## 1. 文件与分层

| 文件 | 职责 |
| --- | --- |
| `main/core/hardware/axp2101.h/.c` | AXP2101 纯寄存器驱动：电压/电量计/充放电状态/开机原因/长按阈值/中断、电源键 IRQ、软关机 API（**未被调用**） |
| `main/core/hardware/rtc.h/.c` | PCF85063A 驱动：一次 burst 读写时间、BCD/星期/有效性、OS 标志、Offset、`struct tm` 互转、首次校时 |
| `main/core/hardware/battery.h/.c` | 应用层封装：电压/电量融合、充电状态；保留 `battery_init()` / `battery_read_voltage_mv()` 旧符号 |
| `main/core/hardware/hw_selftest.h/.c` | 驱动自检（纯逻辑 + 真机只读/无损），`CONFIG_HW_SELFTEST` 控制编译 |
| `main/ui/battery_indicator.h/.c` | 状态栏 / 控制中心共用的“图标 + 百分比”文本生成（≤20% 黄、≤10% 红、充电闪电） |
| `main/ui/app.c` | 顶部状态栏右侧新增电池百分比 label（5s 刷新） |
| `main/ui/notification_panel.c` | 下拉控制中心顶部右侧新增电池百分比（同规则，5s 刷新） |

**已删除**：`main/core/utils/battery_logger.h/.c`（S3 时代没有 PMU，靠 ADC 采样 + 写 SD
卡反推放电曲线；S31 有 AXP2101，电量是随时可读的寄存器，该模块没有存在意义）。
对应 NVS 键 `battlog` / `battmin` 与 config_store API 一并移除（旧键留在 NVS 里无害，不再读取）。

**分层原则**：`axp2101.c` / `rtc.c` 不依赖 LVGL、不建业务任务（PMU 按键事件任务除外）、
不写电源轨；`battery.c` 是唯一对外应用层；UI 只调 `battery_indicator_text()`。

---

## 2. AXP2101 驱动要点

### 2.1 初始化（`axp2101_init(touch_get_i2c_bus())`）

1. 复用触摸驱动建立的 **I²C0 总线**（`touch_get_i2c_bus()`），地址 `0x34`，`scl_speed_hz=400k`
   （IDF 6.1 硬校验，必须显式给）。
2. `i2c_master_probe()` 确认在位；失败只告警（`ESP_ERR_NOT_FOUND`），不 abort。
3. 读 `0x00/0x01`、`0x20`（开机来源）、`0x21`（上次关机原因）、`0x27`（长按阈值）打日志。
4. **清一次中断状态**（`0x48~0x4A` 写 1 清），否则开机前积压的事件会让 GPIO49 一直为低。
5. 可选下发充电参数（`CONFIG_PMU_CHARGE_CONFIG`，默认 **n**）：ICC 300mA、ITERM 50mA、CV 4.2V；
   只写 `0x62/0x63/0x64`，不碰 DCDC/LDO。
6. 可选电源键 IRQ（`CONFIG_PMU_KEY_IRQ`，默认 y）：见 2.2。

> ⚠ **绝对不写** `0x80~0x86` / `0x90~0x9A`（板厂 EFUSE 电源轨默认值）、`0x12`（BATFET）、
> `0x25`（PWROK 时序）、`0x19`（看门狗）。驱动里没有任何通用写寄存器 API，避免误碰。

### 2.2 电源键 IRQ

- GPIO49：输入 + 上拉（板载 4.7k 外部上拉），**下降沿**触发。
- ISR 只 `xQueueSendFromISR`，**绝不做 I²C**；事件任务（PSRAM 栈 3KB、prio 4、Core0）负责：
  `vTaskDelay(5ms)` → 一次 burst 读 `0x48~0x4A` → 写 1 清 → 解析 `0x49[3:0]`。
- **只使能电源键四类中断**：`0x40=0x00`、`0x41=0x0F`、`0x42=0x00`。
  `0x41[3:2]`=短按/长按、`0x41[1:0]`=按下/抬起边沿；后者用于维护
  `axp2101_is_power_key_pressed()`，供 `button_service` 做**双键同时按下**判定。
  若沿用默认全开的中断源，电量计 SOC 更新 / 插拔等事件会持续拉低 IRQ 线 → 中断风暴。
- 业务语义（短按/长按/双键）**统一由 `button_service` 依据"按住状态"生成**；
  PMU 任务只维护状态、清中断，避免两套阈值/重复事件。
- 抗风暴：清完状态后若 IRQ 线仍为低，连续读清 5 次；仍低则**关闭按键中断**并打 `ESP_LOGE`。

### 2.3 能读到的信息（实测值见 §5）

- 电池电压 `0x34/0x35`（**先高后低**，1mV/LSB，14bit）。
- VSYS `0x3A/0x3B`、VBUS `0x38/0x39`、TS `0x36/0x37`、结温 `0x3C/0x3D`。
- 电量计 `0xA4`（0~100；越界由驱动拒绝）。
- 状态 `0x00`（VBUS/BATFET/电池在位）+ `0x01`（电流方向 + 充电阶段）。
- **没有电池电流读数**（芯片无电流 ADC / 库仑计），只能给方向；不要写“读 mA”的假 API。

### 2.4 软关机（已接入 `fake_shutdown`，真机待实测）

```c
esp_err_t axp2101_power_off(void);   /* 写 0x10[0]=1 */
```

`fake_shutdown`（名字保留，实际真关机）的流程：

1. 显示「正在关机...」（黑底居中一行白字，LVGL 全屏覆盖层，`lv_refr_now` 同步刷出）；
2. `player_controller_save_last_position()` + `config_store_flush_pending(true)`；
3. 停止播放并等解码任务退出；
4. `lvgl_port_suspend()`（停止刷屏/触摸/封面任务）；
5. `sd_card_deinit()`（FAT 落盘、释放 SDMMC）；
6. `axp2101_power_off()` → PMU 关掉除 RTCLDO 外所有电源 → 真断电；
7. 2.5s 仍存活则显示「关机失败，请长按电源键 4 秒」，由 PMU 的 OFFLEVEL 硬件兜底。

⚠ 顺序不能反：写 `0x10` 前必须完成所有 flash 写；也不能提前 `touch_power_off()`/删 I²C
总线（PMU 还在 I²C0 上）。⚠ 不再显示 S3 时代的「您现在可以安全断开电源了」黄色面板。

---

## 3. 电池百分比（`battery.c`）

E-Gauge `0xA4` 没有为本板 600mAh 电芯标定过，陌生电池上可能严重偏离（充满瞬间掉 20% 等）。
`battery_fuse_percent()` 按参考文档 §6.3 做**判据层融合**：

| 场景 | 规则 |
| --- | --- |
| 充电中，阶段 DONE | 100% |
| 充电中，未 DONE | 以 gauge 为主，上限锁 **99%**（避免 CV 阶段误显示满电） |
| 充电中且 gauge 不可用 | 电压兜底，同样锁 99% |
| 放电/待机，gauge 与电压估算偏差 ≤8% | `0.6×gauge + 0.4×电压`（四舍五入） |
| 偏差 >8%，或 gauge=0 而电压 >3.6V / gauge=100 而电压 <4.0V | 视为电量计不可信，**只用 OCV 电压表** |
| gauge 越界（0xFF） | 只用电压表 |

电压表用 `axp2101_soc_from_mv()`（600mAh 电芯 OCV 表线性插值）。
**平滑/限幅（每 60s ±2%）留给 UI 层**——当前电量变化慢、5s 刷新一次，暂未加。

UI：状态栏右侧与下拉控制中心顶部右侧显示 `图标 + 百分比`，充电时图标换闪电；
≤20% 黄、≤10% 红；PMU 不可用显示灰色 `--%`。只在 5s LVGL timer 里读一次 I²C（约 1ms）。

---

## 4. PCF85063A 驱动要点

### 4.1 初始化（`rtc_init()`）

1. 复用 I²C0 总线，地址 `0x51`，probe；失败只告警，不 abort。
2. **关闭 CLKOUT**：`Control_2` 写 `0x07`（COF=111；复位默认是 32.768kHz 推挽输出，
   本板 CLKOUT 悬空，既费电又有 EMI）。
3. 检查 `Control_1`：`12_24=1` 只告警（不擅自改写小时含义）；`STOP=1` 则清 STOP 恢复走时。
4. **不做软件复位**（写 `0x00=0x58` 会把备用电源保住的走时清成 2000-01-01）。
5. **首次校时**：`rtc_is_valid()` 为 false（OS=1 / 出厂默认 / 字段非法）时，
   写入**项目诞生日** `2026-08-30 13:45:41`（git 仓库首个提交 `06d8b19` 的时间）：
   - `CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH` 默认 **y**；
   - 有效时间**绝不覆盖**；
   - ⚠ 这是项目诞生日，**不是固件编译时间**（`__DATE__/__TIME__` 会随每次构建变化）。

### 4.2 读写时间

- 读：**一次 burst 读 7 字节**（`04h~0Ah`）。读写时间寄存器期间芯片冻结计数，积压的进位
  最多服务 1 次，整个访问必须 <1s；分多次读会出现“分/秒来自不同时刻”。
- 写：`STOP=1` → 一次事务写 7 字节 → `STOP=0`；`Seconds.bit7=0` 顺带清 OS。
  失败也要把 STOP 放掉，否则 RTC 永远停走。
- 星期：0=周日…6=周六（与 `tm_wday` 一致），寄存器是 3bit 二进制、不是 BCD；
  调用方没填（>6）时按日期用 Sakamoto 公式算。
- 无世纪位：年份固定按 `2000 + 00~99` 解释；闰年规则 `year % 4 == 0`（2000~2099 内成立）。

### 4.3 实测澄清：OS 位不能软件置 1

数据手册 **p.20 §7.3.1.1 / Figure 13** 明确：OS 由硬件在振荡停止/上电未稳定时置 1，
软件只能**写 0 清除**；“若清不掉说明振荡器没起振”。

所以：
- 自检**不能**用“写 OS=1”模拟未初始化（实测无效）；
- 改用“写入 2000-01-01（年份 < 2024）”触发 `rtc_is_valid=false`，
  验证首次校时路径（`CONFIG_RTC_SELFTEST_AUTO_INIT`，默认 **n**，破坏性）。

---

## 5. 真机自检结果（2026-11-14，COM3，debug）

固件：`idfc build` + `idfc flash`，`idfc monitor --wait-for "HWST RESULT"`。

```
I hwst: ===== HWST summary: 26 passed, 0 failed, 0 skipped =====
I hwst: HWST RESULT: PASS
```

关键读数：

| 项目 | 实测 |
| --- | --- |
| 电池电压 | **4190 mV**（8 次采样峰峰值 0 mV；插 USB、电池在位） |
| VSYS / VBUS | 4955 mV / ~4950 mV（USB 供电） |
| 电量计 `0xA4` | **100%** → 展示 100% |
| 充放电状态 | `0x01=0x14`：方向 standby、阶段 **done**（已充满，插着 USB） |
| TS 引脚 `0x36/0x37` | **600 mV → 板上有 NTC**（参考文档 §12 的待确认项结案） |
| 长按阈值 `0x27=0x12` | ONLEVEL=**1s**、OFFLEVEL=**4s**、IRQLEVEL=**1.5s**（EFUSE 默认，非文档猜测的 512ms/6s） |
| 开机来源 `0x20=0x04` | VBUS 插入开机（插 USB） |
| 上次关机 `0x21=0x01` | PWRON 长按 |
| 充电配置（只读） | ICC code `0x09`=300mA、ITERM code `0x1`=25mA、CV code `3`=4.2V |
| RTC | `2026-08-30 13:47:34 wd=0`（首次校时写入项目诞生日后继续走时） |
| RTC CLKOUT | `Control_2=0x07`（COF=111，已关） |
| RTC 走时 | 1.1s 后秒增量 1~2 → **ticking** |
| RTC Offset | MODE=0、OFFSET=0（未校准；本板无 CLKOUT 引出，无法测频） |

首次校时路径（临时打开 `CONFIG_RTC_SELFTEST_AUTO_INIT=y` 验证）：

```
I rtc: time set: 2000-01-01 00:00:00 wd=6          ← 自检把时间写成“未初始化”
W rtc: RTC uninitialized (OS=0): set to project epoch 2026-08-30 13:45:41
I hwst: auto-init result: 2026-08-30 13:45:41
I hwst: [PASS] rtc_auto_init_project_epoch
```

验证完已把该选项改回 **n**：第二次启动 RTC 时间为 `2026-08-30 13:47:34`（继续走时），
日志**没有**再出现 `RTC uninitialized` 警告 → 有效时间不被覆盖。

---

## 6. Kconfig

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CONFIG_HW_SELFTEST` | y（release 关） | 驱动自检任务（PSRAM 栈），串口打印 PASS/FAIL/SKIP + `HWST summary` |
| `CONFIG_PMU_KEY_IRQ` | y | GPIO49 电源键短按/长按/按下/抬起（供双键判定） |
| `CONFIG_PMU_CHARGE_CONFIG` | n | 启动时下发 600mAh 充电参数（ICC/ITERM/CV） |
| `CONFIG_RTC_AUTO_INIT_PROJECT_EPOCH` | y | RTC 无效时用项目诞生日首次校时 |
| `CONFIG_RTC_SELFTEST_AUTO_INIT` | n | **破坏性**自检：验证首次校时路径（会把 RTC 改成项目诞生日） |

---

## 7. 待办 / 已知限制

- [ ] **软关机真机实测**：`fake_shutdown` 已改为写 `0x10[0]=1` 的真关机，等待实测
      （断电效果、开机恢复、SD/NVS 数据完整性）。
- [ ] 低电策略（拒启/自动关机）尚未接回 `power_service` / `boot_flow`；电压/电量接口已就绪。
- [x] PMU 电源键已合并进 `key_handler`：`button_service` 统一生成上/下键短按、长按、
      双键同时按下，`key_handler` 恢复 S3 映射（见 §8）。
- [ ] RTC 时间只在日志里，状态栏尚未显示时间；没有手动校时 UI（当前只能靠首次校时/重新烧录）。
- [ ] 电量融合没有做“每 60s ±2%”平滑；若实测跳变明显再加到 UI 层。
- [x] 设置菜单 →「电源选项」已实现（典型容量/电量/充电中/电压/温度 + 电池健康保护），
      并在开机时按主控复位原因 + PMU `0x20` 判断插电开机、进入充电专用界面；
      设计/验证见 [`POWER_OPTIONS_AND_CHARGE_BOOT.md`](POWER_OPTIONS_AND_CHARGE_BOOT.md)。
- [x] 充电专用界面的真机拔插/按键流程已由用户实测通过（插电开机进界面、10s 息屏、
      按键亮屏、短按上键重启进系统、拔电显示关机提示后断电），见
      [`POWER_OPTIONS_AND_CHARGE_BOOT.md`](POWER_OPTIONS_AND_CHARGE_BOOT.md) §7。
- [x] 电池健康保护：**4.0V 软件限充**（CV 保持 4.2V，`0x18[1]=0` 停充、3.9V 恢复）。
      ⚠ 一开始试过把 CV 降到 4.0V，真机发现电池高于 CV 时 PMU 会断开 BATFET，
      正常使用拔充电器会 DCDC 欠压硬断电（`0x21=0x20`），已改掉，见
      [`POWER_OPTIONS_AND_CHARGE_BOOT.md`](POWER_OPTIONS_AND_CHARGE_BOOT.md) §3。
- [ ] `CONFIG_PMU_CHARGE_CONFIG` 默认关：首次上板先只读确认，确认电池规格后再打开。
- [ ] RTC 是否有备用电源（纽扣电池/超级电容）本板未确认；若没有，每次冷启动 OS=1 →
      会重新写成项目诞生日（符合“未初始化时初始化”的预期）。

---

## 8. 按键整合（上键=PMU PWRON，下键=GPIO61）

`button_service` 把两个来源统一成 S3 时代的五类事件（上/下键短按、长按、双键同时按下）：

| 状态 | 上键短按 | 上键长按 | 下键短按 | 下键长按 | 双键同时按下 |
| --- | --- | --- | --- | --- | --- |
| 亮屏 | 音量 +1 | **关机** | 音量 -1 | 音量 -10（暂定） | 息屏 |
| 息屏 | 播放/暂停 | **关机**（先亮屏显示提示） | 下一曲 | 上一曲 | 亮屏 |

- 上键长按阈值：亮屏 1200ms / 息屏 900ms（与 S3 一致），由 `button_service` 在按下沿取值；
  继续按住到 PMU `OFFLEVEL`（实测 4s）由 PMU 硬件强制断电兜底。
- 下键长按阈值：600ms。
- 双键判定：两键按下时间存在重叠 → 全部抬起后统一上报一次 `BOTH`，并抑制本次单键事件。
- 上键"按住状态"由 `0x49` 的按下/抬起边沿维护（`axp2101_is_power_key_pressed()`），
  轮询周期 5ms；人手按键时长足够判定。

> **真机实测（2026-11-14）**：上/下键短按、长按、双键切换的按键行为已由用户确认
> **符合预期**。长按关机的实际断电/开机恢复效果如未单独验证，可按以下步骤确认：
> 亮屏长按上键 1.5s 左右 → 屏幕出现黑底「正在关机...」→ 设备断电；
> 重新按上键开机后应恢复上次歌曲/音量/配置。若 2.5s 后未断电，屏幕会提示
> 「关机失败，请长按电源键 4 秒」。
