# PCF85063A 实时时钟（RTC）参考文档

> 本文基于 PCF85063A datasheet（Rev. 7.3 — 14 July 2026，共 58 页，文档号 PCF85063A v7.3）整理，
> 面向"要写驱动的人"，力求可直接照抄写代码。文中 `(p.N, Table M)` / `(p.N, §x.y)` 均指该 PDF 的页脚页码与图表/节编号。
> 手册未给出、或本板未确认的地方一律显式标注 **「手册未给出」/「本板未确认」**，不做推测性补全。
>
> **本板落地状态（2026-11）**：驱动已实现于 `main/core/hardware/rtc.c`（一次 burst 读 7 字节、关闭 CLKOUT、
> 无效时用**项目诞生日**首次校时）。INT/CLKOUT 未接主控，闹钟/定时器中断未接；
> 时间显示与手动校时 UI 尚未做。实测与验证记录见 `ideas_and_exps/PMU_RTC_DRIVERS.md`。
>
> ⚠ 实测补充（手册 p.20 Figure 13）：**OS 位由硬件置位，软件只能写 0 清除，写 1 无效**。
> 想模拟"未初始化"不能用"写 OS=1"，只能靠真实断电/停振，或者把时间写成 2000-01-01（年份 < 2024，
> 走 `rtc_is_valid` 的年份下限判据）。

---

## 目录

1. 概述与关键参数
2. 引脚与封装
3. 寄存器总览与逐位表
4. 时间格式细节（BCD / 12-24h / 世纪 / 星期 / 有效性）
5. I²C 读写时序
6. 振荡器、精度与 Offset 校准
7. 中断 / 闹钟 / 定时器 / CLKOUT
8. ESP-IDF 6.1 驱动实现建议（本项目重点）
9. 踩坑与注意事项
10. 参考 C 代码
11. 附录：速查表与"手册未给出"清单

---

## 1. 概述与关键参数

PCF85063A 是 NXP 的低功耗 CMOS 实时时钟 / 日历芯片，内置 32.768 kHz 振荡器（含可选集成负载电容）与分频链，
通过两线双向 I²C 总线读写；**带偏移（Offset）寄存器可做频率微调**，另有闹钟、倒计时定时器、分钟/半小时中断、
CLKOUT 可编程方波输出、振荡停止检测（OS flag）、内部上电复位（POR）(p.1, §1/§2)。

### 1.1 关键参数速查

| 项目 | 数值 | 出处 |
|---|---|---|
| I²C 7-bit 从地址 | **`0x51`**（地址字节 `1010 001`） | p.31, §8.5.1 / Table 38 |
| I²C 写地址字节 / 读地址字节 | `0xA2` / `0xA3` | p.24, §7.4；p.31, Table 38 |
| 总线速率上限 | **400 kbit/s**（400 kHz） | p.1, §1；p.39-40, Table 41 |
| 400 kHz 所需 V<sub>DD</sub> | **1.8 V ~ 5.5 V** | p.34, Table 40 注 [2] |
| 供电范围（接口空闲） | **0.9 V ~ 5.5 V** | p.34, Table 40 |
| 绝对最大 V<sub>DD</sub> | −0.5 V ~ +6.5 V | p.33, Table 39 |
| 工作温度 | −40 ℃ ~ +85 ℃ | p.34, Table 40 |
| 静态电流 I<sub>DD</sub> | 220 nA typ / 450 nA max @ V<sub>DD</sub>=3.3 V、25 ℃、CLKOUT 关、总线空闲；85 ℃ 时 470 nA typ / 600 nA max | p.34, Table 40 |
| 通信时电流 | 18 µA typ / 50 µA max（f<sub>SCL</sub>=400 kHz、3.3 V） | p.34, Table 40 |
| 寄存器数量 | 18 个，8 bit，地址 `00h`~`11h`，地址自增、`11h` 后回绕到 `00h` | p.5, §7 / Figure 5 |
| 封装 | SO8（SOT96-1）/ DFN2626-10（SOT1197-1）/ TSSOP8（SOT505-1） | p.2, Table 1；p.43-45 |
| 封装尺寸 | SO8：body 3.9 mm；DFN2626-10：2.6 × 2.6 × 0.5 mm；TSSOP8：body 3 mm | p.2, Table 1 |
| I²C 端口耐压 | 5 V tolerant | p.35, Table 40 注 [5] |
| 晶振 | 32.768 kHz，R<sub>s</sub> 60 kΩ（手册取值条件）/ 最大 100 kΩ；集成负载电容可选 7 pF 或 12.5 pF | p.34-35, Table 40；p.14, §7.2.4 |

### 1.2 本项目中的连接（FakePodNano Pro / ESP32-S31）

引用 `ideas_and_exps/reference_docs/hardware_v1.md`：

| 项目 | 值 | 说明 |
|---|---|---|
| 总线 | **I²C0** | 与 CST820 触摸、AXP2101、QMI8658 共用 |
| SDA | **GPIO48** | 双向 |
| SCL | **GPIO47** | 输出 |
| 速率 | **400 kHz** | 与 `main/core/hardware/touch.c` 的 `i2c_new_master_bus()` 一致 |
| 从地址 | `0x51` | 开机扫描实测存在 |
| INT / CLKOUT | **未接到主控** | hardware_v1.md §9 明确"本文未提供其 INT/CLKOUT 到主控的连接" |
| 备份电源（纽扣电池/VBAT） | **本板未确认** | 手册未涉及；需按实物确认，判据见 §4.5 |

**由此得出的工程结论（重要）**：

- 本项目只能用 **轮询 I²C** 读时间；**闹钟中断、定时器中断、CLKOUT 输出在本板上都用不了**（除非飞线到某个 GPIO）。
  文档仍把 Alarm/Timer/CLKOUT 的寄存器与规则写全，一是因为寄存器复位值是绕不开的坑（§9），二是将来加飞线时可直接查。
- 驱动设计应**以"读/写时间"为核心**，alarm/timer 只保留寄存器级 API，不做中断接入。

---

## 2. 引脚与封装

引脚定义见 p.3-4, Figure 2/3/4 与 Table 3。

| 符号 | SO8（AT） | DFN2626-10（ATL） | TSSOP8（ATT） | 类型 | 说明 |
|---|---|---|---|---|---|
| OSCI | 1 | 1 | 1 | 输入 | 振荡器输入（接晶振一端） |
| OSCO | 2 | 2 | 2 | 输出 | 振荡器输出（接晶振另一端） |
| CLKOE | — | 3 | — | 输入 | CLKOUT 使能/禁止，**高有效**（仅 DFN 封装有） |
| INT | 3 | 4 | 3 | 输出 | 中断输出，**开漏**（需外部上拉） |
| V<sub>SS</sub> | 4 | 5 | 4 | 电源 | 地 |
| SDA | 5 | 6 | 5 | 输入/输出 | 串行数据线 |
| SCL | 6 | 7 | 6 | 输入 | 串行时钟输入 |
| n.c. | — | 8 | — | — | 不连接 |
| CLKOUT | 7 | 9 | 7 | 输出 | 时钟输出，**推挽** |
| V<sub>DD</sub> | 8 | 10 | 8 | 电源 | 供电 |

- 手册建议：**V<sub>DD</sub> 与所有外部上拉电阻的 V<sub>DD</sub> 接到同一路电源**（p.4, Table 3 注 [1]；p.41, Figure 31 注 2）。
- DFN2626-10 的裸露散热焊盘：经高阻（非导电）硅连接接到 V<sub>SS</sub>，**必须电气隔离**；不能在其下方走线（p.5 注 [2]）。
- 未用引脚必须接确定电平（V<sub>SS</sub> 或 V<sub>DD</sub>）（p.4, Table 3 前言）。
- 常见贴片：`PCF85063AT`（SO8）、`PCF85063ATT`（TSSOP8）。**本板实际封装未确认**，写文档/代码不依赖封装差异。

---

## 3. 寄存器总览与逐位表

### 3.1 寄存器总览（00h ~ 11h，含复位默认值）

寄存器总表见 p.5-6, Table 4；复位值见 p.10-11, Table 7。
**"读/写"一列**：手册 Table 4 未单列读写属性，下表按 §7 各节描述标注（`R/W` 可读写；`R` 只读；`W0C` 写 0 清零）。

| 地址 | 名称 | bit7 | bit6 | bit5 | bit4 | bit3 | bit2 | bit1 | bit0 | 复位值 | 属性 | 参考 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `00h` | Control_1 | EXT_TEST | — | STOP | SR | — | CIE | 12_24 | CAP_SEL | `0x00` | R/W（SR 读恒 0） | p.6-7, Table 5 |
| `01h` | Control_2 | AIE | AF | MI | HMI | TF | COF[2:0] | | | `0x00` | R/W（AF/TF 写 0 清） | p.11, Table 8 |
| `02h` | Offset | MODE | OFFSET[6:0] | | | | | | | `0x00` | R/W | p.14-15, Table 11 |
| `03h` | RAM_byte | B[7:0] | | | | | | | | `0x00` | R/W | p.19, Table 17 |
| `04h` | Seconds | OS | SECONDS（0~59，BCD） | | | | | | | **`0x80`** | R/W | p.19, Table 18 |
| `05h` | Minutes | — | MINUTES（0~59，BCD） | | | | | | | `0x00` | R/W | p.20, Table 20 |
| `06h` | Hours | — | — | AMPM（仅 12h） | HOURS（1~12 或 0~23，BCD） | | | | | `0x00` | R/W | p.20-21, Table 21 |
| `07h` | Days | — | — | DAYS（1~31，BCD） | | | | | | **`0x01`** | R/W | p.21, Table 22 |
| `08h` | Weekdays | — | — | — | — | — | WEEKDAYS[2:0]（0~6） | | | **`0x06`** | R/W | p.21, Table 23 |
| `09h` | Months | — | — | — | MONTHS（1~12，BCD） | | | | | **`0x01`** | R/W | p.22, Table 25 |
| `0Ah` | Years | YEARS（00~99，BCD） | | | | | | | | `0x00` | R/W | p.23, Table 27 |
| `0Bh` | Second_alarm | AEN_S | SECOND_ALARM（0~59，BCD） | | | | | | | `0x80` | R/W | p.24, Table 28 |
| `0Ch` | Minute_alarm | AEN_M | MINUTE_ALARM（0~59，BCD） | | | | | | | `0x80` | R/W | p.24, Table 29 |
| `0Dh` | Hour_alarm | AEN_H | — | AMPM（仅 12h） | HOUR_ALARM（BCD） | | | | | `0x80` | R/W | p.25, Table 30 |
| `0Eh` | Day_alarm | AEN_D | — | DAY_ALARM（1~31，BCD） | | | | | | `0x80` | R/W | p.25, Table 31 |
| `0Fh` | Weekday_alarm | AEN_W | — | — | — | — | WEEKDAY_ALARM[2:0]（0~6） | | | `0x80` | R/W | p.25-26, Table 32 |
| `10h` | Timer_value | T[7:0]（倒计时初值，0 = 停） | | | | | | | | `0x00` | R/W | p.27, Table 33 |
| `11h` | Timer_mode | — | — | — | TCF[1:0] | TE | TIE | TI_TP | | **`0x18`** | R/W | p.27, Table 34 |

