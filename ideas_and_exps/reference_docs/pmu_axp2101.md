# AXP2101 电源管理（PMU）驱动参考

> **本文依据**：`tempdir/newhwadapter/AXP2101_datasheet.pdf` —— X-Powers *AXP2101 Single Cell NVDC PMU with E-gauge*，Rev 1.4（2022-10-13），共 63 页。文中 `[p.N]` 指该 PDF 页码。
> **适用硬件**：FakePodNano Pro V1（ESP32-S31），AXP2101 挂 **I²C0（7-bit 地址 `0x34`）**，PMU 中断接 **GPIO49**，上方电源键接 **PWRON**（无主控 GPIO）。引脚/总线定义见 [hardware_v1.md](hardware_v1.md)。
> **本文覆盖**：① 读电池电压 / ② 读电量百分比 / ③ 读充放电状态与（缺失的）电流 / ④ 读电源键事件、配置长按开关机阈值 / ⑤ 软关机与开机来源 / ⑥ 充电参数（按 600 mAh、3.7 V 标称、4.2 V 满电整定）/ ⑦ 本项目落地设计。
> **状态**：**AXP2101 已接入**——`main/core/hardware/axp2101.c` + `battery.c` 已实现电压 / E-Gauge 电量 / 充电状态 / 电源键 IRQ / 软关机 / 开机原因 / 电池健康保护（4.0V 软件限充），实现与真机实测见 `ideas_and_exps/PMU_RTC_DRIVERS.md` 与 `POWER_OPTIONS_AND_CHARGE_BOOT.md`。本文其余内容保留为**适配前的设计依据**（S3 时代末期所写），寄存器/协议部分仍然有效。

---

## 0. 速查（TL;DR）

| 需求 | 怎么做 | 寄存器 |
| --- | --- | --- |
| 电池电压 | 读 14-bit ADC，**先读高字节再读低字节** | `0x34[5:0]`(高 6 位) + `0x35`(低 8 位)，1 mV/LSB |
| 电量百分比 | 直接读 E-Gauge 输出的 SOC | `0xA4`（0–100，只读）；使能位 `0x18[3]`（默认 **1** 已开） |
| 是否在充电 / 充电阶段 | 状态寄存器 | `0x01[2:0]`；电流方向 `0x01[6:5]` |
| **瞬时放电电流** | ⚠ **芯片没有这个寄存器**（无电流 ADC 通道） | 只能拿方向/状态，见 §7 的三条替代方案 |
| 电源键短按/长按事件 | 使能 IRQ + 读状态位 + 写 1 清除 | 使能 `0x41[3:2]`，状态 `0x49[3:2]`，总中断 `0x49`/`0x4A` |
| 长按开机阈值 | ONLEVEL 128 ms / 512 ms / 1 s / 2 s | `0x27[1:0]`（默认来自 EFUSE） |
| 长按关机阈值 | OFFLEVEL 4 / 6 / 8 / 10 s | `0x27[3:2]`（默认 **6 s**） |
| 短按/长按 IRQ 分界 | IRQLEVEL 1 / 1.5 / 2 / 2.5 s | `0x27[5:4]`（默认 **1.5 s**） |
| 软件关机（真断电） | 写 `0x10[0]=1` → PMU 关掉除 RTCLDO 外所有电源轨 | `0x10[0]`（RWAC） |
| 本次开机的原因 | 读 PWRON status | `0x20`（PWRON/VBUS/电池插入…） |
| 充电电流 | 600 mAh → 0.5C = 300 mA（出厂默认正好 300 mA） | `0x62[4:0]` |
| 充满电压 | 4.2 V → 编码 `011b`（出厂默认） | `0x64[2:0]` |
| 充电截止电流 | 600 mAh 建议 25–50 mA（默认 125 mA 偏大） | `0x63[3:0]` |
| **限制最大充电电压**（不满充） | **有**：CV 五档 4.0 / 4.1 / 4.2 / 4.35 / 4.4 V | `0x64[2:0]`，见 §10.5 |
| **"充到 80% 就停"** | **芯片没有这个功能**：只能降 CV，或软件写充电使能位停充 | `0x18[1]` + §10.5 |

**三条最容易踩的坑**（详见 §15）：① 只装电池时 **BATFET 默认是关的**，必须按 PWRON 或插 VBUS 才能开机；② ADC 必须**先高后低**读两字节，否则数据不完整；③ 电量计（E-Gauge）没有为本电池做过模型标定，`0xA4` 可能与电压严重不符，**必须做电压兜底**。

---

## 1. 芯片与板级连接

### 1.1 芯片概要 [p.1]

| 项目 | 参数 |
| --- | --- |
| 类型 | 单节锂电 NVDC PMU（充放电管理 + 电源路径 + 电量计） |
| 输入 | VBUS 3.9–5.5 V（绝对最大 12 V）[p.10] |
| 电池 | 单节 Li-ion/Li-poly，VBAT 工作范围 **2.5–4.5 V**，UVLO 2.4 V，耗尽阈值 2.5 V [p.11-12] |
| 输出 | 5 路 DCDC（DCDC1/2 各 2 A，DCDC3 2 A，DCDC4 1.5 A，DCDC5 1 A）+ 11 路 LDO |
| 充电 | 线性充电，100 mA–1 A，CV 精度 ±0.5%，**ICHG 精度 ±20%** |
| 电量计 | E-Gauge 3.0（输出电压/百分比，**无电流读数**） |
| 接口 | TWSI（I²C 兼容），从地址 **0x68/0x69（8-bit）= 0x34（7-bit）** [p.18] |
| 封装 | QFN-40，5×5 mm，外露焊盘接 GND |
| 关断电流 | < 40 µA（BATFET 关、只留 RTCLDO）[p.1][p.19] |

### 1.2 板级连接（FakePodNano Pro V1）

| 信号 | 连接 | 说明 |
| --- | --- | --- |
| SDA / SCL | GPIO48 / GPIO47（I²C0，400 kHz） | 与 CST820(0x15)、PCF85063(0x51)、QMI8658(0x6B) **共用总线** |
| IRQ | **GPIO49**，输入，低有效 | 开漏输出，**需要 4.7 kΩ 上拉到逻辑电源** [p.8]；下降沿触发 |
| PWRON | 上方电源键 → PMU 的 PWRON 脚 | 内部 100 kΩ 上拉到 VINT [p.8]，**没有主控 GPIO** |
| PWROK | 系统复位指示输出 | 板级已接，固件一般不需要动 |
| CHGLED | 充电指示灯（开漏） | 由 PMU 自动驱动，可用 `0x69` 改行为 |
| BAT | 电池（板载 600 mAh / 3.7 V 标称 / 4.2 V 满电） | 芯片内部 BATFET 连到 SYS |
| TS | 电池温度检测 | ⚠ 本板是否装了 NTC **未确认**，见 §12 |
| 电源轨 | DCDC1 默认 3.3 V、ALDO1 默认 1.8 V（EFUSE/板厂配置） | **不要改**，见 §11 |

> ⚠ SDA/SCK 手册要求 2.2 kΩ 上拉（外部已有）；`i2c_new_master_bus()` 里再开内部上拉只是兜底，不冲突。

---

## 2. 通信协议（TWSI / I²C）

### 2.1 帧格式

标准 I²C，7-bit 地址 `0x34`：

```
写单寄存器：  START | 0x34<<1|W | REG | DATA | STOP
读单寄存器：  START | 0x34<<1|W | REG | Sr | 0x34<<1|R | DATA | STOP
连续读 N 字节：START | 0x34<<1|W | REG | Sr | 0x34<<1|R | D0 D1 ... D(N-1) | STOP
```

- 寄存器地址是 **单字节**，读操作时内部地址指针**自增**（可一次突发读多个寄存器）。
- ⚠ **写操作不要依赖地址自增**：AXP 系列在不同批次上对连续写的行为并不完全一致，驱动里**一律"一个寄存器一次传输"**，代价可忽略（400 kHz 下单字节约 50 µs）。
- 速率：板级总线 400 kHz；手册未给 TWSI 时序上限，按 400 kHz 用即可（触摸、PMU、RTC、IMU 全在这条线上）。

### 2.2 ESP-IDF 6.1 下的写法

本项目触摸驱动已经用**新版 I²C master 驱动**建好了 I²C0 总线，并用 `touch_get_i2c_bus()` 暴露句柄（见 `main/core/hardware/touch.c`）。PMU/RTC/IMU 应当 **复用同一个 bus handle**，不要再 `i2c_new_master_bus()`（会返回 `ESP_ERR_INVALID_STATE` 或抢引脚）。

```c
/* 设备句柄：地址绑定在设备上，scl_speed_hz 必须显式给（IDF 6.1 硬校验，0 → INVALID_ARG） */
i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x34,
    .scl_speed_hz    = 400 * 1000,
};
i2c_master_dev_handle_t dev;
ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_get_i2c_bus(), &dev_cfg, &dev));

/* 写一个寄存器 */
uint8_t buf[2] = { reg, val };
i2c_master_transmit(dev, buf, 2, 100);

/* 读 N 个寄存器 */
i2c_master_transmit_receive(dev, &reg, 1, out, n, 100);
```