> ⚠ 复位后 `11h = 0x18`：TCF[1:0]=`11`（定时器源 = 1/60 Hz）、TE=0（定时器关）。
> 这一点很容易被"想当然按全 0 写"的代码忽略。

### 3.2 Control_1（`00h`，p.6-7, Table 5）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7 | **EXT_TEST** | `0`（默认） | 正常模式 |
| | | `1` | 外部时钟测试模式：**CLKOUT 变成输入**，内部时钟被 CLKOUT 上外加的信号取代 |
| 6 | — | `0` | 未使用 |
| 5 | **STOP** | `0`（默认） | RTC 走时（分频链正常工作） |
| | | `1` | RTC 停止：分频链 F2~F14 被异步复位为 0，不产生 1 Hz tick；CLKOUT 上 ≤8 kHz 的输出也被停 |
| 4 | **SR** | `0`（默认） | 无软件复位 |
| | | `1` | 发起软件复位（**该位读回恒为 0**） |
| 3 | — | `0` | 未使用 |
| 2 | **CIE** | `0`（默认） | 不产生校正中断 |
| | | `1` | 每个校正周期都产生中断脉冲（配合 Offset 用，见 §6.2） |
| 1 | **12_24** | `0`（默认） | **24 小时制** |
| | | `1` | 12 小时制（AM/PM 位在 Hours 寄存器 bit5） |
| 0 | **CAP_SEL** | `0`（默认） | 集成负载电容 **7 pF** |
| | | `1` | 集成负载电容 **12.5 pF** |

补充：

- **软件复位命令 = 向 `00h` 写 `0x58`**（bit6、bit4、bit3 为 1，其余为 0），即二进制 `0101 1000`（p.7 注 [2]）。
  手册要求发完复位命令后**用 STOP + START 重新初始化接口**（p.10, Figure 8）。
- 软件复位后**所有寄存器回到 Table 7 的复位值，地址指针回到 `00h`**（p.10）。
- `STOP=1` 时 **F0、F1 两级分频器不复位**，只有 F2~F14 复位；因此释放 STOP 后第一次 +1 秒出现在
  **0.507813 s ~ 0.507935 s** 之后（p.8-9, Table 6 / Figure 7）。亚秒相位因此不可控（±0.5 s 量级），但秒增量本身正确。

### 3.3 Control_2（`01h`，p.11, Table 8）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7 | **AIE** | `0`（默认） | 闹钟中断禁止 |
| | | `1` | 闹钟中断使能（AF 有效时拉低 INT） |
| 6 | **AF** | 读 `0` | 闹钟标志未激活 |
| | | 读 `1` | 闹钟标志已激活 |
| | | **写 `0`** | **清除**闹钟标志 |
| | | **写 `1`** | 标志保持不变（不清） |
| 5 | **MI** | `0`（默认） | 分钟中断禁止 |
| | | `1` | 分钟中断使能 |
| 4 | **HMI** | `0`（默认） | 半小时中断禁止 |
| | | `1` | 半小时中断使能 |
| 3 | **TF** | `0`（默认） | 无定时器中断产生 |
| | | `1` | 定时器中断发生时由硬件置 1；**写 0 清除** |
| 2-0 | **COF[2:0]** | `000`（默认） | CLKOUT 频率选择，见 §7.4 |

> ⚠ 写标志位是"**写 0 清、写 1 不动**"，且手册明确"为防止清一个标志时误覆盖另一个标志，
> 写访问期间做逻辑与"（p.12, §7.2.2.1 的 AF 段）。**不要用 read-modify-write 去"置位"标志**，
> 那是无效的；清标志就写 0（可以整字节写 0，同时清 AF 与 TF）。

### 3.4 Offset（`02h`，p.14-15, Table 11）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7 | **MODE** | `0`（默认） | 正常模式：每 **2 小时**校正一次 |
| | | `1` | 快速（course/fast）模式：每 **4 分钟**校正一次 |
| 6-0 | **OFFSET[6:0]** | `000 0000`（默认） | 偏移量，**二进制补码**，范围 +63 ~ −64 |

- MODE=0：每个 LSB = **4.34 ppm**；MODE=1：每个 LSB = **4.069 ppm**（p.15）。
- 极值：MODE=0 时 +63 → +273.420 ppm，−64 → −277.760 ppm；MODE=1 时 +63 → +256.347 ppm，−64 → −260.416 ppm（p.15, Table 12）。
- 校正方式：**加/减时钟校正脉冲**（改变"某一秒"的周期），**不改变振荡器频率**（p.15）。
- 校正不影响 32.768 k/16.384 k/8.192 k/4.096 k/2.048 k/1.024 k 这些 CLKOUT 频率，但**影响 1 Hz 的 CLKOUT**、
  以及定时器源 ≤1 Hz 与 1/60 Hz（MODE=0，见 p.16, Table 14）；MODE=1 时 ≤1.024 kHz 的定时器源都会受影响（p.17, Table 16）。

### 3.5 RAM_byte（`03h`，p.19, Table 17）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-0 | **B[7:0]** | `0000 0000`（默认） | 自由 RAM 字节，用途自定（例如存"时间已校准"标记） |

> 这是芯片里**唯一**不会被 SR 之外逻辑改写的存储位。若想做一个"软标记"区分"从未校时"，可以用它
> （但硬上电复位后它同样回 0，与 OS 位信息冗余，本项目更推荐用 OS 位 + 合理性校验，见 §4.5）。

### 3.6 时间/日期寄存器（`04h`~`0Ah`）

#### Seconds（`04h`，p.19, Table 18）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7 | **OS** | `0` | 时钟完整性保证（振荡器正常） |
| | | `1`（**默认**） | **时钟完整性无保证**：振荡器停止过或被中断 |
| 6-4 | SECONDS | `000`~`101`（BCD 十位） | 秒十位 0~5 |
| 3-0 | SECONDS | `0000`~`1001`（BCD 个位） | 秒个位 0~9 |

- 复位值 `0x80` → 秒 = 00，且 **OS = 1（时间无效）**。
- OS 位**只能由命令（写 0）清除**；若写了 0 之后仍读回 1，说明振荡器没起振（p.20, §7.3.1.1 / Figure 13）。
- 上电到晶体稳定之间（**200 ms ~ 2 s**，取决于晶振类型、温度、供电）会置 OS（p.20）。

#### Minutes（`05h`，p.20, Table 20）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7 | — | `0` | 未使用 |
| 6-4 | MINUTES | 0~5 | 十位（BCD） |
| 3-0 | MINUTES | 0~9 | 个位（BCD） |

#### Hours（`06h`，p.20-21, Table 21）

**12 小时制**（`12_24 = 1`）：

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-6 | — | `00` | 未使用 |
| 5 | **AMPM** | `0`（默认） | **AM** |
| | | `1` | **PM** |
| 4 | HOURS | 0~1 | 十位（BCD） |
| 3-0 | HOURS | 0~9 | 个位（BCD） |

**24 小时制**（`12_24 = 0`，默认）：

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-6 | — | `00` | 未使用 |
| 5-4 | HOURS | 0~2 | 十位（BCD） |
| 3-0 | HOURS | 0~9 | 个位（BCD） |

> ⚠ 手册只说"12 小时制下 HOURS 为 1~12"，**未给出 12 AM / 12 PM 与 0 / 12 的换算约定**（手册未给出）。
> 本项目固定用 24 小时制（默认值），不要在驱动里引入 12 小时制分支。

#### Days（`07h`，p.21, Table 22）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-6 | — | `00` | 未使用 |
| 5-4 | DAYS | 0~3 | 十位（BCD） |
| 3-0 | DAYS | 0~9（默认 `1`） | 个位（BCD），即 1~31 |

#### Weekdays（`08h`，p.21, Table 23 / Table 24）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-3 | — | `0000 0` | 未使用 |
| 2-0 | **WEEKDAYS** | `0`~`6` | 星期，见下表（**非 BCD，是 3 bit 二进制**） |

| 值 | 星期 | 值 | 星期 |
|---|---|---|---|
| `0` | **Sunday** | `4` | Thursday |
| `1` | Monday | `5` | Friday |
| `2` | Tuesday | `6` | **Saturday（复位默认值）** |
| `3` | Wednesday | | |

> 手册注：**这个对应关系可以由用户重新定义**（p.22, Table 24 注 [1]）。本项目按 0=周日 使用，
> 与 Linux `struct tm` 的 `tm_wday`（0 = Sunday）**天然一致**，不需要换算。

#### Months（`09h`，p.22, Table 25）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-5 | — | `000` | 未使用 |
| 4 | MONTHS | 0~1 | 十位（BCD） |
| 3-0 | MONTHS | 1~9 | 个位（BCD），即 1~12 |

#### Years（`0Ah`，p.23, Table 27）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-4 | YEARS | 0~9 | 十位（BCD） |
| 3-0 | YEARS | 0~9 | 个位（BCD） |

### 3.7 Alarm 寄存器（`0Bh`~`0Fh`）

**所有闹钟寄存器 bit7 都是"使能位"，且是低有效：`0` = 使能（参与比较），`1` = 禁止（忽略）**（p.24-26）。
复位值全为 `0x80`，即**复位后所有闹钟都处于"禁止"状态**。

| 地址 | 寄存器 | bit7 | bit6~bit0 布局 | 复位值 |
|---|---|---|---|---|
| `0Bh` | Second_alarm | **AEN_S**（`0`=使能，`1`=禁止） | bit6-4 秒十位（BCD 0~5）、bit3-0 秒个位（BCD 0~9） | `0x80` |
| `0Ch` | Minute_alarm | **AEN_M**（`0`=使能，`1`=禁止） | bit6-4 分十位（BCD 0~5）、bit3-0 分个位（BCD 0~9） | `0x80` |
| `0Dh` | Hour_alarm | **AEN_H**（`0`=使能，`1`=禁止） | bit6 未使用（0）；**24h**：bit5-4 十位（0~2）、bit3-0 个位；**12h**：bit5 = AMPM、bit4 十位（0~1）、bit3-0 个位 | `0x80` |
| `0Eh` | Day_alarm | **AEN_D**（`0`=使能，`1`=禁止） | bit6 未使用（0）；bit5-4 十位（0~3）、bit3-0 个位（BCD 1~31） | `0x80` |
| `0Fh` | Weekday_alarm | **AEN_W**（`0`=使能，`1`=禁止） | bit6-3 未使用；bit2-0 星期（0~6，0=周日） | `0x80` |

逐条（原文数据）：

| bit | 寄存器 | 值 | 含义 |
|---|---|---|---|
| 7 | Second_alarm `0Bh` | `0` | Second alarm **Enabled** |
| | | `1`（默认） | Disabled |
| 6-4 | Second_alarm | 0~5 | 秒闹钟十位（BCD） |
| 3-0 | Second_alarm | 0~9 | 秒闹钟个位（BCD） |
| 7 | Minute_alarm `0Ch` | `0` | Enabled |
| | | `1`（默认） | Disabled |
| 6-4 / 3-0 | Minute_alarm | 0~5 / 0~9 | 分闹钟（BCD） |
| 7 | Hour_alarm `0Dh` | `0` | Enabled |
| | | `1`（默认） | Disabled |
| 6 | Hour_alarm | `0` | 未使用 |
| 5 | Hour_alarm（仅 12h） | `0`=AM / `1`=PM | AM/PM |
| 4 / 3-0 | Hour_alarm（12h） | 0~1 / 0~9 | 小时闹钟（BCD） |
| 5-4 / 3-0 | Hour_alarm（24h） | 0~2 / 0~9 | 小时闹钟（BCD） |
| 7 | Day_alarm `0Eh` | `0` | Enabled |
| | | `1`（默认） | Disabled |
| 6 | Day_alarm | `0` | 未使用 |
| 5-4 / 3-0 | Day_alarm | 0~3 / 0~9 | 日闹钟（BCD） |
| 7 | Weekday_alarm `0Fh` | `0` | Enabled |
| | | `1`（默认） | Disabled |
| 6-3 | Weekday_alarm | `0` | 未使用 |
| 2-0 | Weekday_alarm | 0~6 | 星期闹钟（手册写 "coded in BCD format"，但取值 0~6，位型与二进制相同） |

### 3.8 定时器寄存器（`10h`/`11h`）

#### Timer_value（`10h`，p.27, Table 33）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-0 | **T[7:0]** | `00h`（默认）~`FFh` | 倒计时值（**二进制，不是 BCD**） |

倒计时周期：`CountdownPeriod = T / SourceClockFrequency`（p.27 注 [2]）。
**写 0 会停止定时器**；有效值 1~255（p.28）。

#### Timer_mode（`11h`，p.27, Table 34）

| bit | 符号 | 值 | 含义 |
|---|---|---|---|
| 7-5 | — | `000` | 未使用 |
| 4-3 | **TCF[1:0]** | `00` | 定时器源时钟 **4.096 kHz** |
| | | `01` | **64 Hz** |
| | | `10` | **1 Hz** |
| | | `11`（**默认**） | **1/60 Hz** |
| 2 | **TE** | `0`（默认） | 定时器关闭 |
| | | `1` | 定时器使能 |
| 1 | **TIE** | `0`（默认） | 不产生定时器中断 |
| | | `1` | 定时器中断使能 |
| 0 | **TI_TP** | `0`（默认） | 中断跟随 TF 标志（电平型） |
| | | `1` | 中断产生脉冲（脉冲型） |

> 手册建议：不用定时器时 **TCF[1:0] 置 `11`（1/60 Hz）以省电**（p.28, Table 35 注 [1]）。这正是复位默认值。

---

## 4. 时间格式细节（BCD / 12-24h / 世纪 / 星期 / 有效性）

### 4.1 BCD 编码

- **秒、分、时、日、月、年、以及对应的 alarm 寄存器全部是 BCD**（p.5, §7）。
- **星期寄存器不是 BCD**，是 3 bit 二进制（0~6），只不过 0~6 的位型与 BCD 相同（p.21, Table 23/24）。
- **Timer_value 是二进制**，不是 BCD（p.27, Table 33）。
- 高位（十位）字段有效范围各不相同：秒/分 0~5，时（24h）0~2，时（12h）0~1，日 0~3，月 0~1，年 0~9。
  **写之前必须做范围检查**，写超范围的值芯片行为未定义（手册未给出）。

### 4.2 12/24 小时制

- 由 `Control_1` 的 `12_24`（bit1）决定：`0` = 24h（**默认**），`1` = 12h（p.7, Table 5）。
- 12 小时制下 AM/PM 在 `Hours` 的 **bit5**（AMPM）：`0` = AM，`1` = PM（p.20-21, Table 21）；
  `Hour_alarm` 同样如此（p.25, Table 30）。
- 12 小时制下 Hours 的十位只有 bit4（0~1），24 小时制下十位是 bit5-4（0~2）。**同一个字节两种解释，切换后必须重写时间**。
- 本项目**只用 24 小时制**（默认值，不要改）。

### 4.3 世纪位与年份范围

- **芯片没有世纪（century）位**：`Years` 只有 00~99 的 BCD 两位（p.23, Table 27）。
- 手册通过"复位值 + 闰年规则"隐含 2000~2099 的映射：复位后日期为 **01 January 2000**（p.11），
  闰年判定规则是"**年计数器数值能被 4 整除（含 00）就补 29 日**"（p.21, Table 22 注 [1]）。
  即芯片把 `00` 当闰年 → **`00` 对应 2000，年份 2000~2099**。
- 因此驱动里统一按 `公元年 = 2000 + BCD 值` 处理；**不要试图支持 1900 年代**（手册未给出任何世纪标志）。

### 4.4 星期取值

见 §3.6 的 Weekdays 表：**0 = Sunday，1 = Monday，…，6 = Saturday，复位默认 6（Saturday）**。
与 Linux `struct tm.tm_wday`（0 = Sunday）一致，直接赋值即可，不需要 ±1。

### 4.5 无效 / 未初始化时间的判断

这是驱动里最容易漏的部分。可用的判据有三层：

1. **OS 位（Seconds 的 bit7）** —— 最权威的"时间可能无效"标志。
   - 复位默认 `1`；振荡器停过/被中断会置 `1`；只能软件写 0 清除（p.19-20）。
   - **注意**：OS=1 只说明"振荡器出过问题"，不代表寄存器内容一定乱；OS=0 也不保证内容被正确设置过。
2. **寄存器复位默认组合** —— 若读到"接近出厂默认"，说明很可能从未校时：
   `04h=0x80`、`05h=0x00`、`06h=0x00`、`07h=0x01`、`08h=0x06`、`09h=0x01`、`0Ah=0x00`
   → 即 **00:00:00，2000-01-01，Saturday**（p.10-11, Table 7 与其下的三条 "resets to"）。
   > ⚠ 注意：**时间寄存器的复位值并不是全 0x00**。只有 `Control_1/2`、`Offset`、`RAM_byte`、`Minutes`、`Hours`、
   > `Years`、`Timer_value` 这些是 `0x00`；`Seconds=0x80`、`Days=0x01`、`Weekdays=0x06`、`Months=0x01`、
   > `Timer_mode=0x18`、以及 5 个 alarm 寄存器 `=0x80`。
3. **合理性（sanity）校验** —— 纯软件层兜底，防止 BCD 位型非法或跑飞：
   `month ∈ 1..12`、`day ∈ 1..当月天数`、`hour ≤ 23`、`minute ≤ 59`、`second ≤ 59`、
   各 BCD 半字节 ≤ 9、`weekday ≤ 6`、`year ∈ 0..99`。

**推荐判定策略（本项目）**：

```
rtc_is_valid() =
       (OS == 0)                 // 振荡器没停过
    && sanity_ok()               // 位型/范围合法
    && (year >= 2024)            // 排除"2000-01-01 00:00:00"这类出厂默认
```

> 最后一条是工程折衷：芯片本身**无法区分**"2000-01-01 00:00:00 是真实时间还是出厂默认"（手册未提供出厂标记）。
> 用"年份下限"来判，代价是机器真的回拨到 2024 年之前会误判；对本项目（2026 年的产品）可接受。
> 更严谨的做法：校时成功后把某个魔数写进 `RAM_byte（03h）`，读取时校验魔数——但注意 **RAM_byte 也会被 SR 复位清 0**。

**关于备份电源（重要，本板未确认）**：
- 芯片本身支持 0.9 V~5.5 V 供电；只要 V<sub>DD</sub> 不掉到 0，走时与寄存器内容保持。
- 手册应用示例给的是"1 F 超级电容 + 低 V<sub>F</sub> 二极管"做备用电源（p.41, Figure 31）。
- 若 V<sub>DD</sub> 真的掉到 0，上电后 `Seconds` 的 OS 会置 1，**时间回到 2000-01-01**，必须重新校时。
- 手册警告：**回流焊时不要给芯片加电**（例如板上已焊纽扣电池），必须能保证一次完整的 "0 V" 断电（`Qreset`）才能
  让器件重新使能（p.34, Table 39 注 [1]）。这与"首次上电"行为直接相关。

---

## 5. I²C 读写时序

### 5.1 从地址与 R/W

- 7-bit 地址 `1010 001` = **0x51**（p.31, Table 38）。
- 写：地址字节 `0xA2`；读：地址字节 `0xA3`（p.24/§7.4、p.31）。
- 寄存器地址是 **5 bit**（`00h`~`11h`），高 3 bit 不使用（p.32, §8.5.2）。
- **地址自增**：每读/写一个数据字节后地址 +1，**`11h` 之后回绕到 `00h`**（p.5, Figure 5）。

### 5.2 写寄存器（单次写）

```
START | 0xA2 (地址+W) | ACK | 寄存器指针 (00h~11h) | ACK | data0 | ACK | ... | dataN | ACK | STOP
```
见 p.32, Figure 22 (WRITE mode)。地址自增，可一次连写多个寄存器。

### 5.3 读寄存器（写指针 + repeated START）

```
START | 0xA2 (W) | ACK | 寄存器指针 | ACK |  [Sr 或 STOP+START]
      | 0xA3 (R) | ACK | data0 | ACK(主) | ... | dataN | NACK | STOP
```
见 p.32, Figure 23。手册原话：**多主机场景或为了更快，可用 repeated START (Sr) 替代 STOP+START**。
主机在最后一个字节回 **NACK**，然后发 STOP。

用 ESP-IDF 新 I²C 驱动时，这一整套 = **一次 `i2c_master_transmit_receive()`**（见 §10 代码）。

### 5.4 连续读与"一次读完 7 字节"

- 手册 §7.4 明确给出**推荐读时间流程**（p.24）：写指针到 `04h`，然后 repeated START + 读，依次读
  Seconds / Minutes / Hours / Days / Weekdays / Months / Years，共 **7 字节**，最后 STOP。
- **必须一次读完**（一次事务内），原因见 §5.5。

### 5.5 读写期间的时间冻结与"1 秒窗口"（关键）

p.23, §7.4：

- **读/写 `04h`~`0Ah` 期间，时间计数电路被阻塞（blocked）**。目的：
  1. 防止在进位瞬间读到"错的时钟/日历"；
  2. 防止读周期内时间寄存器被自增。
- 访问结束后时间电路释放；期间积压的"自增请求"**最多只会被服务 1 次**，所以
  **所有访问必须在一个事务里、且在 1 秒内完成**（Figure 15 明确画出 `t < 1 s`）。
- 手册给的反例：如果一次写"秒~时"、另一次写"日~年"，两次之间时间可能进位 → 时间被写坏。
  **设置时间必须一次事务写 7 字节。**

### 5.6 交流时序参数（p.39-40, Table 41）

V<sub>DD</sub> = 1.8~5.5 V、T<sub>amb</sub> = −40~+85 ℃、f<sub>osc</sub> = 32.768 kHz、R<sub>s</sub> = 60 kΩ、C<sub>L</sub> = 7 pF 条件下的测量值：