新版驱动**自带互斥**（同一总线上的并发访问会被串行化），所以 PMU 任务、触摸（`esp_lcd_panel_io_i2c`）、RTC 任务可以各写各的，不需要额外的总线锁。

---

## 3. 上电 / 关机 / 复位行为（必须先搞懂，否则会怀疑人生）

### 3.1 三种"复位" [p.18, p.21-22]

| 名称 | 触发 | 结果 |
| --- | --- | --- |
| POR（上电复位） | VBUS 或 VBAT 首次上升 | **全部寄存器回默认值**，TWSI 可用，电源轨按 EFUSE 默认启动 |
| System reset | 软件写 `0x10[1]=1`、PWROK 拉低、看门狗超时 | 除 RTCLDO/VREF 外的输出全关，寄存器回默认 |
| Power on reset | 从"完全没电"到上电 | 连 RTCLDO 和 VREF 都关 |

**推论**：每次开机（包括按键从关机态开机）**配置都不会保留**，驱动必须在 `pmu_init()` 里重新下发一遍。

### 3.2 电池单独供电时**不会自动开机**（重要）

> "If only battery is present and VBAT is higher than depletion threshold, **BATFET … is off by default and need to be turned on by pressing the PWRON key or inserting an adapter**." [p.18 §6.5.2]

所以：

- 只接电池、按一下电源键 → 按 ONLEVEL 判定后开机；
- 开机后 `0x00[4]`（BATFET state）应显示 `0`（= close，即导通；`1` 是 open/断开）；
- 这解释了为什么"上电即跑"的调试方式在电池供电下不成立，必须按键或用 VBUS。

### 3.3 关机有三条独立路径

| 路径 | 触发条件 | 是否可配置 | 特点 |
| --- | --- | --- | --- |
| **A. 长按 PWRON 到 OFFLEVEL** | 按住 PWRON > OFFLEVEL（默认 6 s） | `0x22[1]` 使能/禁止，`0x27[3:2]` 改时长 | **硬件兜底**，固件死机也能关机；`0x22[0]` 决定这一路触发后是"关机"（0）还是"重启"（1） |
| **B. 软件软关机** | 写 `0x10[0]=1` | 无 | **本项目"关机"应走这条**：除 RTCLDO 外全部断电，静态电流 40 µA 量级 |
| **C. 保护性关机** | VSYS 欠压（VOFF 默认 2.6 V，`0x24` 可调）、DCDC 过/欠压、VBUS 过压、结温 >145 ℃、电池 OVP 4.4 V | `0x22`、`0x23`、`0x24` | 故障保护，读 `0x21` 可知原因 |

另有 `0x10[2]`：**"PWRON 16 s 强制关 PMIC"**（默认关）。打开后即使 A/B/C 全失效，按住 16 s 也能断。

### 3.4 从关机态唤醒 / 开机来源

`0x20`（PWRON status，只读）逐位记录本次开机是谁触发的 [p.35]：

| 位 | 含义 |
| --- | --- |
| 5 | EN 模式下 PWRON 恒高 |
| 4 | 电池插入且电压正常作为开机源 |
| 3 | 充电时电池 >3.3 V 作为开机源 |
| 2 | VBUS 插入且正常作为开机源 |
| 1 | IRQ 脚被拉低作为开机源 |
| 0 | PWRON 按住超过 ONLEVEL 作为开机源 |

`0x21`（PWROFF status）记录**上一次为什么关机**：bit7 结温、bit6 DCDC OV、bit5 DCDC UV、bit4 VBUS OV、bit3 VSYS UV、bit2 EN 模式、**bit1 软件配置（我们的软关机）**、bit0 PWRON 长按。

> 建议：`pmu_init()` 里读一次 `0x20`/`0x21`，存进 NVS/日志，用于区分"用户按键开机"和"插 USB 开机"（后者可直接进充电界面）。

---

## 4. 寄存器总表

> 完整列表 [p.29-31]；下表带 ★ 的是本项目要用的，带 ⛔ 的是**不要动**的。

| 地址 | 名称 | R/W | 本项目用途 |
| --- | --- | --- | --- |
| `0x00` | PMU status1（VBUS 好/BATFET/电池在位/限流） | R | 诊断 |
| `0x01` | PMU status2（电流方向/充电阶段） | R | ★ 充放电状态 |
| `0x04-0x07` | DATA BUFFER | RW | 不用 |
| `0x10` | PMU common config（软关机/重启/16s） | RW | ★ 软关机 `[0]` |
| `0x12` | BATFET control | RW | ⛔ 改了可能电池开不了机 |
| `0x13` | Die temperature control | RW | 不用 |
| `0x14` | 最小系统电压控制（充电时 VSYS DPM） | RW | ⛔ |
| `0x15` | 输入电压限制 VINDPM | RW | ⛔（默认 4.36 V） |
| `0x16` | 输入电流限制 IINLIM | RW | ☆ 接 USB 时按电源能力设 |
| `0x17` | 复位电量计 | RW | ☆ 电量计跑飞时用 |
| `0x18` | 充电/电量计/看门狗 使能 | RW | ★ `[3]` 电量计（默认 1）、`[1]` **充电使能（软件限充用，§10.5）**、`[0]` 看门狗（别开） |
| `0x19` | 看门狗控制 | RW | ⛔ 别开（开了要定时喂） |
| `0x1A` | 低电告警阈值 L1/L2 | RW | ★ 百分比告警 |
| `0x20` | PWRON status | R | ★ 开机来源 |
| `0x21` | PWROFF status | R | ★ 上次关机原因 |
| `0x22` | PWROFF_EN | RW | ★ PWRON 长按是否关机 |
| `0x23` | DCDC OV/UVP 关机控制 | RW | ⛔ |
| `0x24` | VSYS 欠压关机电平 | RW | ⛔ |
| `0x25` | PWROK 与关断时序 | RW | ⛔ |
| `0x26` | 睡眠/唤醒控制 | RW | ☆ 一般不用（我们用软关机） |
| `0x27` | **IRQLEVEL / OFFLEVEL / ONLEVEL** | RW | ★ 长按阈值 |
| `0x28-0x2B` | 快速开机时序 | RW | ⛔ |
| `0x30` | ADC 通道使能 | RW | ★ `[0]` 电池电压通道 |
| `0x34/0x35` | **vbat 高/低字节** | R | ★ 电压 |
| `0x38/0x39` | vbus 高/低 | R | ☆ |
| `0x3A/0x3B` | vsys 高/低 | R | ☆（估算电流用） |
| `0x3C/0x3D` | tdie 高/低 | R | ☆ 温度 |
| `0x36/0x37` | ts 高/低 | R | ☆ 电池温度 |
| `0x40-0x42` | IRQ 使能 0/1/2 | RW | ★ 按键/低电/充电 |
| `0x48-0x4A` | IRQ 状态 0/1/2 | RW1C | ★ 每类事件 |
| `0x50` | TS 脚控制（是否参与充电判断） | RW | ☆ 见 §12 |
| `0x52-0x57` | TS 迟滞 / 充放电温度阈 | RW | ⛔ |
| `0x58-0x5D` | JEITA 标准 | RW | ⛔ |
| `0x61` | 预充电流 | RW | ☆ 600 mAh 可调小 |
| `0x62` | **恒流充电电流 ICC** | RW | ★ |
| `0x63` | 截止电流 ITERM + 使能 | RW | ★ |
| `0x64` | **恒压充电电压 CV** | RW | ★ 4.2 V |
| `0x65` | 热调节阈值 | RW | ⛔ |
| `0x67` | 充电安全定时器 | RW | ☆ |
| `0x68` | 电池检测使能 | RW | ⛔（默认开） |
| `0x69` | CHGLED 控制 | RW | ☆ |
| `0x6A` | 纽扣电池充电终止电压 | RW | ⛔ |
| `0x80-0x86` | DCDC 开关/电压 | RW | ⛔⛔ **绝对不要写** |
| `0x90-0x9A` | LDO 开关/电压 | RW | ⛔⛔ **绝对不要写** |
| `0xA1` | 电池参数（ROM） | R | 不用 |
| `0xA2` | 电量计控制（ROM/SRAM、BROM） | RW | ☆ 不建议动 |
| `0xA4` | **电池百分比 0–100** | R | ★ |

---

## 5. 功能①：读电池电压

### 5.1 寄存器与换算 [p.28, p.40]

- ADC 是 **14-bit** SAR，通道 0 = VBAT，量程 **0–8.191 V，1 mV/LSB**（`0x1FFF` = 8191 mV）。
- 数据拆分在 `0x34[5:0]`（高 6 位）+ `0x35[7:0]`（低 8 位）。
- ⚠ **必须先读高字节、再读低字节**（读高字节会把当前 ADC 结果锁存），手册明确写了这一点。

```c
static esp_err_t pmu_read_adc14(i2c_master_dev_handle_t dev, uint8_t reg_h, uint16_t *out)
{
    uint8_t b[2];
    /* 先高后低：读 0x34 会锁存，随后读 0x35 拿到同一批数据 */
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &reg_h, 1, &b[0], 1, 100), TAG, "adc h");
    uint8_t reg_l = reg_h + 1;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &reg_l, 1, &b[1], 1, 100), TAG, "adc l");
    *out = ((uint16_t)(b[0] & 0x3F) << 8) | b[1];
    return ESP_OK;
}
/* VBAT(mV) = 原始值 × 1 mV/LSB */
```

### 5.2 采样通道

`0x30[0]` = VBAT 通道使能，**默认 1**；`0x30[3]` VSYS、`[2]` VBUS、`[4]` 结温、`[1]` TS 默认 0（TS 默认 1）。只想读电压就保持默认即可。

### 5.3 精度与使用建议

- 手册**只给了 LSB（1 mV），没给精度指标**（无 ±LSB/INL 数据）。实测应做 4–8 次平均。
- 这里测的是 **BAT 引脚电压**（无分压电阻误差），比 S3 时代"10K+10K 分压 + ADC 校准"干净得多，但**带载时仍有 BATFET 50 mΩ 的压降**：I=100 mA → 5 mV，可忽略；I=1 A → 50 mV，需要留意。
- 充电时电压被抬高（CV 4.2 V + 极化），**不要用充电中的电压算百分比**。

---

## 6. 功能②：电量百分比

### 6.1 E-Gauge 是什么、能给我们什么 [p.28]

- 电量计输出两个东西：**电池容量百分比 `0xA4`**（0–100，只读）和电池电压（`0x34/0x35`）。
- 使能位 `0x18[3]`，**默认 1（已开）**；`0x17` 可以复位电量计（`[3]` 写 1，RWAC）。
- 低电告警：`0x1A[7:4]` = Level2（5–20%，默认 `1010b`=15%），`[3:0]` = Level1（0–15%，默认 `0001b`=1%）；对应 IRQ 在 `0x40[7]`/`0x40[6]`，状态 `0x48[7]`/`0x48[6]`。
- 手册提到可用"电池模块参数"（`0xA1`）做标定，还提到 `0x40[5]`（gauge watchdog timeout）与 `0x40[4]`（gauge new SOC，即 SOC 更新完成）两个 IRQ。

### 6.2 ⚠ 关键限制：没有为本电池标定过

> "Once a default battery is selected for a particular design, **it is highly recommended to program the battery module** to achieve better Fuel Gauge accuracy." [p.28 §6.11]

- 本板电池是 600 mAh / 3.7 V / 4.2 V，**没有（也无法）用 X-Powers 的上位机把电池模型烧进 `0xA1`**；
- 芯片**没有电流采样输入引脚**（QFN-40 引脚表里没有 CSP/CSN 之类的检流输入，§1.1 的 40 脚全部是电源/功率/使能/通信脚），寄存器里也没有电流数据（见 §7.2），所以 E-Gauge 只能靠**电压 + 电池模型 + 学习**推 SOC；陌生电池上**误差可能非常大**（常见现象：满电充电中显示 100%，拔掉充电器瞬间掉到 70%；或长期停在某个值）。
- 手册说它"能自动学习电池特性"，但学习需要完整的充放电循环，且没有给出收敛时间。

**结论：`0xA4` 只能当"参考值"，本项目必须做电压兜底 + 平滑 + 迟滞。** 下面给一套可落地的融合算法。

### 6.3 建议的百分比算法（三层）

```
采样层：每 5 s 读 VBAT（8 次平均）+ 读 0xA4 + 读 0x01 充放电状态
        ↓
判据层：
  ① 充电中（0x01[6:5]==01 或 [2:0]∈{预充,CC,CV}）→ 用 0xA4，但上限锁 99%，充满（[2:0]==100）时 100%
  ② 放电/待机：
     - 若 0xA4 与电压推算值偏差 ≤ 8% → 用两者加权（0.6×gauge + 0.4×电压）
     - 若偏差 > 8%，或 0xA4 ∈ {0, 100} 而电压明显不在两端 → 视为 gauge 不可信，**只用电压表**
  ③ 电压表：见 §6.4 的 OCV 表，线性插值
        ↓
平滑层：变化率限幅（每 60 s 最多 ±2%），单向迟滞（反向变化需持续 30 s）
        ↓
展示层：状态栏/控制中心显示；≤20% 变黄，≤10% 变红；≤5% 触发低电关机
```

限幅的意义：E-Gauge 偶尔会跳变 10%+，直接上屏会让用户觉得"电量乱跳"。

### 6.4 600 mAh 电芯的 OCV–SOC 参考表（25 ℃ 静置）

> 典型 4.2 V 钴酸锂/三元小电芯的**静置开路电压**，用于兜底估算。**必须实测校准**（本项目已删除 S3 时代的 `battery_logger`，可用 `battery_read_voltage_mv()` 的串口日志/开发者选项 + 手动记录来修正；详见 `ideas_and_exps/PMU_RTC_DRIVERS.md`）。

| SOC % | OCV (mV) | SOC % | OCV (mV) |
| --- | --- | --- | --- |
| 100 | 4200 | 40 | 3800 |
| 95 | 4150 | 30 | 3740 |
| 90 | 4110 | 20 | 3680 |
| 80 | 4050 | 15 | 3630 |
| 70 | 3980 | 10 | 3570 |
| 60 | 3920 | 5 | 3490 |
| 50 | 3860 | 3 | 3420 |
| — | — | 0 | 3300 |

使用注意：

- 表是**静置**电压；播放中（负载 100–300 mA）实际电压低 30–80 mV，要按电流补偿，否则会**提前报低电**。
- 本项目现有常量 `BATTERY_CRITICAL_LOW_MV = 3450` 对应上表约 **4%**，可作为"低电关机"阈值；若想更早提醒，用 3.57 V（10%）做 UI 告警。
- 满电 4.2 V 附近曲线极平（4.15→4.20 就有 5%），所以**接近满电时电压法不可靠**，此时反而要信 gauge。

### 6.5 低电策略（接回 power_service）

| 级别 | 判据 | 动作 |
| --- | --- | --- |
| 提醒 | SOC ≤ 20% 或 VBAT ≤ 3.68 V 持续 30 s | 状态栏变黄 + Toast |
| 警告 | SOC ≤ 10% 或 VBAT ≤ 3.57 V 持续 30 s | 变红 + 强提示 |
| 关机 | SOC ≤ 3% **且** VBAT ≤ 3.45 V 持续 60 s（与旧 `CONFIG_LOW_BATTERY_SHUTDOWN_SEC` 语义一致） | 软关机 `0x10[0]=1` |
| 拒启 | 开机时 VBAT < 3.45 V 且 `0x01[6:5]` 不是充电 | 显示"电量过低"页后软关机（旧 boot_flow 的低电拒启逻辑复用） |

> ⚠ 两个判据要 **AND**：充电时 SOC 可能暂时不涨、电压被抬高，单看一个会误判。

---

## 7. 功能③：充放电状态与"当前放电电流"（重点：这个功能芯片给不了）

### 7.1 能拿到的状态 [p.31-32]

`0x01`（PMU status2）：

| 位 | 含义 |
| --- | --- |
| `[6:5]` | **电池电流方向**：00 待机 / 01 充电 / 10 放电 / 11 保留 |
| `[4]` | 系统是否已开机 |
| `[3]` | VINDPM 状态（输入电压被限制） |
| `[2:0]` | **充电阶段**：000 涓流 / 001 预充 / 010 恒流(CC) / 011 恒压(CV) / 100 充电完成 / 101 未充电 |

`0x00`（PMU status1）：`[5]` VBUS 良好、`[4]` BATFET 状态、`[3]` 电池在位、`[2]` 电池 Active 模式、`[1]` 热调节中、`[0]` 输入限流中。

有了这些就能做："充电中/放电中/已充满/未插电"四态，够 UI 画闪电图标了。

### 7.2 ⚠ AXP2101 **没有**电池电流读数

逐个核对过数据手册：

- ADC 只有 5 个通道：**VBAT / VBUS / VSYS / TS / 结温**（`0x30` 使能、`0x34-0x3D` 数据）[p.28, p.39-41]；
- 寄存器列表里**没有任何 battery current / coulomb counter 数据寄存器**（对比 AXP192/202 有 `0xB0-0xB3` 的库仑计，AXP2101 取消了）；
- `0x01[6:5]` 只给**方向**，不给幅值。

所以"获取当前放电电流"在本硬件上**无法直接实现**。三条可落地的替代路线：

#### 方案 A：SOC 微分估算（纯软件，零硬件改动）

```
I_avg ≈ C_nom × ΔSOC / Δt        （C_nom = 600 mAh）
```

- 精度取决于 SOC，而 SOC 本身不可靠（§6.2）→ **误差 30–50%，只能看趋势**；
- 改进：用电压微分配合 OCV 表反推电量变化（`ΔQ ≈ C_nom × (SOC_vbat(t1) − SOC_vbat(t2))`），静置时无意义（ΔSOC≈0），播放中可用；
- 适合"剩余续航估算"：`剩余分钟 ≈ SOC% × 600 mAh / I_avg`。

#### 方案 B：用 BATFET 压降硬算（利用现成两个 ADC 通道）

电池单独供电时 BATFET 全开，`Rds(on) ≈ 50 mΩ` [p.1, p.27]：