| 符号 | 参数 | 最小 | 最大 | 单位 |
|---|---|---|---|---|
| C<sub>b</sub> | 每条总线的容性负载 | — | 400 | pF |
| **f<sub>SCL</sub>** | SCL 时钟频率 | **0** | **400** | kHz |
| t<sub>HD;STA</sub> | (重复) START 保持时间 | 0.6 | — | µs |
| t<sub>SU;STA</sub> | 重复 START 建立时间 | 0.6 | — | µs |
| t<sub>LOW</sub> | SCL 低电平周期 | 1.3 | — | µs |
| t<sub>HIGH</sub> | SCL 高电平周期 | 0.6 | — | µs |
| t<sub>r</sub> | SDA/SCL 上升时间 | 20 | 300 | ns |
| t<sub>f</sub> | SDA/SCL 下降时间 | 20 × (V<sub>DD</sub>/5.5) | 300 | ns |
| **t<sub>BUF</sub>** | **STOP 到下一个 START 的总线空闲时间** | **1.3** | — | µs |
| t<sub>SU;DAT</sub> | 数据建立时间 | 100 | — | ns |
| t<sub>HD;DAT</sub> | 数据保持时间 | 0 | — | ns |
| t<sub>SU;STO</sub> | STOP 建立时间 | 0.6 | — | µs |
| t<sub>VD;DAT</sub> | 数据有效时间 | 0 | 0.9 | µs |
| t<sub>VD;ACK</sub> | ACK 有效时间 | 0 | 0.9 | µs |
| t<sub>SP</sub> | 输入滤波器应抑制的尖峰脉宽 | 0 | 50 | ns |

手册补充（Table 41 注）：

- [1] 详细 I²C 规范见 UM10204。
- [2] **两次 START 之间的总线访问时间必须小于 1 秒**（对应 §5.5 的窗口）。
- [3] 器件内部对 SDA 至少提供 300 ns 的保持时间，以跨越 SCL 下降沿的不确定区。

### 5.7 写后要等吗？—— 不需要

- 这是 **SRAM 型寄存器 RTC，没有 EEPROM 写周期**，写完立即生效，**不需要 busy-wait / 轮询 ACK**。
- 唯一需要满足的是 I²C 的 **t<sub>BUF</sub> ≥ 1.3 µs**（STOP 到下一个 START 的空闲）。
  这个由 ESP-IDF 的 I²C 驱动在每次事务间保证，**应用层不需要 `vTaskDelay`**。
- 唯一的"时间语义"等待：设置时间时若用了 STOP 位，**释放 STOP 后约 0.5078 s 才发生第一次 +1 秒**（见 §3.2）。

### 5.8 上电流程与总线恢复（p.42, §13.1；p.32, §8.5.2.1）

手册推荐的上电流程：

1. V<sub>DD</sub> 在数十 µs 内**单调**爬升到目标值；爬升过慢或非单调会导致 POR 失败 → I²C 卡死和/或寄存器值错误。
2. 如果看到 SDA 被拉死在低电平，实现**总线清除**：**盲目发 9 个 SCL 时钟**（"stuck bus" 恢复）。
3. 通信正常建立后，**置 `Control_1.SR` 做一次软件复位**，保证所有寄存器是复位默认值。
   > ⚠⚠ **本项目绝对不能无条件做这一步**：SR 会把时间寄存器清成 2000-01-01，**备用电池保住的走时会被抹掉**。
   只有在"确定时间无效（OS=1 且日期是出厂默认）"时才考虑复位，见 §8.5。
4. 若要做断电重启（power cycle），V<sub>DD</sub> 必须到 0 V 并保持 > 100 ms，再按第 1 步上电。

---

## 6. 振荡器、精度与 Offset 校准

### 6.1 晶振要求

手册给出的晶振相关参数（p.34-35, Table 40；p.41, §13）：

| 参数 | 数值 | 说明 |
|---|---|---|
| 频率 | 32.768 kHz | — |
| 串联电阻 R<sub>s</sub>（ESR） | 典型 **60 kΩ**（手册所有电气参数在此条件下测得）；**最大 100 kΩ** | 用 70 kΩ 晶振时功耗增加几 nA、起振稍慢（p.41） |
| 集成负载电容 C<sub>L(itg)</sub> | `CAP_SEL=0`：**4.2 / 7 / 9.8 pF**（min/typ/max）<br>`CAP_SEL=1`：**7.5 / 12.5 / 17.5 pF** | 由 `Control_1` bit0 选择（p.14, §7.2.4） |
| C<sub>L(itg)</sub> 定义 | C<sub>L(itg)</sub> = C<sub>OSCI</sub>·C<sub>OSCO</sub> / (C<sub>OSCI</sub> + C<sub>OSCO</sub>) | 即两脚电容串联值（p.35 注 [7]） |
| 起振时间 | 上电后 200 ms ~ 2 s（取决于晶振类型、温度、供电） | 期间 OS 位置 1（p.20） |
| 可靠起振的 V<sub>DD</sub> | **> 1.2 V**；0.9 V 也能起振但可能很慢（高温尤甚） | p.35, Table 40 注 [1] |
| 振荡器频率随 V<sub>DD</sub> 变化 | **0.075 ppm typ**（ΔV<sub>DD</sub> = 200 mV，T<sub>amb</sub> = 25 ℃） | p.35, Table 40；曲线见 Figure 29 (p.39) |

> **晶振选型要点**：C<sub>L</sub> 必须与 `CAP_SEL` 匹配（板子上若已有外部电容，就按实际 C<sub>L</sub> 选 `CAP_SEL`；
> 手册的设计意图是"外接 32.768 kHz 晶振 + 片内可选电容"，见 Figure 31 应用图）。
> 本项目晶振型号/负载电容**本板未确认**，写驱动时可保守地**不动 `CAP_SEL`**（保持默认 7 pF）。

### 6.2 Offset 校准怎么用

1. 用 CLKOUT 输出 32.768 kHz，测实际频率 f<sub>meas</sub>（手册 Figure 11 的流程，p.18）：
   ```
   t_meas = 1 / f_meas
   D_meas = 1/32768 − t_meas
   E_ppm  = 1 000 000 × D_meas / t_meas
   Offset = E_ppm / 4.34     (MODE = 0)
   Offset = E_ppm / 4.069    (MODE = 1)
   ```
   手册的算例：f<sub>meas</sub> = 32768.48 Hz → 14.648 ppm → MODE=0 需 3.375 → **写 3**（3 个校正脉冲）；
   MODE=1 需 3.600 → **写 4**。
2. **正 ppm 表示"实测频率偏高、走时偏快"，需要用负的 OFFSET 值去补偿**（OFFSET 为补码，按表取 −N）。
   手册 Figure 12 的示例：测得 +14.648 ppm，MODE=0 写 3 个校正脉冲后残余 −1.628 ppm；MODE=1 写 4 个后残余 −1.276 ppm
   （注意这两个示例的符号方向——按手册算例，写正值得到的是"扣除"效果；实际标定时请以残余偏差的符号做一次闭环验证，**手册未给出正负号的文字约定**，属于"手册表述不清"的地方）。
3. **校正精度**：偏移校准可达到 **±2 ppm**（0.5 × 每个 LSB）（p.18）。
4. **±1 ppm ≈ 每天 0.0864 秒**（p.18）。所以 ±2 ppm ≈ 每月约 10 秒量级。
5. `MODE=0`（默认）省电（每 2 小时校正一次），`MODE=1` 每 4 分钟校正一次、精度更好但**功耗更高**（p.16）。

> 本项目**没有把 CLKOUT 引到主控**，无法测频 → **offset 校准在本板上无法做**（除非飞线到 GPIO 用 PCNT 测频）。
> 因此驱动只保留寄存器级 API，不实现自动校准。

### 6.3 温度与电压对精度的影响

- **电压**：手册给出 Δf/f 随 V<sub>DD</sub> 的曲线（p.39, Figure 29）：1 V→5 V 范围内约 ±1.5 ppm 量级；
  表内给出 0.075 ppm typ / 200 mV。
- **温度**：**手册未给出 Δf/f 随温度变化的曲线**（只有 I<sub>DD</sub> 随温度的图，p.37, Figure 26）。
  32.768 kHz 音叉晶振的温度特性（典型抛物线系数约 −0.03 ppm/℃²）**属于晶振规格书内容，不来自本手册**。
  工程上的结论：整机精度主要由**所选晶振 + 焊盘寄生 + 温度**决定，芯片只提供 ±2 ppm 级的微调手段。
- 手册明确提醒：**所有基于 32.768 kHz 的定时（包括 Timer"秒"）都假设 0 ppm 偏差**，频率偏了时间就偏（p.28）。

### 6.4 典型精度

- **手册未给出"整机典型精度 ppm"这一指标**（数据手册只保证电气参数与 ±2 ppm 的校准能力）。
- 可用的定量锚点只有两条：①偏移校准可达 **±2 ppm**（p.18）；②**±1 ppm = 0.0864 s/天**（p.18）。
- 实际精度必须实测（参见 §6.2，本板需飞线 CLKOUT 才能测）。

---

## 7. 中断 / 闹钟 / 定时器 / CLKOUT

### 7.1 中断结构与电气

- `INT` 是**开漏输出**，需外部上拉（p.4, Table 3）。
- 中断源汇总到 Figure 9（p.12）：`AF & AIE`、`TF & TIE`、`(MI|HMI) & 对应使能`、`CIE & 校正脉冲`。
- 中断脉冲宽度：MODE=0 时 **1/64 s**；MODE=1 的校正中断为 **1/1024 s**（多次校正脉冲间隔 1/512 s）（p.13, p.17）。
- 中断输出极性：`INT` 低有效（开漏拉低）。引脚表中的 `CLKOE` 是"高有效"（那是 CLKOUT 使能，不是 INT）。
- 清标志：**写 0** 到 `Control_2` 的 AF / TF（见 §3.3）。

### 7.2 闹钟（Alarm）

**规则（p.26, §7.5.6）**：

1. 把 `AEN_x` 清 0 = 该字段参与比较；置 1 = 忽略。
2. **所有"已使能"的比较项之间是逻辑与**：分、时、日、星期必须**同时**匹配才置 AF。
3. **只有"时间自增到匹配"的那一刻**才置 AF（写 alarm 寄存器时时间已经相等不会立刻触发）。
4. AF 置位后保持，直到命令清除；清除后要等下一次匹配才会再置位。
5. AIE=1 时 `INT` 跟随 AF（电平）；AIE=0 时只置标志不输出中断。

**常用配置**：

| 目标 | 配置 |
|---|---|
| 每天 HH:MM:SS | AEN_S/AEN_M/AEN_H = 0；**AEN_D = 1、AEN_W = 1**（关键！否则日/星期也要匹配） |
| 每周 X 日 HH:MM | AEN_W = 0，`WEEKDAY_ALARM = X`；AEN_D = 1；时/分使能，秒可禁用（AEN_S=1） |
| 每月 D 日 HH:MM | AEN_D = 0，`DAY_ALARM = D`；AEN_W = 1 |
| 每分/每 30 秒 | **不要用 alarm**，用 `MI`/`HMI`（见 §7.5） |

> ⚠ **日与星期是"与"不是"或"**：这是最容易踩的坑之一——只想每天闹，却把 Day_alarm 的 bit7 留成了 0
> （复位是 0x80 = 禁止，看起来"安全"，但一旦你为了设日期而写了 `0x0D` 这种值就使能了 Day 比较），
> 结果闹钟只在"那一天的星期也对上"时才响，表现为"闹钟每月只响一次"。

### 7.3 定时器（Timer）