```
I ≈ (VBAT − VSYS) / 0.05 Ω        // VBAT=0x34/35, VSYS=0x3A/3B
```

- 分辨率：1 mV / 50 mΩ = **20 mA**，但两个通道**不是同时采样**、各有噪声，实测抖动可能 ±50–100 mA；
- 只在"适配器未插入、BATFET 全开"时成立；插 USB 进入 supplement 模式后 `VDS` 被稳到 30 mV，公式失效；
- 可作为**粗估**（判断"待机 20 mA / 播放 120 mA / 亮屏 200 mA"这种量级），不要拿去显示精确 mA 值。

#### 方案 C：外置电流检测（要改硬件）

- BAT 与系统之间串 **10 mΩ 采样电阻 + INA219/INA226**（I²C 地址可配，能挂在同一条 I²C0 上，不占新引脚），精度 1%；
- 或换带库仑计的 PMU（AXP2202 等，**不是本板**）；
- 这是唯一能拿到"准确瞬时电流"的办法，需要硬件改动 + 飞线，**建议列入 FUTURE_PLAN 评估，不要现在做**。

### 7.3 建议对外 API 的语义

```c
/* 返回值明确区分"测不到"和"测得 0" */
esp_err_t pmu_read_current_ma(int *ma);   /* 方案 B 粗估；不可用时返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t pmu_read_power_state(pmu_power_state_t *st); /* 方向 + 充电阶段 + 是否插电 */
```

UI 上**不要显示 mA 数字**（会误导），只在开发者选项/调试页显示粗估。

---

## 8. 功能④：电源键事件与长按阈值

### 8.1 事件从哪来

PMU 识别 PWRON 的四种状态：**短按 / 长按 / 下降沿 / 上升沿** [p.19 §6.5.4.1]，通过 IRQ 上报：

| 事件 | 使能位 | 状态位（写 1 清） | 默认 |
| --- | --- | --- | --- |
| 短按 | `0x41[3]` | `0x49[3]` | 1（开） |
| 长按 | `0x41[2]` | `0x49[2]` | 1（开） |
| 下降沿（按下） | `0x41[1]` | `0x49[1]` | 0（关） |
| 上升沿（松开） | `0x41[0]` | `0x49[0]` | 0（关） |

- IRQ 脚（GPIO49）是**开漏低有效**，任一使能的事件置位都会拉低；
- 处理完必须**写 1 清状态位**，否则 IRQ 一直低；
- 若需要"按下/松开"时间戳（做双击、组合键），把 `0x41[1:0]` 也打开。

### 8.2 三个时间阈值（`0x27`，这就是"配置长按开关机的阈值"）

| 字段 | 位 | 选项 | 默认 |
| --- | --- | --- | --- |
| **ONLEVEL** | `[1:0]` | 00: 128 ms / 01: 512 ms / 10: 1 s / 11: 2 s | **EFUSE**（板厂烧的，读出来才知道） |
| **OFFLEVEL** | `[3:2]` | 00: 4 s / 01: 6 s / 10: 8 s / 11: 10 s | **6 s** |
| **IRQLEVEL** | `[5:4]` | 00: 1 s / 01: 1.5 s / 10: 2 s / 11: 2.5 s | **1.5 s** |

语义：

- **ONLEVEL**：关机态按住 PWRON 超过它 → 开机 [p.19]；
- **OFFLEVEL**：开机态按住 PWRON 超过它 → **PMU 直接断电关机** [p.20]；是否允许这条路径由 `0x22[1]` 控制（默认 EFUSE），触发后是"关机"还是"重启"由 `0x22[0]` 控制；
- **IRQLEVEL**：手册只给了选项值、没写判定语义（`0x27` 描述只有一行）。按 AXP 系列惯例：**按住超过 IRQLEVEL 判为"长按 IRQ"，短于它判为"短按 IRQ"**。⚠ 这一条**需要实测确认**（打日志：按住 1 s / 2 s 各试一次，看 `0x49` 哪位被置）。

**改法（务必读-改-写，保留其它位）**：

```c
uint8_t v;
i2c_master_transmit_receive(dev, (uint8_t[]){0x27}, 1, &v, 1, 100);
v &= ~(0x3F);                       /* 只清 [5:0]，[7:6] 是 RO */
v |= (1 << 4) | (1 << 2) | 0x01;    /* IRQLEVEL=1s, OFFLEVEL=6s, ONLEVEL=512ms */
uint8_t w[2] = { 0x27, v };
i2c_master_transmit(dev, w, 2, 100);
```

> 建议值：ONLEVEL = 512 ms（防误触、又不至于按住半天不开机）；OFFLEVEL = 6 s（默认，和手机习惯接近）；IRQLEVEL = 1.5 s（默认，用作"长按 = 关机/电源菜单"的固件判据）。

### 8.3 固件里的按键语义建议

| 场景 | 事件 | 动作 |
| --- | --- | --- |
| 亮屏 | 短按 | 息屏（`power_service_screen_off`） |
| 息屏 | 短按 | 亮屏 |
| 播放中 | 短按 | 亮屏/切歌（按现有 `key_handler` 约定） |
| 任意 | **长按（IRQLEVEL，1.5 s）** | 弹"关机"确认或直接软关机 |
| 关机态 | 按住 > ONLEVEL（512 ms） | PMU 自己开机，固件无需参与 |
| 任意 | 按住 > OFFLEVEL（6 s） | PMU **硬断电**（用户强制关机，固件无法拦截） |

> ⚠ 编写本文时 `button_service` 只处理下方 GPIO61 按键；**现状已合并**：`button_service` 用 `axp2101_is_power_key_pressed()` 维护上键（PMU PWRON）的按住状态，统一生成上/下键短按、长按与双键同时按下，`key_handler` 只做应用分发。

### 8.4 ISR / 任务模型（必须遵守）

```
GPIO49 下降沿 ISR（IRAM，极短）
    → xQueueSendFromISR(pmu_evt_queue, ...) / vTaskNotifyGiveFromISR
        → pmu_svc 任务（PSRAM 栈，prio 4~5）
            → i2c 读 0x49 / 0x4A / 0x48
            → 写 1 清对应位
            → 分发事件（按键 / 低电 / 充电插入 / 充满）
```

- **绝不能在 ISR 里做 I²C**（新版驱动会阻塞、可能拿互斥锁）；
- 读状态时建议一次连续读 `0x48..0x4A`（3 字节）再统一清，避免"清一个又漏一个"；
- 事件回调里如果要动 UI，按本项目约定用 `lvgl_port_lock()` 或走 `config_store` 的延迟通知。

---

## 9. 功能⑤：软关机（替代 S3 时代的"伪关机 + deepsleep"）

### 9.1 为什么应该换

📅 S3 时代（1.x）的 `fake_shutdown` 是"停外设 + 进 deepsleep"（为了省电，deep sleep 几十 µA）。**S31（2.x）已改为 AXP2101 软关机真断电**（`fake_shutdown.c` 写 `0x10[0]=1`，文件名保留）。下表是当时的方案对比：

| 方案 | 关机电流 | 唤醒方式 | 体验 |
| --- | --- | --- | --- |
| deepsleep（现状） | 取决于板级漏电，通常 100 µA 级 | GPIO/定时器唤醒 | 假关机，RTC/内存部分保留 |
| **AXP2101 软关机（推荐）** | **~40 µA**（手册标称，只留 RTCLDO） | 按 PWRON > ONLEVEL / 插 USB | **真关机**，和消费电子一致 |

### 9.2 流程

```
用户长按/定时到点
  → 停播放、停 LVGL 任务、关屏、释放 SD、确保没有 flash 写操作
  → pmu_power_off()
      ├─ 可选：先把 PWROFF 原因写 NVS（下次开机区分"用户关机/低电关机"）
      ├─ 写 0x10[0]=1
      └─ 等待断电（不要 return 后继续跑）
  → PMU 关掉除 RTCLDO 外所有输出，MCU 掉电
```

注意：

- **写 `0x10[0]` 前必须保证没有正在进行的 flash 写**（NVS/pldata），断电会写坏；用 `config_store_flush_pending()` + 等播放任务退出；
- 断电后 RTC（PCF85063）**继续走时**（RTCLDO 供电），所以关机再开机时间不丢 —— 这也是 RTC 适配的价值（见 [rtc_pcf85063a.md](rtc_pcf85063a.md)）；
- 软关机后 **无法被定时器唤醒**（PMU 不认 RTC 闹钟），睡眠定时器语义要改：到点即关机，不再"定时唤醒"；
- `0x26`（sleep/wakeup）是另一套轻睡眠机制（部分电源轨断电 + 唤醒恢复），**不建议用**——它的语义比软关机复杂，且电收益不如真关机。

### 9.3 开机引导要做的三件事

1. 读 `0x20` 判断开机来源（按键 / USB 插入 / 电池插入）；
2. 读 `0x21` 判断上次关机原因（软关机 / PWRON 长按 / 欠压保护），欠压关机时开机直接进"充电/低电"页；
3. 读电压 + SOC，低电则走拒启流程。

---

## 10. 充电配置（按 600 mAh / 4.2 V 整定）

### 10.1 出厂默认 vs 建议值

| 参数 | 寄存器 | 出厂默认 | 600 mAh 建议 | 理由 |
| --- | --- | --- | --- | --- |
| 恒流充电电流 ICC | `0x62[4:0]` | {EFUSE,0b,EFUSE}（常见 300 mA） | **300 mA**（0.5C） | 小电芯 0.5C 温和；要快充可到 600 mA(1C)，但发热明显 |
| 恒压充电电压 CV | `0x64[2:0]` | `011b` = **4.2 V** | **4.2 V**（不改；想"不满充"见 §10.5） | 与电池满电电压一致；4.35/4.4 V 会过充 |
| 截止电流 ITERM | `0x63[3:0]` | `0101b` = 125 mA | **25 mA（`0001b`）或 50 mA（`0010b`）** | 125 mA ≈ 0.2C，会在 80–90% 就判充满，白白损失容量 |
| 截止使能 | `0x63[4]` | 1 | 1 | 保持 |
| 预充电流 IPRE | `0x61[3:0]` | 125 mA | **25–50 mA** | <3.0 V 时小电流唤醒更安全 |
| 充电安全定时器 | `0x67[5:4]` | 12 h | 5 h（`00b`）足够 | 600 mAh@300 mA 约 2.5 h，5 h 留余量 |
| 预充超时 | `0x67[1:0]` | 手册正文 50 min / 寄存器表 60 min（**不一致**） | 保持默认 | 实测确认即可 |
| 输入限流 IINLIM | `0x16[2:0]` | 1500 mA | **500 mA（`001b`）** 接普通 USB；接 2 A 电源可放 1500 mA | 避免拉垮弱 USB 口 |
| VBUS 输入电压限制 | `0x15[3:0]` | 4.36 V | 不改 | — |
| 电池检测 | `0x68[0]` | 1 | 不改 | — |
| 充电状态 | `0x01[2:0]` | — | 只读 | — |

### 10.2 ICC 编码公式 [p.48]

```
N = 0x62[4:0]
N ≤ 8 : ICC = 25 × N            mA   （0, 25, 50, 75, 100, 125, 150, 175, 200）
N > 8 : ICC = 200 + 100 × (N-8) mA   （0x09=300, 0x0A=400, 0x0B=500, 0x0C=600,
                                       0x0D=700, 0x0E=800, 0x0F=900, 0x10=1000）
```
> 注意这里**不是线性**的：`0x08=200 mA` 而 `0x09=300 mA`，跨越 0.5C→1C 时不要按 +25 mA 算。

### 10.3 充电相关 IRQ

| 事件 | 使能 | 状态 |
| --- | --- | --- |
| 充电开始 | `0x42[3]` | `0x4A[3]` |
| 充电完成 | `0x42[4]` | `0x4A[4]` |
| 充电安全定时器超时 | `0x42[1]` | `0x4A[1]` |
| 电池过压保护 | `0x42[0]` | `0x4A[0]` |
| VBUS 插入/拔出 | `0x41[7]`/`0x41[6]` | `0x49[7]`/`0x49[6]` |
| 电池过温(充电) | `0x40[3]` | `0x48[3]` |

> "充电完成"的判定：`0x01[2:0] == 100`（charge done），或 `0x4A[4]`。充满后若电压掉到 `VREG − 100 mV`（recharge 阈值）会自动再充，不需要软件干预。

### 10.4 充电时的电压行为（解释"电量卡在 99%"）

- 阶段：**涓流（<2 V，10 mA）→ 预充（<3.0 V）→ CC → CV → 完成**；
- CV 阶段电流逐步降到 ITERM 才判完成；ITERM 设太大 → 判完成太早 → 实际只充到 80–90%；
- 完成后 `0x01[2:0]=100`，同时 **BATFET 会被关掉**（手册写明"stops charging … and turns off BATFET"）——所以"充满后拔掉电源"瞬间，系统会短暂切到电池，属正常现象。

### 10.5 能不能限制"最大充电电压 / 最大充电电量"？（2026-11 专项调查）

**结论：芯片没有"充到 X% 就停"的功能，但有三条可组合的手段** —— `0x64` 充电电压档位（真正的"电压上限"）、`0x18[1]` 充电使能开关（软件随时停充）、`0x63` 截止电流（让充电提前判满）。

原因在于 **PMU 的充电过程是完全自主的**：

> "When PMU meets all charging conditions, it can complete the whole charging process **without the participation of Host**." [p.23 §6.7.3]

只要 VBUS 在、`0x18[1]=1`，它就会一路充到 `VREG`、等电流降到 `ITERM` 才判满；**电量计 `0xA4` 只用于上报，不参与充电决策**（寄存器层面没有任何"目标电量 / 充电上限百分比"的输入口）。

#### 10.5.1 能力对照表

| 想要的效果 | 芯片支持 | 手段 | 说明 |
| --- | --- | --- | --- |
| 设定**充电电压上限** | ✅ | `0x64[2:0]`，4.0 / 4.1 / 4.2 / 4.35 / 4.4 V 五档可用（另有 5.0 V 的错误档） | 唯一能"设定充到多高电压"的地方；见 §10.5.2 |
| **充到某个电量百分比就停** | ❌ **无此功能** | — | 没有 SOC 目标寄存器，充电不接受电量计反馈 |
| 软件随时**停止/恢复充电** | ✅ | `0x18[1]`（Cell Battery charge enable，默认 1） | 只关充电，不断系统供电；见 §10.5.3 方案 B |
| 让充电**提前结束**（等效降充满度） | ⚠ 间接 | 调大 `0x63[3:0]` 截止电流 | 只在 CV 段起作用，"提前判满"，不是精确百分比 |
| 限制**充电电流**（发热/时长） | ✅ | `0x62[4:0]` | 不改变最终电压 |
| 限制**充电时间** | ✅ | `0x67` 安全定时器 | 超时是**停止充电 + 报 IRQ**（保护），不是限充策略 |
| 温度相关的电压/电流折减 | ✅（JEITA） | `0x58` 使能、`0x59-0x5B` 温区 | 高/低温时自动把 CV 降 1~2 档、电流降 50%，但**不能指定目标** |
| 电池过压保护 | ✅（保护） | 内部 VBAT OVP ≈ 4.4 V | 是保护动作，**不是**可以拿来做"充电上限"的设定 |

#### 10.5.2 `0x64[2:0]`：充电电压（CV 目标）全编码 [p.48]

| 编码 | 电压 | 能不能用（本项目 4.2 V 电芯） |
| --- | --- | --- |
| `000b` | **5.0 V** | ❌ 绝对不要（手册标注值，对单节 4.2 V 电芯是错误档；疑似测试档/笔误） |
| `001b` | 4.0 V | ✅ 可作"保养模式"（约 75–80% 电量） |
| `010b` | 4.1 V | ✅ 可作"较保养"（约 85–90%） |
| `011b` | **4.2 V** | ✅ 标准（出厂默认） |
| `100b` | 4.35 V | ❌ 高压电芯专用 |
| `101b` | 4.4 V | ❌ 高压电芯专用（正好等于 VBAT OVP 保护点） |
| `11Xb` | reserved | ❌ |

要点：

- 这里的 CV 是**充满目标电压**（CC→CV 的切换点、CV 阶段维持的电压），不是"过压保护阈值"；改它只影响"充到多满"，不影响充电电流；
- **每次开机（POR/System Reset）都会回默认 4.2 V**，必须在 `pmu_init()` 里重新下发；
- 充满后若 VBAT 掉到 `VREG − 100 mV` 会自动再充（re-charge），不需要软件干预 [p.24 §6.7.3.4]；
- 充满瞬间 PMU 会**关掉 BATFET**（§10.4），这是正常行为，不是掉电。

#### 10.5.3 三条"不满充"路线

**方案 A：只改 CV（推荐，最简单、不依赖固件常驻）**

| CV | 估算满充 SOC | 代价 |
| --- | --- | --- |
| 4.0 V | ≈ 75–80% | 续航损失约 20%，循环寿命收益最大 |
| 4.1 V | ≈ 85–90% | 续航损失约 10%，折中 |
| 4.2 V | 100% | 默认 |

- 优点：充电、终止、再充全部由 PMU 自己完成，**固件崩了也不会漏充/过充**；插拔适配器不用管；
- 缺点：档位粗（没有 4.05 V 这种中间值）；"满"变成 4.1/4.0 V 后，**电量计仍按 4.2 V 的模型算，UI 可能永远到不了 100%**，需要把 ≥（该电压对应 SOC）显示为"已充满"；
- ⚠ 上表的 SOC 是用 §6.4 的 OCV 表**估**的（充电时有极化/内阻，实际会偏高），必须实测（见 §17）。

**方案 B：软件到点停充（`0x18[1]=0`）**

```
判停：VBUS 在位 且 充电阶段 ∈ {CC, CV} 且 VBAT ≥ 4.05 V（≈80%）
停充：0x18 ← (原值 & ~0x02)      /* 只清 bit1，保留 bit3 电量计、bit0 看门狗 */
恢复：VBAT ≤ 3.90 V（迟滞）  或  重新插入 VBUS（监听 0x49[7] 的 VBUS 插入 IRQ）
```