- 源时钟：4.096 kHz / 64 Hz / 1 Hz / 1/60 Hz（`TCF[1:0]`）。
- 周期：`T / f_source`，T = 1~255（写 0 = 停止）。
- 范围：**244 µs**（4.096 kHz，T=1）~ **4 小时 15 分**（1/60 Hz，T=255）。需要更长的周期用 alarm（p.27）。
- 行为：计数到 1→0 时置 **TF**，计数器自动重装 T 并开始下一周期（p.28, Figure 17）。
- **第一次周期的误差**：使能命令与定时器源时钟异步，首周期在 `T−1 ~ T+1` 之间（4.096 k/64 Hz），
  1 Hz 与 1/60 Hz 时为 `(T−1)+1/64 ~ T+1/64`（p.29, Table 36）。
- **改 T 之前必须先清 TE=0**：否则新值可能在计数器重装时写入，造成首个周期不确定（p.28 明确"不推荐"）。
- **读 `10h` 返回的是当前计数值，不是初值 T**；且读的时候无法冻结计数器，手册建议**连读两次取一致值**（p.29）。
- MI/HMI 的定时精度受 Offset 影响：**只有 `OFFSET[6:0] = 00h` 时周期才是标称值**（p.13）。
- MODE=0 时，Timer 的 1 Hz 与 1/60 Hz 源会被校正脉冲影响（p.16, Table 14）。

### 7.4 CLKOUT 频率选项（p.14, Table 10）

| COF[2:0] | CLKOUT 频率 | 典型占空比 | STOP=1 的影响 |
|---|---|---|---|
| `000`（**默认**） | **32 768 Hz** | 60:40 ~ 40:60 | 无影响 |
| `001` | 16 384 Hz | 50:50 | 无影响 |
| `010` | 8 192 Hz | 50:50 | 无影响 |
| `011` | 4 096 Hz | 50:50 | CLKOUT = LOW |
| `100` | 2 048 Hz | 50:50 | CLKOUT = LOW |
| `101` | 1 024 Hz | 50:50 | CLKOUT = LOW |
| `110` | 1 Hz | 50:50 | CLKOUT = LOW |
| `111` | **CLKOUT = LOW（关闭）** | — | — |

关键事实：

- **CLKOUT 是推挽输出，且上电后默认是使能的**（p.14）；结合 COF 默认 `000`，
  **复位后芯片会一直在 CLKOUT 脚上输出 32.768 kHz 方波**。
- 关掉它：`COF[2:0] = 111`；DFN 封装还可以拉低 `CLKOE`（拉低即禁用，禁用后 CLKOUT 为 LOW）。
- 除 32.768 kHz 外，其它频率占空比都是 50:50；32.768 kHz 的占空比由振荡器本身决定，无法从外部影响。
- 手册标注的 220 nA 静态电流是 **CLKOUT disabled** 条件下测的（p.34, Table 40）。
  **复位默认状态（CLKOUT 开、32.768 kHz）功耗会更高** → 驱动初始化时应主动关掉 CLKOUT（本项目 CLKOUT 悬空）。

### 7.5 分钟 / 半小时中断（MI / HMI）

- 与秒计数器同步运行的预定义定时器（p.13）。
- **只能在 `MODE = 0`（正常偏移模式）下使用**（p.13 明确）。
- 组合效果（Table 9）：

| MI | HMI | 结果 |
|---|---|---|
| 0 | 0 | 无中断 |
| 1 | 0 | 每 1 分钟一次 |
| 0 | 1 | 每 30 秒一次 |
| 1 | 1 | 每 30 秒一次 |

- 首次中断延迟：MI 在启动后 1~59 s；HMI 在启动后 1~29 s（之后才周期化）（p.13）。

### 7.6 如何清中断标志（汇总）

| 标志 | 位置 | 清除方式 | 备注 |
|---|---|---|---|
| AF（闹钟） | `Control_2` bit6 | **写 0** | 写 1 无效（保持不变） |
| TF（定时器） | `Control_2` bit3 | **写 0** | 写 1 无效 |
| OS（振荡停止） | `Seconds` bit7 | **写 0** | 若清不掉 = 振荡器没起振 |
| （MI/HMI 无标志位） | — | 靠中断脉冲本身 | 只有 TF 能间接反映 |

> 实操：读 `Control_2` → 判断 AF/TF → **写回一个把该位清 0 的字节**（其余位按需保留）。
> 因为"写 1 保持"，所以可以直接 `ctrl2 & ~(AF)` 写回，不需要特殊时序。

---

## 8. ESP-IDF 6.1 驱动实现建议（本项目重点）

### 8.1 用新 I²C master 驱动，不要用 legacy

**结论：用 `driver/i2c_master.h`（新驱动），禁止 legacy `driver/i2c.h`。**

理由（都是本项目已有的既成事实，见 `main/core/hardware/touch.c` 头部注释）：

- 项目在 IDF 6.1 迁移时已经把触摸改成新驱动：`i2c_new_master_bus()` 建**总线句柄**，
  `i2c_master_bus_add_device()` 为每个从设备建**设备句柄**（地址绑在设备句柄上）。
- `esp_lcd_new_panel_io_i2c()` 在 6.1 只接受 `i2c_master_bus_handle_t`，**同一端口不可能新旧混用**。
- 新驱动自带 `i2c_master_probe()`（发地址看 ACK），比手搓 `i2c_cmd_link` 简单。
- 新驱动的 API 天然支持"写指针 + repeated START 读"的原子事务：`i2c_master_transmit_receive()`。

### 8.2 总线共享：复用同一个 bus handle（最容易炸的地方）

现状（务必先读代码）：

| 事实 | 位置 |
|---|---|
| I²C0 总线由**触摸驱动**创建：`i2c_new_master_bus()`，SDA=48、SCL=47、`glitch_ignore_cnt=7`、内部上拉开、**总线速率 400 kHz** | `main/core/hardware/touch.c` `touch_init()` |
| 触摸驱动**导出了总线句柄**：`i2c_master_bus_handle_t touch_get_i2c_bus(void)` | `main/core/hardware/touch.c` |
| 总线创建是**同步**的（`touch_init()` 返回时 bus 已可用）；只有触摸控制器本身是异步初始化 + 扫描 | `touch_init()` |

因此 RTC 驱动必须这样接：

```c
// rtc_init() 里：
i2c_master_bus_handle_t bus = touch_get_i2c_bus();   // 复用，绝不能再 i2c_new_master_bus()
if (bus == NULL) { /* 触摸还没初始化，推迟/报错，不要自己建总线 */ }

const i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x51,          // PCF85063A
    .scl_speed_hz    = 400 * 1000,    // 必须 > 0，且与总线一致
};
i2c_master_bus_add_device(bus, &dev_cfg, &s_rtc_dev);
```

**三个必须遵守的点**：

1. **不要重复 `i2c_new_master_bus(I2C_NUM_0, ...)`**：同一端口已被占用，会返回错误；
   若强行"先 deinit 触摸总线的再建"会把触摸一起搞死。
2. **`scl_speed_hz` 必须显式填且 > 0**。IDF 6.1 的 `i2c_master_bus_add_device()` 有硬校验
   （`invalid scl frequency`），这一条在触摸迁移时已经踩过一次（见 touch.c 注释）。
3. **事务是总线级串行的**，不需要自己加互斥锁：`i2c_master.c` 内部用 `bus_lock_mux`
   在每次 `i2c_master_transmit/receive/transmit_receive/probe` 期间持有总线（`i2c_master_transaction_start()`
   里 `xSemaphoreTake(i2c_dev->master_bus->bus_lock_mux, ...)`）。
   但**跨多次 API 调用的"读-改-写"不是原子的**，所以读时间一定要用**一次 `transmit_receive`**读完 7 字节。

**总线负载评估**（本项目）：触摸 indev 每 33 ms 轮询一次，RTC 每 1 s 读 7 字节（≈ 8 字节 × 9 bit /400 kHz ≈ 180 µs），
占用率可忽略；RTC 事务最坏只需要等触摸一次事务（几百 µs）完成，不会引起可感知的 UI 卡顿。

### 8.3 建议的文件位置与模块边界

```
main/core/hardware/rtc.h        # 对外 API + rtc_time_t
main/core/hardware/rtc.c        # I²C 读写、BCD 转换、有效性判定、tm 互转
```

- 放 `core/hardware/`：与 `display/touch/battery/sd_card` 同级，符合项目"与应用无关的底层驱动"分层。
- **必须**把 `rtc.c` 加进 `main/CMakeLists.txt` 的 `MAIN_SRCS`（本项目新增源文件的固定要求）。
- **不需要**新增 Kconfig；唯一可能的选项是"状态栏是否显示 RTC 时间"，可以先不做，或复用 debug/release 两套配置。
- 不建任务：读时间由 UI 侧 `lv_timer` 驱动（1 Hz），驱动本身无后台任务、无中断。省内部 RAM。

### 8.4 API 设计

```c
/* 时间结构体：语义与 Linux struct tm 对齐，但只保留 RTC 能表达的字段 */
typedef struct {
    uint16_t year;    /* 公元年，如 2026（芯片内只有 00~99，固定按 2000+ 处理） */
    uint8_t  month;   /* 1~12 */
    uint8_t  day;     /* 1~31 */
    uint8_t  hour;    /* 0~23（固定 24 小时制） */
    uint8_t  minute;  /* 0~59 */
    uint8_t  second;  /* 0~59 */
    uint8_t  weekday; /* 0=周日 … 6=周六（与 tm_wday 一致，由调用方或驱动按日期推算） */
} rtc_time_t;
```

| 函数 | 作用 | 返回 |
|---|---|---|
| `esp_err_t rtc_init(void)` | 复用 I²C0 总线、添加设备、关 CLKOUT、读一次状态 | `ESP_OK` / `ESP_ERR_INVALID_STATE`（总线未就绪）/ I²C 错误 |
| `esp_err_t rtc_get_time(rtc_time_t *out)` | 一次性读 `04h`~`0Ah` 7 字节并解析 | 同上 |
| `esp_err_t rtc_set_time(const rtc_time_t *t)` | STOP=1 → 一次写 7 字节 → STOP=0 | 同上 |
| `esp_err_t rtc_is_valid(bool *valid)` | OS 位 + 合理性校验 + 年份下限 | 同上 |
| `esp_err_t rtc_get_osf(bool *osf)` / `rtc_clear_osf(void)` | 单独读/清 OS 位 | 同上 |
| `esp_err_t rtc_read_reg(uint8_t reg, uint8_t *val)` / `rtc_write_reg(uint8_t reg, uint8_t val)` | 寄存器级逃生舱（alarm/timer/offset 用） | 同上 |
| `esp_err_t rtc_set_offset(int8_t offset_lsb, bool fast_mode)` | 写 `Offset`（`MODE` + `OFFSET[6:0]` 补码） | 同上 |
| `void rtc_time_to_tm(const rtc_time_t *t, struct tm *out)` / `void rtc_time_from_tm(const struct tm *in, rtc_time_t *t)` | 与 Linux `struct tm` 互转 | — |

设计要点：

- **`rtc_get_time` 返回的 `weekday` 直接取寄存器值**（芯片自己维护），**不要自己算**——
  但芯片不会自动纠正"你写错了星期"。`rtc_set_time()` 里若调用方给的 `weekday > 6`，用日期算一个填进去
  （用 `struct tm` + `mktime` 或 Zeller 公式；本项目已有 `time.h`）。
- 所有 I²C 操作给 **100 ms 超时**（`xfer_timeout_ms = 100`），失败返回 `ESP_FAIL`/`ESP_ERR_TIMEOUT`，
  **不要用 `ESP_ERROR_CHECK`**：RTC 坏了不应该让设备起不来（第 8.7 节）。