- 优点：真能做到"充到 80% 就停"，可以给用户做成开关/保养模式；
- 缺点：**依赖固件常驻**（PMU 任务挂了就再也不充）；阈值受负载与内阻影响会漂；停充发生在 CV 前段，没有涓流收尾；
- ⚠ **别用 `0xA4` 当判停依据**：电量计未标定（§6.2），拿它当开关会误判；用电压；
- ⚠ `0x18[1]` **不会因为插拔 VBUS 而自动恢复**（PMU 一直有电池供电，寄存器不复位），必须自己写回；这一点需实测确认（见 §17）。

**方案 C：A + B 组合**（CV 4.1 V 兜底 + 软件 80% 停）—— 双保险，适合"电池保养模式"开关。

#### 10.5.4 伪代码（方案 B）

```c
/* 充电上限（0=关闭 / 80 / 90），在 PMU 轮询任务里每 5 s 调一次 */
void pmu_charge_limit_tick(const axp2101_status_t *st)
{
    static bool stopped = false;
    const uint16_t stop_mv   = 4050;   /* ≈80%：4.05 V（按 §6.4 OCV 表） */
    const uint16_t resume_mv = 3900;   /* 迟滞，防抖 */

    if (!st->vbus_present) {           /* 拔掉适配器 → 复位状态机 */
        stopped = false;
        return;
    }
    if (!stopped && st->vbat_mv >= stop_mv &&
        (st->charge_stage == AXP2101_CHG_CC || st->charge_stage == AXP2101_CHG_CV)) {
        axp2101_set_charge_enable(false);   /* 0x18[1] = 0 */
        stopped = true;
    } else if (stopped && st->vbat_mv <= resume_mv) {
        axp2101_set_charge_enable(true);    /* 0x18[1] = 1 */
        stopped = false;
    }
}
```

#### 10.5.5 本项目落地现状（改动量很小）

> ⚠ **2026-11 真机实测补充（很重要）**：把 CV 降到**低于当前电池电压**（例如电池 4.17V、
> CV 4.0V）时，PMU 会进入"电池高于目标/充电完成"状态：它自己把 `0x18[1]`（充电使能）
> 清成 0，并**断开 BATFET**。这时插着 VBUS 系统还能跑（由 VBUS 供电），但**拔掉充电器
> 的瞬间 VSYS 没有电池来源 → DCDC 欠压保护直接断电**（`0x21=0x20`）。
> 所以"降 CV 做保养"只适合电池本来就低于目标电压的场合，正常使用会被坑；
> 先把电放到 4.0V 以下再开保护也只能把问题推迟到下一次充满（到 4.0V 又会断 BATFET）。
> 本项目最终采用 **CV 保持 4.2V + 软件 `0x18[1]=0` 停充（4.0V 停 / 3.9V 恢复）**，
> 避免进入"完成"态、BATFET 保持导通；实现见
> [`../POWER_OPTIONS_AND_CHARGE_BOOT.md`](../POWER_OPTIONS_AND_CHARGE_BOOT.md) §3。

- **已实现**：`axp2101_set_charge_voltage(axp2101_cv_t)`（CV 档位枚举）与 `axp2101_set_charge_enable(bool)`（读-改-写 `0x18` bit1，**保留 bit3=1、bit0=0**）都在 `main/core/hardware/axp2101.h` 里；电池健康保护就是"CV 保持 4.2 V + 4.0 V 写 `0x18[1]=0` 停充 / 3.9 V 恢复"，见 `POWER_OPTIONS_AND_CHARGE_BOOT.md` §3；
- Kconfig 建议：`CONFIG_PMU_CHARGE_CV`（choice：4.0 / 4.1 / 4.2 V）与 `CONFIG_PMU_CHARGE_LIMIT_PERCENT`（0=关 / 80 / 90），默认都取"关/4.2 V"，避免改变现有行为。

#### 10.5.6 明确走不通的思路

- ❌ 用 `0x1A`（低电告警 L1/L2）反向控制充电：它只产生 IRQ，不参与充电决策；
- ❌ 用 `0x63` 截止电流精确控 SOC：它只在 CV 段生效，调大只是"提前判满"，一致性很差；
- ❌ 用 JEITA 的电压折减（`0x59[3:2]`/`[1:0]`）当限充：那是温度保护，常温下不动作；
- ❌ 用 `0x24`（VOFF 关机阈值，2.6–3.3 V）当"放电下限"：它是**关机保护**，不是可用的放电截止设定（真正的过放保护在电池包的保护板里）。

---

## 11. 电源轨：只读、不改（⛔ 红线）

手册给出 5 路 DCDC + 11 路 LDO 的完整电压/开关寄存器（`0x80-0x86`、`0x90-0x9A`），**全部由板厂的 EFUSE 默认值配置**，寄存器 reset 值多处标注为 `EFUSE` 而非固定值 —— 也就是说**每块板子的默认值可能不同**。

⚠ **绝对不要写这些寄存器**：

- 写错 DCDC1（3.3 V IO 电源）或 ALDO1（1.8 V）→ 直接掉电/屏幕不亮，且因为寄存器在下次上电才恢复，"改坏 → 重启就恢复"并不可靠；
- `0x12[3]`（BATFET 在关机态使能）也不要动，改了可能导致**电池供电开不了机**；
- `0x25`（PWROK/关断时序）、`0x28-0x2B`（快速开机时序）同理。

需要知道"某路电源多少伏"时：**读出来**（`0x82-0x86`、`0x92-0x9A`），只做日志/诊断。

---

## 12. 温度 / TS 引脚（上板必查项）

- TS 用于**电池温度保护**：PMU 往 TS 注恒流（`0x50[1:0]`：20/40/50/60 µA，默认 50 µA），外部 NTC 到 GND，ADC 读回电压（`0x36/0x37`）。
- **充电条件里包含"TS 电压在允许范围"**，超出范围 → **暂停充电并报 IRQ** [p.26]。
- 若本板**没有装 NTC**：TS 悬空会读到异常电压，可能导致**充不上电**。手册给的解法 [p.27]：
  > "Some battery may have no temperature sensitive resistor. Under this situation, **TS pin can be pulled down to GND with a 10 KOhm resistor externally** or set as external input of ADC through register."

  → 软件兜底：`0x50[4]=1`（TS 作为外部固定输入，**不参与充电判定**）。
- **上板第一件事**：读 `0x36/0x37` 换算电压。
  - ≈ 0.5 V → 有 10 kΩ NTC（25 ℃ 典型值），正常；
  - ≈ 0.0 V → 被 10 kΩ 下拉到地，正常（无 NTC 但已按推荐处理）；
  - 其他值/浮动 → 需要写 `0x50[4]=1` 再试充电。
- 温度阈值（`0x54-0x57`）默认：充电低温 ≈0 ℃ / 高温 ≈55 ℃，工作低温 ≈-10 ℃ / 高温 ≈60 ℃，一般不用改。
- 结温 ADC：`0x3C/0x3D`，量程 0.1 mV/LSB 对应 ℃（`[p.28]` 表 6-5 写的是 0.1 mV/LSB、0–0.8191 V，实际换算按 X-Powers 惯例 0.1 mV = 0.1 ℃，**需实测标定**）。

---

## 13. IRQ 汇总表（写 1 清除）

| 组 | 寄存器 | 关键位 |
| --- | --- | --- |
| 电量 | `0x40` / `0x48` | `[7]` SOC 低于 Level2、`[6]` 低于 Level1、`[5]` 电量计看门狗、`[4]` SOC 更新 |
| 电池温度 | `0x40` / `0x48` | `[3]` 充电过温、`[2]` 充电低温、`[1]` 工作过温、`[0]` 工作低温 |
| 插拔 | `0x41` / `0x49` | `[7]` VBUS 插入、`[6]` VBUS 拔出、`[5]` 电池插入、`[4]` 电池拔出 |
| 电源键 | `0x41` / `0x49` | `[3]` 短按、`[2]` 长按、`[1]` 下降沿、`[0]` 上升沿 |
| 系统/充电 | `0x42` / `0x4A` | `[7]` 看门狗、`[6]` LDO 过流、`[4]` 充满、`[3]` 开始充电、`[2]` 结温 L1、`[1]` 充电超时、`[0]` 电池 OVP |

清除方式：向状态寄存器**对应位写 1**（RW1C）。注意有的位是"条件消失自动清"（如过温、OVP），手册在每位的说明里注明了。

---

## 14. 本项目集成方案

### 14.1 建议的文件与分层

```
main/core/hardware/
  axp2101.h / axp2101.c      # 纯寄存器驱动：读写、电压、SOC、状态、按键阈值、软关机
  battery.h / battery.c      # 应用层封装（保留现有 API，内部改为调 axp2101）
main/core/utils/
  power_service.c            # 接入低电策略（读 battery/pmu，不再用 ADC）
```

- **`axp2101.c` 不依赖 LVGL、不建任务**，只做"寄存器 ↔ 语义"；
- `battery.c` 保留 `battery_init()` / `battery_read_voltage_mv()` 两个现有符号（调用方零改动），内部换成 PMU；新增 `battery_read_percent()` 等；
- 驱动里不要 `printf` 日志洪水：PMU 每 5 s 读一次，用 `ESP_LOGD`。