- 不缓存时间到静态变量（避免"缓存过期"类 bug）；缓存策略放在 UI 层（§8.6）。

### 8.5 时间有效性判定与"首次上电校时"策略

**判定（`rtc_is_valid`）**：

```
读 7 字节
→ OS == 0 ?
→ BCD 位型/范围合法（§4.5 第 3 层）
→ year >= 2024 ?        // 排除 2000-01-01 出厂默认
三者全真 → 有效
```

**策略（无网络对时设备，推荐）**：

| 场景 | 行为 |
|---|---|
| 首次上电 / OS=1 / 明显是出厂默认 | `rtc_is_valid = false` → **不要**自动写一个假时间（写 2000 年比不写更糟，用户会看到错的年份）；UI 显示 `--:--` |
| 用户在设置菜单手动校时 | `rtc_set_time()` + 同时 `rtc_clear_osf()`（写作时间时秒字节 bit7=0 已顺带清 OS） |
| 平时开机 | 只读不写；**绝不在每次开机时"顺手校时"**（会把电池保住的精度/时间冲掉） |
| 想减少手工输入 | 可选：把"上一次关机时间"存 NVS，开机时若 RTC 明显落后则提示用户确认，**不要静默写入** |

**绝对不要做的事**：

- ❌ 无条件发软件复位（写 `00h` = `0x58`）——会把电池保住的走时清零（§5.8 第 3 条）。
- ❌ 用 `__DATE__`/`__TIME__`（编译时间）当初始时间——固件可能是几个月前编的。
- ❌ 每次开机都 `rtc_set_time()`——同样会破坏已经准确的走时。

### 8.6 与 LVGL / UI 的对接

现状：顶部状态栏（`main/ui/app.c`）目前是"左=音量档位、中=当前页面名、右=电池电压"。加时间有两种做法：

**方案 A（推荐）：状态栏加一个 label，1 Hz 读 RTC**

- 在 `app.c` 的状态栏创建时加一个 `lv_label`（如显示 `12:34`，或 `12:34` + 小字星期）。
- 用一个 **1 s 周期的 `lv_timer`**（LVGL 任务上下文，已持锁，不需要 `lvgl_port_lock`）：
  ```
  每 1 s:
    rtc_get_time(&t)              // 一次 8 字节 I²C 事务，~200 µs
    if (失败) 显示 "--:--"
    else 拼 "HH:MM" 与当前 label 文本比较，不同才 lv_label_set_text()
  ```
- **务必"字符串没变就不 set_text"**：LVGL 的 `lv_label_set_text` 即使内容相同也会重建文本缓存/动画，
  项目里跑马灯已经被这个坑坑过（见 AGENTS.md 的 `global_style_marquee_set_text` 约定）。
  1 Hz 下如果每秒都 set，会带来无谓的 `lv_inv_area` 与重绘。
- 只显示到分钟时，可以 **10 s 或 30 s 读一次**来省总线；但要注意"分钟跳变最多晚 30 s"。
  项目现有实现建议：**1 Hz 读、只更新变化的字符串**，简单且看不出延迟，I²C 开销可忽略。

**方案 B：读一次 + 本地推秒**

- 开机读一次，之后用 `esp_timer_get_time()` 递推秒数，每分钟跟 RTC 对齐一次。
- 优点：总线访问更少。缺点：多一份"本地时基"状态，跨零点/暂停恢复时容易错，**不推荐**。

**注意事项**：

- RTC 读失败（I²C NACK）要**降级显示**，不能阻塞 UI 或反复重试打满总线。
- 若将来做"睡眠定时/闹钟"UI，**不要**指望 PCF85063A 的中断（本板 INT 未接）；
  现有 `sleep_timer` 基于 `esp_timer` 的方案不要改动。
- 时间字符串用 TTF 字体渲染，注意状态栏宽度（`460 px`，右侧已有电池电压，中间是页面名）——
  可能需要缩短为 `HH:MM` 两字符组，或把页面名左移。

### 8.7 初始化位置与顺序（`main/main.c`）

建议插在 `touch_init()` **之后**、`boot_flow_run()` 之前：

```c
touch_init();        // ← I²C0 总线在这里创建，touch_get_i2c_bus() 从此可用
rtc_init();          // ← 在这里；失败只打日志，不 abort
...
boot_flow_run(...);
```

- **必须在 `touch_init()` 之后**：总线句柄由触摸驱动创建（同步完成）。
- **不必很早**：LVGL 已在 `display_init()` 之后启动，RTC 只影响状态栏时间，晚一点无所谓。
- **失败不致命**：`rtc_init()` 内部不要 `ESP_ERROR_CHECK`。RTC 挂了设备仍要能放歌。
- 若将来把 I²C0 总线抽成独立模块（`i2c0_bus_init()`），RTC 与触摸都从那里取句柄，是更干净的长期方案；
  但**本次不要动 touch.c**（工作区有其它 agent 并发修改）。

---

## 9. 踩坑与注意事项

> 按"踩了会浪费半天"排序。

1. **`AEN_x` 是低有效使能**（0=使能，1=禁止），复位值 `0x80` = 全部禁止。
   写闹钟时忘了清 bit7 → 闹钟永远不响；反过来，只写了时分却让 Day_alarm 保持使能 →
   闹钟变成"每月只在某天响"。**日与星期是"与"关系**（§7.2）。
2. **不要无条件软件复位（`00h` = `0x58`）**。手册 §13.1 推荐上电后做 SR，但 SR 会把时间寄存器清成
   `2000-01-01 00:00:00`，**电池保住的走时全丢**。只有确认"时间无效"时才做（§8.5）。
3. **设置时间必须一次事务写 7 字节，且 1 秒内完成**。分两次写（先时分秒、再日月年）会被进位插进来，
   写坏时间（§5.5）。读同理：**一次 burst 读 7 字节**，不要分 7 次读。
4. **时间寄存器的复位值不是全 0**：`Seconds=0x80`（OS=1）、`Days=0x01`、`Weekdays=0x06`、`Months=0x01`、
   `Timer_mode=0x18`。用"读回来全是 0x00 就是没初始化"来判断会失败（§3.1/§4.5）。
5. **CLKOUT 上电默认是开着的（32.768 kHz，推挽）**。本项目 CLKOUT 悬空，等于让一根悬空脚持续翻转，
   既费电又有一点 EMI。初始化时写 `Control_2.COF=111` 关掉（§7.4）。
6. **`STOP` 位**：想"先把时间设好再开始走"就 `STOP=1` → 写时间 → `STOP=0`；
   **忘了清 STOP 就永远不走时**，且现象是"时间读写都正常，就是不动"（§3.2）。
   注意释放 STOP 后第一次 +1 秒在 **0.5078 s** 之后，不是整 1 s（±0.5 s 的相位，不影响长期）。
7. **BCD 与二进制混用**：秒~年、alarm 是 BCD；**Timer_value 是二进制；Weekdays 是 3 bit 二进制**。
   把 `25` 直接写进 Minutes 会得到 `0x19` = BCD 的 19 分（§4.1）。
8. **12/24 小时制默认是 24h（`12_24=0`）**。如果哪天真去切 12h，注意同一个 Hours 字节的位含义全变了
   （bit5 从"十位"变成 AM/PM），且**必须重写 Hours 与 Hour_alarm**（§4.2）。
9. **没有世纪位**。年份只有 00~99，按 2000+ 解释；闰年规则是"能被 4 整除（含 00）"，所以 `00` = 2000 是闰年（§4.3）。
10. **I²C 事务串行是驱动保证的，但"读-改-写"不是原子的**。清 AF/TF 时要 `读 Control_2 → 改 → 写回`，
    而"写 1 不变、写 0 清除"的语义让这件事很安全；但**不要用两次 I²C 调用去拼一次时间读取**。
11. **`tBUF` 与 1 秒窗口**：驱动自动满足 1.3 µs 的 tBUF；真正要自己保证的是"两次 START 之间 < 1 s"，
    也就是**别在 RTC 事务中间插入大延时**（比如 `vTaskDelay`、等信号量）。
12. **上拉电阻**：SDA/SCL 必须有上拉（板载已有外部上拉，触摸驱动还开了内部上拉）。400 kHz 下
    手册限 C<sub>b</sub> ≤ 400 pF、t<sub>r</sub> ≤ 300 ns；若加长飞线导致通信偶发失败，先怀疑上升时间。
13. **V<sub>DD</sub> 掉电**：掉到 0 之后时间丢失、OS 置位、日期回 2000-01-01；靠纽扣电池/超级电容维持时，
    手册要求"回流焊期间不得加电，且必须能完整断电一次"（§4.5、§5.8）。
14. **INT/CLKOUT 在本板未接**：不要写依赖中断的代码，也不要在 `rtc_init` 里轮询 `Control_2` 等 AF。
15. **手册自身的一处表述不清**：Offset 的正负号方向（"正 ppm 该写正还是负"）在 §7.2.3 的文字与 Figure 12 的
    算例之间不够直观，**属于手册未明确给出文字约定的地方**，标定时必须实测闭环（§6.2 第 2 条）。

---

## 10. 参考 C 代码

> 风格与本项目一致（snake_case、模块前缀 `rtc_`、中文注释）。寄存器名/位名用注释标在对应位置。
> 假定文件：`main/core/hardware/rtc.h` / `rtc.c`，并已把 `rtc.c` 加入 `main/CMakeLists.txt` 的 `MAIN_SRCS`。
> 以下代码**未经编译验证**（本文档只写文档，不改源码），落地时按实际接口微调。

### 10.1 `rtc.h`

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCF85063A 实时时钟（I²C0 共享总线）
 *
 * 硬件连接见 ideas_and_exps/reference_docs/hardware_v1.md：
 *   I²C0（SDA=GPIO48 / SCL=GPIO47 / 400 kHz），与 CST820(0x15)、AXP2101(0x34)、
 *   QMI8658(0x6B) 共用；本板 INT/CLKOUT 未接主控，只能轮询读取。
 *
 * datasheet 依据：PCF85063A Rev.7.3，寄存器表见 rtc_pcf85063a.md §3。
 */

#define RTC_I2C_ADDR            0x51    /* 7-bit 地址 1010 001（datasheet Table 38） */
#define RTC_I2C_TIMEOUT_MS      100

/* 寄存器地址（PCF85063A，00h~11h，共 18 个） */
#define RTC_REG_CONTROL_1       0x00
#define RTC_REG_CONTROL_2       0x01
#define RTC_REG_OFFSET          0x02
#define RTC_REG_RAM_BYTE        0x03
#define RTC_REG_SECONDS         0x04
#define RTC_REG_MINUTES         0x05
#define RTC_REG_HOURS           0x06
#define RTC_REG_DAYS            0x07
#define RTC_REG_WEEKDAYS        0x08
#define RTC_REG_MONTHS          0x09
#define RTC_REG_YEARS           0x0A
#define RTC_REG_SECOND_ALARM    0x0B
#define RTC_REG_MINUTE_ALARM    0x0C
#define RTC_REG_HOUR_ALARM      0x0D
#define RTC_REG_DAY_ALARM       0x0E
#define RTC_REG_WEEKDAY_ALARM   0x0F
#define RTC_REG_TIMER_VALUE     0x10
#define RTC_REG_TIMER_MODE      0x11

/* Control_1（00h）位定义 —— datasheet Table 5 */
#define RTC_C1_EXT_TEST         (1u << 7)
#define RTC_C1_STOP             (1u << 5)   /* 1 = 停走时（写时间时用） */
#define RTC_C1_SR               (1u << 4)   /* 软件复位；读恒 0 */
#define RTC_C1_CIE              (1u << 2)   /* 校正中断使能 */
#define RTC_C1_12_24            (1u << 1)   /* 1 = 12 小时制（默认 0 = 24h） */
#define RTC_C1_CAP_SEL          (1u << 0)   /* 0 = 7 pF，1 = 12.5 pF */

/* Control_2（01h）位定义 —— datasheet Table 8 */
#define RTC_C2_AIE              (1u << 7)
#define RTC_C2_AF               (1u << 6)   /* 写 0 清除，写 1 不变 */
#define RTC_C2_MI               (1u << 5)
#define RTC_C2_HMI              (1u << 4)
#define RTC_C2_TF               (1u << 3)   /* 写 0 清除 */
#define RTC_C2_COF_MASK         0x07u

/* Seconds（04h）bit7：振荡停止标志，1 = 时间可能无效 */
#define RTC_SEC_OS              (1u << 7)

/** 时间结构体（字段语义对齐 Linux struct tm，但只保留 RTC 能表达的字段） */
typedef struct {
    uint16_t year;      /* 公元年，如 2026（芯片只有 00~99，固定按 2000+ 解释） */
    uint8_t  month;     /* 1~12 */
    uint8_t  day;       /* 1~31 */
    uint8_t  hour;      /* 0~23（本项目固定 24 小时制） */
    uint8_t  minute;    /* 0~59 */
    uint8_t  second;    /* 0~59 */
    uint8_t  weekday;   /* 0 = 周日 … 6 = 周六（与 tm_wday 一致） */
} rtc_time_t;

/** 初始化：复用触摸驱动已创建的 I²C0 总线，添加 0x51 设备并关闭 CLKOUT */
esp_err_t rtc_init(void);

/** 一次 burst 读 7 字节（04h~0Ah）解析出时间 */
esp_err_t rtc_get_time(rtc_time_t *out);

/** 写时间：STOP=1 → 一次写 7 字节 → STOP=0 */
esp_err_t rtc_set_time(const rtc_time_t *t);

/** 时间有效性：OS=0 且字段合理且年份不低于 2024 */
esp_err_t rtc_is_valid(bool *valid);

/** 单独读 / 清 OS 标志 */
esp_err_t rtc_get_osf(bool *osf);
esp_err_t rtc_clear_osf(void);

/** 寄存器级读写（alarm / timer / offset 用） */
esp_err_t rtc_read_reg(uint8_t reg, uint8_t *val);
esp_err_t rtc_write_reg(uint8_t reg, uint8_t val);

/** 写 Offset 寄存器：offset_lsb 为 -64~+63，fast_mode = MODE 位 */
esp_err_t rtc_set_offset(int offset_lsb, bool fast_mode);

/** 与 Linux struct tm 互转（tm_wday 与 RTC 的 weekday 天然一致） */
void rtc_time_to_tm(const rtc_time_t *t, struct tm *out);
void rtc_time_from_tm(const struct tm *in, rtc_time_t *t);

#ifdef __cplusplus
}
#endif
```

### 10.2 `rtc.c`（核心部分）

```c
#include "rtc.h"

#include <string.h>

#include "esp_log.h"
#include "touch.h"          /* touch_get_i2c_bus()：I²C0 总线句柄由触摸驱动创建 */

static const char *TAG = "rtc";

static i2c_master_dev_handle_t s_rtc_dev = NULL;   /* 0x51 设备句柄（复用总线） */

static bool rtc_time_fields_ok(const rtc_time_t *t);   /* 前置声明：set 与 is_valid 共用 */

/* ---------------- BCD ↔ 十进制 ----------------
 * PCF85063A 的秒/分/时/日/月/年与全部 alarm 寄存器是 BCD（datasheet §7 "BCD format"）；
 * 注意 Timer_value 是二进制、Weekdays 是 3bit 二进制，不要套这两个函数。
 */
static inline uint8_t rtc_bcd2bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) & 0x0F) * 10u + (v & 0x0F));
}

static inline uint8_t rtc_bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* ---------------- 寄存器级读写 ---------------- */

esp_err_t rtc_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_rtc_dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 写指针 + repeated START + 读 1 字节（datasheet Figure 23） */
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, val, 1, RTC_I2C_TIMEOUT_MS);
}

esp_err_t rtc_write_reg(uint8_t reg, uint8_t val)
{
    if (s_rtc_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };   /* 指针 + 数据，一次事务（datasheet Figure 22） */
    return i2c_master_transmit(s_rtc_dev, buf, sizeof(buf), RTC_I2C_TIMEOUT_MS);
}

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_rtc_dev == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 新驱动会自动处理地址自增（芯片侧自增，11h 后回绕到 00h） */
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, buf, len, RTC_I2C_TIMEOUT_MS);
}

/* ---------------- 初始化 ---------------- */

esp_err_t rtc_init(void)
{
    /* ⚠ 复用触摸驱动创建的 I²C0 总线，绝不能再 i2c_new_master_bus(I2C_NUM_0)：
       引脚已被占用，重复初始化会失败；deinit 重建还会把触摸一起搞死。 */
    i2c_master_bus_handle_t bus = touch_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C0 bus not ready (touch_init 必须先跑)");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = 400 * 1000,   /* 必须 > 0，且与总线一致（IDF 6.1 硬校验） */
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_rtc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", RTC_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    /* 探测一次，确认芯片在位（发地址看 ACK） */
    err = i2c_master_probe(bus, RTC_I2C_ADDR, RTC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A not responding at 0x%02x", RTC_I2C_ADDR);
        /* 不致命：没有 RTC 也要能开机放歌 */
    }

    /* 关闭 CLKOUT：复位默认 COF=000（32.768 kHz 推挽输出，本板悬空），既费电又有 EMI。
       COF=111 → CLKOUT = LOW。同时把 AF/TF 写 0（写 0 即清除，写 1 保持不变）。
       注意这里不能置 AIE/MI/HMI（本板 INT 未接，不需要中断）。 */
    uint8_t c2 = RTC_C2_COF_MASK;           /* 0b0000_0111：AIE=0 AF=0 MI=0 HMI=0 TF=0 COF=111 */
    err = rtc_write_reg(RTC_REG_CONTROL_2, c2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable CLKOUT failed: %s", esp_err_to_name(err));
    }

    bool valid = false;
    rtc_is_valid(&valid);
    ESP_LOGI(TAG, "PCF85063A ready, time %s", valid ? "valid" : "INVALID");
    return ESP_OK;
}

/* ---------------- 读时间 ---------------- */

esp_err_t rtc_get_time(rtc_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ⚠ 必须一次读完 7 字节（04h~0Ah）：
       读写时间寄存器期间芯片冻结计数，且积压的自增请求最多只保留 1 个，
       整个访问必须在 1 秒内完成（datasheet §7.4 / Figure 15）。
       分多次读会出现"分来自这一刻、时来自下一刻"的进位撕裂。 */
    uint8_t buf[7] = { 0 };
    esp_err_t err = rtc_read_regs(RTC_REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* buf[0] Seconds：bit7 = OS，bit6-4 十位，bit3-0 个位 */
    uint8_t second = rtc_bcd2bin(buf[0] & 0x7F);
    /* buf[1] Minutes：bit7 未使用 */
    uint8_t minute = rtc_bcd2bin(buf[1] & 0x7F);

    /* buf[2] Hours：24 小时制时 bit5-4 = 十位、bit3-0 = 个位
       （12 小时制时 bit5 = AMPM、bit4 = 十位；本项目固定 24h，不处理 12h 分支） */
    uint8_t hour = rtc_bcd2bin(buf[2] & 0x3F);

    /* buf[3] Days：bit5-4 十位、bit3-0 个位（1~31） */
    uint8_t day = rtc_bcd2bin(buf[3] & 0x3F);
    /* buf[4] Weekdays：bit2-0，0 = 周日 … 6 = 周六（3bit 二进制，不是 BCD） */
    uint8_t weekday = (uint8_t)(buf[4] & 0x07);
    /* buf[5] Months：bit4 十位、bit3-0 个位（1~12） */
    uint8_t month = rtc_bcd2bin(buf[5] & 0x1F);
    /* buf[6] Years：bit7-4 十位、bit3-0 个位（00~99） */
    uint16_t year = (uint16_t)(2000 + rtc_bcd2bin(buf[6]));

    out->year    = year;
    out->month   = month;
    out->day     = day;
    out->hour    = hour;
    out->minute  = minute;
    out->second  = second;
    out->weekday = weekday;

    ESP_LOGD(TAG, "get %04u-%02u-%02u %02u:%02u:%02u wd=%u",
             year, month, day, hour, minute, second, weekday);
    return ESP_OK;
}

/* ---------------- 写时间 ---------------- */

esp_err_t rtc_set_time(const rtc_time_t *t)
{
    if (t == NULL || !rtc_time_fields_ok(t)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) 置 STOP=1：让分频链停住，写时间时不会被进位打断。
          读-改-写是为了保留 CAP_SEL / 12_24 / CIE 等既有配置。 */
    uint8_t c1 = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_CONTROL_1, &c1);
    if (err != ESP_OK) {
        return err;
    }
    err = rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 | RTC_C1_STOP));
    if (err != ESP_OK) {
        return err;
    }

    /* 2) 一次事务写 7 字节：Seconds..Years。
          Seconds 的 bit7（OS）写 0 —— 这是清除振荡停止标志的唯一手段。 */
    uint8_t buf[7];
    buf[0] = rtc_bin2bcd(t->second) & 0x7F;          /* OS = 0 */
    buf[1] = rtc_bin2bcd(t->minute) & 0x7F;
    buf[2] = rtc_bin2bcd(t->hour)   & 0x3F;          /* 24h：bit5-4 十位、bit3-0 个位 */
    buf[3] = rtc_bin2bcd(t->day)    & 0x3F;
    buf[4] = (uint8_t)(t->weekday & 0x07);           /* 3bit 二进制，不是 BCD */
    buf[5] = rtc_bin2bcd(t->month)  & 0x1F;
    buf[6] = rtc_bin2bcd((uint8_t)(t->year % 100));

    uint8_t wbuf[8];
    wbuf[0] = RTC_REG_SECONDS;                       /* 寄存器指针 04h */
    memcpy(&wbuf[1], buf, sizeof(buf));
    err = i2c_master_transmit(s_rtc_dev, wbuf, sizeof(wbuf), RTC_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        /* 失败也要把 STOP 放掉，否则 RTC 永远停走 */
        rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 & ~RTC_C1_STOP));
        return err;
    }

    /* 3) 清 STOP 开始走时。注意：释放后第一次 +1 秒在 0.507813~0.507935 s 之后
          （不是因为 STOP 有问题，而是 F0/F1 两级分频器不复位，见 datasheet Table 6）。 */
    err = rtc_write_reg(RTC_REG_CONTROL_1, (uint8_t)(c1 & ~RTC_C1_STOP));
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "set %04u-%02u-%02u %02u:%02u:%02u wd=%u",
             t->year, t->month, t->day, t->hour, t->minute, t->second, t->weekday);
    return ESP_OK;
}