### 14.2 API 草案（与本项目命名习惯一致）

```c
typedef enum {
    PMU_CHG_NONE = 0, PMU_CHG_TRICKLE, PMU_CHG_PRECHARGE,
    PMU_CHG_CC, PMU_CHG_CV, PMU_CHG_DONE,
} pmu_charge_stage_t;

typedef enum {
    PMU_CURRENT_STANDBY = 0, PMU_CURRENT_CHARGING, PMU_CURRENT_DISCHARGING,
} pmu_current_dir_t;

typedef struct {
    uint16_t vbat_mv;        /* 0x34/0x35 */
    uint16_t vsys_mv;        /* 0x3A/0x3B，可选 */
    uint8_t  soc;            /* 0xA4 */
    bool     vbus_present;   /* 0x00[5] */
    bool     battery_present;/* 0x00[3] */
    pmu_current_dir_t dir;   /* 0x01[6:5] */
    pmu_charge_stage_t chg;  /* 0x01[2:0] */
} pmu_status_t;

typedef enum { PMU_KEY_SHORT, PMU_KEY_LONG, PMU_KEY_DOWN, PMU_KEY_UP } pmu_key_event_t;
typedef void (*pmu_key_cb_t)(pmu_key_event_t ev, void *ctx);

/* 初始化：复用 I²C0 总线句柄，建 0x34 设备、读芯片状态、下发推荐配置、挂 GPIO49 中断 */
esp_err_t pmu_init(i2c_master_bus_handle_t bus);

esp_err_t pmu_read_status(pmu_status_t *st);
esp_err_t pmu_read_soc(uint8_t *pct);
esp_err_t pmu_read_vbat_mv(uint16_t *mv);
esp_err_t pmu_read_current_ma(int32_t *ma);          /* 方案 B 粗估，可能 NOT_SUPPORTED */

esp_err_t pmu_configure_charge(uint16_t icc_ma, uint8_t iterm_code, bool vreg_42v);
esp_err_t pmu_set_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel);
esp_err_t pmu_register_key_cb(pmu_key_cb_t cb, void *ctx);

pmu_boot_reason_t pmu_get_boot_reason(void);          /* 解析 0x20/0x21 */
esp_err_t pmu_power_off(void);                        /* 0x10[0]=1，不返回 */
```

### 14.3 初始化顺序（改 `main.c`）

```
config_store_init
deep_worker_init
display_init
lvgl_port_init / boot_splash
touch_init            ← 这里同步建好 I²C0 bus（i2c_new_master_bus 是同步的）
battery_init          ← 改为 pmu_init(touch_get_i2c_bus())：读电压/SOC/开机原因 + 下发配置
audio_init
pldata_init
button_service_init   ← 下方 GPIO61 键，与 PMU 键是两套
sd_card_init
boot_flow_run         ← 用 pmu 的电压/SOC 做低电拒启
...
```

⚠ `battery_init()` 现在的位置（`touch_init()` 之后）刚好可用；**不要**挪到 `touch_init()` 之前，否则拿不到 bus handle。

### 14.4 Kconfig 建议（`main/Kconfig.projbuild`）

| 选项 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `CONFIG_PMU_AXP2101` | bool | y | 总开关；迁移期可关掉退回占位 |
| `CONFIG_PMU_BAT_CAPACITY_MAH` | int | 600 | 电池容量，用于续航估算/电流粗估 |
| `CONFIG_PMU_ICC_MA` | choice | 300 | 充电电流（100/300/500/600/1000） |
| `CONFIG_PMU_LOW_BAT_PERCENT` | int | 5 | 低电关机百分比 |
| `CONFIG_PMU_LOW_BAT_SHUTDOWN_SEC` | int | 60 | 持续多久才关（沿用旧名语义） |
| `CONFIG_PMU_KEY_OFFLEVEL` | choice | 6s | 4/6/8/10 s |
| `CONFIG_PMU_KEY_ONLEVEL` | choice | 512ms | 128ms/512ms/1s/2s |
| `CONFIG_PMU_POWEROFF_HOLD_16S` | bool | y | 打开 `0x10[2]` 的 16 s 强制断电兜底 |

### 14.5 UI 对接

- 📅 写本文时顶部状态栏第三格是**电池电压**（S3 语义）；**现状已是百分比 + 充电图标**（`main/ui/battery_indicator.c`），左格显示 RTC 时间，电压细节在设置菜单 →「电源选项」；
- 下拉控制中心顶部右侧同样显示百分比（已实现）；
- 低电提示统一用现有 `toast`；
- 充电中显示"⚡"；`0x01[2:0]==100` 显示"已充满"。

### 14.6 迁移步骤（建议分 5 步提交，每步真机验证）

1. **只读**：`pmu_init` + 读电压/SOC/状态，串口打印（不动任何 UI）。验证：串口数值与万用表对得上（±20 mV）。
2. **按键**：GPIO49 中断 + `0x49` 解析 + 日志。验证：短按/长按分别打出正确事件，IRQ 不卡死。
3. **UI**：状态栏换百分比，低电变黄/红。
4. **策略**：低电关机接 `power_service`，开机拒启接 `boot_flow`。
5. **软关机**：`fake_shutdown` 的"等待断电"分支改为 `pmu_power_off()`（保留 deepsleep 作为 A/B 兜底，用 Kconfig 切换）。

### 14.7 验证清单（真机）

- [ ] 电池单独供电：长按 512 ms 开机；`0x20[0]=1`
- [ ] 插 USB：自动开机；`0x20[2]=1`；`0x00[5]=1`
- [ ] 电压读数与万用表差 ≤ 20 mV（静置、拔充电器）
- [ ] SOC 在满充→放空一轮后曲线单调（允许 ±2% 抖动，不允许跳变 >10%）
- [ ] 短按/长按 IRQ 分别命中 `0x49[3]`/`0x49[2]`，清位后 IRQ 释放
- [ ] 按住 6 s：PMU 断电（示波器看 VSYS 掉电），松手后保持关机
- [ ] 软关机：写 `0x10[0]` 后电流 < 100 µA（实测板级漏电）
- [ ] TS 电压读数正常（0.5 V 或 0 V），充电能进入 CC
- [ ] 充电 300 mA ±20%，CV 4.20 V ±0.5%
- [ ] 充满后 `0x01[2:0]=100`，再放电 100 mV 后自动 re-charge

---

## 15. 踩坑清单（按血泪程度排序）

1. **电池单独供电不开机**：BATFET 默认关，必须 PWRON 或 VBUS。别以为是固件挂死。
2. **ADC 高低字节顺序**：必须先读 `0x34`（锁存）再读 `0x35`。先读低的会拿到旧数据/错位。
3. **E-Gauge 没标定**：`0xA4` 在陌生电池上可能离谱。必须电压兜底 + 限幅，否则 UI 电量乱跳。
4. **芯片没有电流寄存器**：`0x01[6:5]` 只是方向。不要写个 `pmu_read_current()` 去读一个不存在的寄存器。
5. **改 `0x27` 要读-改-写**：`[7:6]` 是只读保留位，整字节写可能踩到未定义位。
6. **`0x22[1]` 可能没开**：OFFLEVEL 长按是否真的关机取决于 EFUSE 默认；要"一定有关机兜底"就自己写 `0x22[1]=1`（并确认 `0x22[0]` 选的是 power-off 而不是 restart）。
7. **软关机前必须停 flash 写**：NVS/pldata 正在写时断电会损坏数据。先 flush、停播放/扫描任务。
8. **软关机后不能定时唤醒**：PMU 不认 RTC 闹钟，睡眠定时器语义要改。
9. **IRQ 是开漏**：GPIO49 必须外部 4.7 kΩ 上拉（板上有），配置成 `GPIO_PULLUP_ONLY` + 下降沿；**中断里不能 I²C**。
10. **状态位不清会反复进中断**：`0x49`/`0x4A` 必须写 1 清；建议先读后清。
11. **`0x63` ITERM 默认 125 mA 对 600 mAh 太大**：会导致"充不满"，用户会报"电量只到 88%"。
12. **`0x62` ICC 编码非线性**：`0x08`→200 mA、`0x09`→300 mA，别按 25 mA 步进算。
13. **ICHG 精度 ±20%**、CV ±0.5%：不要用 PMU 寄存器反推精确电流。
14. **TS 悬空可能充不上电**：先读 `0x36/0x37`；没 NTC 时按手册处理（10 kΩ 下拉或 `0x50[4]=1`）。
15. **别写 `0x80-0x86` / `0x90-0x9A`**：EFUSE 默认值，写错直接不开机/黑屏。
16. **`0x10[1]` 是重启 PMU**（不是关机），会复位所有寄存器并重启 SoC。
17. **看门狗 `0x18[0]` 默认关**：别手滑打开，否则 4 s（`0x19[2:0]` 默认）不喂就复位。
18. **手册自身有三处不一致**：预充定时器默认（正文 50 min vs 寄存器表 60 min）、DCDC5 表里多出一个 `11001b: 1.2 V`（与 1.4–3.7 V/100 mV 步进公式矛盾）、正文说 OFFLEVEL 后是否自动开机看 `0x22[3:2]` 但寄存器表里 `[7:3]` 是只读、真正定义的是 `0x22[0]`（0=关机/1=重启）。以寄存器表和实测为准。
19. **POR 后寄存器回默认**：每次开机都要重新下发充电/按键配置，不要假设"上次写过就一直在"。
20. **`0x01[2:0]` 的 101 是"未充电"**，不是"放电"；放电态看 `[6:5]=10`。
21. **没有"充电到 X% 停止"的寄存器**：PMU 充电全自动（§10.5）。想不满充只有两条路——降 CV（`0x64`，粗档）或软件写 `0x18[1]=0` 停充；**两种都不会被"插拔 VBUS"自动撤销**，改了就得自己记着改回来。
22. **CV 只能 ≤ 4.2 V**（本项目电芯）：`100b`=4.35 V、`101b`=4.4 V 是高压电芯专用，`000b` 更标着 5.0 V——设错就是过充，没有 BMS 兜底时直接鼓包/起火。改 CV 前先读回 `0x64` 确认当前值。