/* ---------------- 有效性 / OS 标志 ---------------- */

esp_err_t rtc_get_osf(bool *osf)
{
    uint8_t sec = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_SECONDS, &sec);
    if (err != ESP_OK) {
        return err;
    }
    *osf = (sec & RTC_SEC_OS) != 0;
    return ESP_OK;
}

esp_err_t rtc_clear_osf(void)
{
    /* 写 0 到 Seconds.bit7 即可清除；其余位保持原值，避免把秒改坏 */
    uint8_t sec = 0;
    esp_err_t err = rtc_read_reg(RTC_REG_SECONDS, &sec);
    if (err != ESP_OK) {
        return err;
    }
    return rtc_write_reg(RTC_REG_SECONDS, (uint8_t)(sec & ~RTC_SEC_OS));
}

esp_err_t rtc_is_valid(bool *valid)
{
    if (valid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *valid = false;

    uint8_t buf[7] = { 0 };
    esp_err_t err = rtc_read_regs(RTC_REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* 1) OS 位：1 = 振荡器停过/被中断，时间不可信（datasheet §7.3.1.1） */
    if (buf[0] & RTC_SEC_OS) {
        return ESP_OK;                     /* 保持 false */
    }

    /* 2) BCD 位型 + 数值范围合理性 */
    const uint8_t sec  = (uint8_t)(buf[0] & 0x7F);
    const uint8_t min  = (uint8_t)(buf[1] & 0x7F);
    const uint8_t hour = (uint8_t)(buf[2] & 0x3F);
    const uint8_t day  = (uint8_t)(buf[3] & 0x3F);
    const uint8_t mon  = (uint8_t)(buf[5] & 0x1F);

    if ((sec & 0x0F) > 9 || (sec >> 4) > 5) return ESP_OK;
    if ((min & 0x0F) > 9 || (min >> 4) > 5) return ESP_OK;
    if ((hour & 0x0F) > 9 || (hour >> 4) > 2) return ESP_OK;
    if ((day & 0x0F) > 9 || (day >> 4) > 3) return ESP_OK;
    if ((mon & 0x0F) > 9 || (mon >> 4) > 1) return ESP_OK;

    rtc_time_t t;
    err = rtc_get_time(&t);                /* 用已校验过的字节重建一次结构体 */
    if (err != ESP_OK) {
        return err;
    }
    if (!rtc_time_fields_ok(&t)) {
        return ESP_OK;                     /* 保持 false */
    }

    /* 3) 年份下限：排除 2000-01-01 00:00:00 这类出厂默认（芯片无出厂标记，只能这样折衷） */
    *valid = (t.year >= 2024);
    return ESP_OK;
}

/* 字段范围检查（供 set/is_valid 共用） */
static bool rtc_time_fields_ok(const rtc_time_t *t)
{
    if (t->month < 1 || t->month > 12) return false;
    if (t->hour > 23 || t->minute > 59 || t->second > 59) return false;
    if (t->weekday > 6) return false;
    if (t->year < 2000 || t->year > 2099) return false;

    static const uint8_t days_in_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint8_t dim = days_in_month[t->month - 1];
    /* 闰年：芯片的规则是"年数值能被 4 整除（含 00）就补 29 日"，即 2000 年是闰年 */
    if (t->month == 2 && (t->year % 4) == 0) {
        dim = 29;
    }
    return (t->day >= 1 && t->day <= dim);
}
```

### 10.3 Offset 写入

```c
esp_err_t rtc_set_offset(int offset_lsb, bool fast_mode)
{
    if (offset_lsb < -64 || offset_lsb > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Offset（02h）：bit7 = MODE（0 = 每 2 小时校正，1 = 每 4 分钟校正）
                       bit6-0 = OFFSET[6:0]，二进制补码 +63 ~ -64
       每个 LSB：MODE=0 时 4.34 ppm，MODE=1 时 4.069 ppm（datasheet Table 11/12） */
    uint8_t v = (uint8_t)(offset_lsb & 0x7F);
    if (fast_mode) {
        v |= 0x80;
    }
    return rtc_write_reg(RTC_REG_OFFSET, v);
}
```

### 10.4 与 Linux `struct tm` 互转

```c
/*
 * struct tm（glibc/newlib）字段约定：
 *   tm_sec  0~59
 *   tm_min  0~59
 *   tm_hour 0~23
 *   tm_mday 1~31
 *   tm_mon  0~11   ← 注意是 0 基，和 RTC 的 1~12 差 1
 *   tm_year 自 1900 起的年数 ← 和 RTC 的"2000 + 年份"差 1900
 *   tm_wday 0 = 周日 … 6 = 周六 ← 与 RTC 的 Weekdays 寄存器一致，直接赋值
 */
void rtc_time_to_tm(const rtc_time_t *t, struct tm *out)
{
    if (t == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec  = t->second;
    out->tm_min  = t->minute;
    out->tm_hour = t->hour;
    out->tm_mday = t->day;
    out->tm_mon  = (int)t->month - 1;             /* 1~12 → 0~11 */
    out->tm_year = (int)t->year - 1900;           /* 2026 → 126 */
    out->tm_wday = t->weekday;                    /* 0 = 周日，与 RTC 一致 */
    out->tm_isdst = -1;                           /* 无夏令时概念 */
}

void rtc_time_from_tm(const struct tm *in, rtc_time_t *t)
{
    if (in == NULL || t == NULL) {
        return;
    }
    t->second  = (uint8_t)in->tm_sec;
    t->minute  = (uint8_t)in->tm_min;
    t->hour    = (uint8_t)in->tm_hour;
    t->day     = (uint8_t)in->tm_mday;
    t->month   = (uint8_t)(in->tm_mon + 1);       /* 0~11 → 1~12 */
    t->year    = (uint16_t)(in->tm_year + 1900);  /* 126 → 2026 */
    t->weekday = (uint8_t)in->tm_wday;            /* 若调用方未填 wday，用 mktime 归一化后再取 */
}
```

> **注意**：`mktime()` 会**修改传入的 `struct tm`**（归一化 `tm_wday`/`tm_yday`），
> 且依赖 TZ 环境。本项目没有 RTC 时区概念，建议**只把 `mktime` 用来算 `tm_wday`**：
> `time_t tt = mktime(&tmp); localtime_r(&tt, &tmp);` 然后取 `tmp.tm_wday`。
> 若不想引入 TZ，可用 Zeller/Sakamoto 公式算星期，避免 `mktime` 的夏令时干扰。

---

## 11. 附录：速查表与"手册未给出"清单

### 11.1 一页速查

| 想做的事 | 做法 |
|---|---|
| 读时间 | 一次 `transmit_receive(0x04, 7 字节)`，解析见 §10.2 |
| 写时间 | `STOP=1` → 一次写 7 字节（秒..年）→ `STOP=0` |
| 判断时间是否可信 | `Seconds.bit7(OS) == 0` 且字段合法 且 年份 ≥ 2024 |
| 清 OS | 写 `Seconds` 时 bit7 写 0 |
| 关 CLKOUT | `Control_2` 的 `COF[2:0] = 111` |
| 24 小时制 | `Control_1.bit1 = 0`（默认） |
| 每天闹钟 | 时/分/秒 alarm 使能（bit7=0），**日/星期 alarm 全部禁用（bit7=1）** |
| 关中断/清标志 | `Control_2` 对应位写 0（AF bit6、TF bit3） |
| 微调频率 | `Offset(02h)`：`MODE` + `OFFSET[6:0]` 补码；MODE=0 时 4.34 ppm/LSB |
| 软件复位 | 写 `00h = 0x58`（**会清时间！慎用**） |
| 地址自增 | 自动；`11h` 后回绕 `00h` |

### 11.2 手册未给出 / 本板未确认（不要凭猜测写代码）

| 项目 | 状态 |
|---|---|
| 整机"典型精度 ppm"指标 | **手册未给出**（只有 offset 校准可达 ±2 ppm、±1 ppm≈0.0864 s/天） |
| Δf/f 随温度的曲线（温度系数） | **手册未给出**（只有 I<sub>DD</sub> 随温度的图）；需查晶振规格书 |
| 12 小时制下 12 AM/12 PM 与 0/12 的换算约定 | **手册未给出**（只说 12 小时制 HOURS 为 1~12）→ 本项目固定 24h |
| Offset 正负号与"走快/走慢"的文字对应 | **手册表述不够明确**（文字与 Figure 12 算例方向需实测闭环） |
| 出厂"未校时"标记 | **手册未提供**（只能靠 OS 位 + 复位默认值 + 年份下限推断） |
| 本板 RTC 是否有纽扣电池/超级电容备份 | **本板未确认**（hardware_v1.md 未提供） |
| 本板 RTC 晶振型号与 C<sub>L</sub> / `CAP_SEL` 实际取值 | **本板未确认** → 驱动保持默认（7 pF） |
| 本板 RTC 封装（SO8 / TSSOP8 / DFN） | **本板未确认**（不影响驱动） |
| 本板 INT / CLKOUT 是否可飞线到 GPIO | **本板未确认**（hardware_v1.md 明确未提供连接） |

### 11.3 主要表格索引（便于回查原文）

| 内容 | 页码 | 图表号 |
|---|---|---|
| 订购信息 / 封装 | p.2 | Table 1、Table 2 |
| 引脚定义 | p.4 | Table 3 |
| 寄存器总览 | p.5-6 | Table 4 |
| Control_1 | p.6-7 | Table 5 |
| STOP 释放后首次 +1 秒 | p.8-9 | Table 6、Figure 7 |
| 寄存器复位值 | p.10-11 | Table 7 |
| Control_2 | p.11 | Table 8 |
| 中断结构 | p.12 | Figure 9 |
| MI/HMI 组合 | p.13 | Table 9 |
| CLKOUT 频率 | p.14 | Table 10 |
| Offset / 校正值 | p.14-15 | Table 11、Table 12 |
| MODE=0/1 校正脉冲 | p.15-17 | Table 13、Table 15 |
| 校正频率影响 | p.16-17 | Table 14、Table 16 |
| 校准流程与结果 | p.18 | Figure 11、Figure 12 |
| RAM_byte | p.19 | Table 17 |
| Seconds / BCD 示例 | p.19 | Table 18、Table 19 |
| Minutes / Hours | p.20-21 | Table 20、Table 21 |
| Days / Weekdays | p.21 | Table 22、Table 23、Table 24 |
| Months | p.22 | Table 25、Table 26 |
| Years | p.23 | Table 27 |
| 读写时间流程（1 秒窗口） | p.23-24 | Figure 14、Figure 15 |
| Alarm 寄存器 | p.24-26 | Table 28~Table 32、Figure 16 |
| Timer | p.27-29 | Table 33~Table 37、Figure 17 |
| I²C 地址 | p.31 | Table 38 |
| 读写时序（图） | p.32 | Figure 22、Figure 23 |
| 限值 / 静态特性 | p.33-35 | Table 39、Table 40 |
| I²C 交流特性 | p.39-40 | Table 41 |
| 应用图 / 上电流程 | p.41-42 | Figure 31、§13.1 |
| 封装图 / 焊盘 | p.43-50 | Figure 32~Figure 38 |
| 修订历史 | p.51-52 | Table 45 |