---

## 16. 参考代码骨架

```c
/* ======================= axp2101.c 骨架（示意） ======================= */
#define AXP2101_ADDR        0x34
#define REG_STATUS1         0x00
#define REG_STATUS2         0x01
#define REG_COMMON_CFG      0x10
#define REG_GAUGE_CFG       0x18
#define REG_LOW_BAT_TH      0x1A
#define REG_PWRON_STATUS    0x20
#define REG_PWROFF_EN       0x22
#define REG_KEY_LEVEL       0x27
#define REG_ADC_EN          0x30
#define REG_VBAT_H          0x34
#define REG_VBAT_L          0x35
#define REG_IRQ_EN1         0x41
#define REG_IRQ_ST1         0x49
#define REG_ICC             0x62
#define REG_ITERM           0x63
#define REG_CV              0x64
#define REG_SOC             0xA4

static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(d, &reg, 1, buf, n, 100);
}
static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, 2, 100);
}
static esp_err_t wr_masked(i2c_master_dev_handle_t d, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(rd(d, reg, &v, 1), TAG, "rd");
    v = (v & ~mask) | (val & mask);
    return wr(d, reg, v);
}

/* 电压：先高后低 */
esp_err_t pmu_read_vbat_mv(uint16_t *mv)
{
    uint8_t h, l;
    ESP_RETURN_ON_ERROR(rd(s_dev, REG_VBAT_H, &h, 1), TAG, "vbat_h");
    ESP_RETURN_ON_ERROR(rd(s_dev, REG_VBAT_L, &l, 1), TAG, "vbat_l");
    *mv = (uint16_t)(((h & 0x3F) << 8) | l);   /* 1 mV/LSB */
    return ESP_OK;
}

/* SOC */
esp_err_t pmu_read_soc(uint8_t *pct)
{
    return rd(s_dev, REG_SOC, pct, 1);
}

/* 充放电状态 */
esp_err_t pmu_read_status(pmu_status_t *st)
{
    uint8_t s0, s1;
    ESP_RETURN_ON_ERROR(rd(s_dev, REG_STATUS1, &s0, 1), TAG, "st0");
    ESP_RETURN_ON_ERROR(rd(s_dev, REG_STATUS1 + 1, &s1, 1), TAG, "st1");
    st->vbus_present    = (s0 >> 5) & 1;
    st->battery_present = (s0 >> 3) & 1;
    st->dir = (pmu_current_dir_t)((s1 >> 5) & 0x3);
    st->chg = (pmu_charge_stage_t)(s1 & 0x7);
    return pmu_read_vbat_mv(&st->vbat_mv);
}

/* 粗估电流：只在不插电时用（BATFET 全开，Rds(on)=50 mΩ） */
esp_err_t pmu_read_current_ma(int32_t *ma)
{
    uint16_t vbat, vsys;
    ESP_RETURN_ON_ERROR(pmu_read_vbat_mv(&vbat), TAG, "vbat");
    ESP_RETURN_ON_ERROR(pmu_read_adc14(s_dev, 0x3A, &vsys), TAG, "vsys");
    *ma = ((int32_t)vbat - (int32_t)vsys) * 1000 / 50;   /* (mV)/(50 mΩ) = mA */
    return (*ma < 0) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/* 按键配置 */
esp_err_t pmu_set_key_levels(uint8_t onlevel, uint8_t offlevel, uint8_t irqlevel)
{
    uint8_t v = (uint8_t)(((irqlevel & 3) << 4) | ((offlevel & 3) << 2) | (onlevel & 3));
    return wr_masked(s_dev, REG_KEY_LEVEL, 0x3F, v);
}

/* 软关机：写 0x10[0]=1，之后 PMU 断电 */
esp_err_t pmu_power_off(void)
{
    ESP_LOGW(TAG, "PMU soft power off");
    return wr_masked(s_dev, REG_COMMON_CFG, 0x01, 0x01);
}
```

```c
/* ======================= 电量融合（示意） ======================= */
static uint8_t soc_from_voltage(uint16_t mv)
{
    /* §6.4 的 OCV 表，降序查表 + 线性插值；这里只给形状 */
    static const uint16_t ocv_mv[] = {4200,4150,4110,4050,3980,3920,3860,3800,
                                      3740,3680,3630,3570,3490,3420,3300};
    static const uint8_t  ocv_soc[] = {100,95,90,80,70,60,50,40,
                                       30,20,15,10,5,3,0};
    if (mv >= ocv_mv[0]) return 100;
    for (int i = 1; i < sizeof(ocv_mv)/sizeof(ocv_mv[0]); i++) {
        if (mv >= ocv_mv[i]) {
            int d_v = ocv_mv[i-1] - ocv_mv[i];
            int d_s = ocv_soc[i-1] - ocv_soc[i];
            return (uint8_t)(ocv_soc[i] + (mv - ocv_mv[i]) * d_s / (d_v ? d_v : 1));
        }
    }
    return 0;
}
/* 展示值 = 平滑( 0.6*gauge + 0.4*voltage )，偏差过大或充电态时按 §6.3 的规则改走单一路径 */
```

---

## 17. 待实测/待确认清单（写死去实现前必须做的实验）

| # | 项目 | 方法 | 记录什么 |
| --- | --- | --- | --- |
| 1 | `0x27[1:0]` ONLEVEL 的 EFUSE 默认值 | 读 `0x27` | 实际默认开机时长 |
| 2 | IRQLEVEL 语义 | 按住 1 s / 1.5 s / 2 s，看 `0x49[2]`/`0x49[3]` | 长按 IRQ 到底在多久触发 |
| 3 | `0x22[1]` 是否默认使能 | 按住 6 s | 是否真断电 |
| 4 | TS 是否有 NTC | 读 `0x36/0x37` | 0.5 V / 0 V / 其他 |
| 5 | ICC 实际值 | 串电流表（USB 侧）看充电阶段 | 是否 300 mA |
| 6 | SOC 曲线质量 | 满充→放空，每 30 s 记录 | 与电压表的偏差、跳变点 |
| 7 | 板级关机电流 | 软关机后测电池端电流 | 是否 < 100 µA |
| 8 | 电压 ADC 噪声 | 静置连读 100 次 | 峰峰值（决定平均次数） |
| 9 | 电池内阻 | 静置/播放两种状态电压差 ÷ 电流 | 用于电压法负载补偿 |
| 10 | 预充定时器默认 | 读 `0x67` | 50 还是 60 min |
| 11 | **各 CV 档的实际满充点** | 分别设 4.0/4.1/4.2 V，各充满后静置 10 min 记 VBAT 与 `0xA4` | 每档对应的真实 SOC、耗时（§10.5.3 的估算表要靠它校正） |
| 12 | **`0x18[1]=0` 的行为** | 充电中清 bit1，观察 `0x01[2:0]`、`0x00[4]`、系统是否继续由 VBUS 供电；再插拔 VBUS 看该位是否保持 | 停止是否彻底、是否要软件恢复 |
| 13 | **充电中改 CV 是否安全** | CC 段把 CV 从 4.2 改成 4.1，观察是否立即终止/异常 | 决定"运行时切保养模式"能不能做 |

---

## 18. 术语

| 缩写 | 含义 |
| --- | --- |
| PMU / PMIC | 电源管理单元/芯片 |
| NVDC | Narrow Voltage DC，窄电压直流充电架构（VBUS→VSYS 走线性/BUCK，电池经 BATFET 并联） |
| BATFET | 电池与系统之间的内部 MOS（Rds(on) 50 mΩ） |
| RBFET | VBUS 与 VMID 之间的内部 MOS（100 mΩ） |
| VINDPM | 输入电压动态调节（输入能力不足时降 VBUS 目标） |
| VOFF | VSYS 欠压关机阈值（默认 2.6 V） |
| CC / CV | 恒流 / 恒压充电阶段 |
| OCV | 开路电压（Open Circuit Voltage），用于估算 SOC |
| SOC | 荷电状态（State of Charge），即电量百分比 |
| EFUSE | 一次性可编程熔丝，板厂写入的出厂默认配置 |
