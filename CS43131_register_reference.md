# CS43131 数据手册寄存器考据报告（用于 ESP32 音频驱动改写）

> 依据：`CS43131_datasheet_full\CS43131_N.jpeg`（文件页号 N == 印刷页号 N）。
> 所有 bit 字段均以 **Section 7 "Register Descriptions" 详细描述页** 为准（Quick Reference 表仅用于定位地址/详述页码）。
> 封面/目录见 p.3；寄存器块基址见 p.106 Table 6-1：`0x01xxxx(Global) / 0x02xxxx(PLL) / 0x04xxxx(ASP,XSP) / 0x08xxxx(Headphone,PCM) / 0x0Fxxxx(Interrupt)`。

---

## ⚠ 关键结论（先读这段）

1. **ASP_M/SB（master/slave）在 `0x040018` 的 bit 4**。数据手册原文（7.3.10）：**bit4 = 0 → Slave Mode（input），bit4 = 1 → Master Mode（output）**。
   —— 即：**bit4=0 是 SLAVE（从机），bit4=1 是 MASTER（主机）**。这直接回答了你要确认的矛盾点。
2. **串口采样率寄存器实际地址是 `0x010008`（不是 0x01000B）**，默认值 0x01 = 44.1kHz。你之前记的 0x01000B 有误。
3. **串口采样位宽寄存器是 `0x01000C`**：ASP_SPSIZE = bit[1:0]，编码 **00=32bit，01=24bit(默认)，10=16bit，11=8bit**。没有 “4/6” 这种编码。
4. **PCM 静音/斜坡寄存器 `0x090003`**：你记的 “bit3=SZC” 有误 —— **PCM_SZC 是 bit[5:4]**；`bit3=PCM_AMUTE`（自动静音）；静音位是 **bit1=PCM_MUTE_A、bit0=PCM_MUTE_B**。
5. **`0x0B0000`(HV 模式)、`0x010010`(pop-free)、`0x080032`(HP 控制) 在寄存器表里不存在**。它们不在基址表/Quick Reference/详细描述中，地址很可能是记错了（参见文末“未找到/待核实”）。
6. **CS43131 在本项目中是 I2S 从机**（ESP32 出 BCK/WS/DIN，见 AGENTS.md GPIO38/39/40）。因此 **ASP_M/SB 必须写 0**，且 ASP 的 N/M、LCHI/LCPR 分频器在主模式下才真正用于生成 SCLK/LRCK；从机模式下它们不参与时钟生成（从机下 SCLK/LRCK 是输入）。

---

## 1. `0x040018` ASP Clock Configuration（7.3.10，详述 p.120）
复位值（图中 Default 行）= `0x1C`（bit4=1, bit3=1, bit2=1, bit1=0, bit0=0）
⚠ 注意：图中 Default 行 bit4=1，但描述写 “0=(Default) Slave”。**以描述为准：bit4=0=Slave，bit4=1=Master**。此为手册内部矛盾，已在文末标注。

| Bit | Name | 含义 | 复位 |
|---|---|---|---|
| 7:5 | Reserved | — | 0 |
| 4 | **ASP_M/SB** | **0 = Slave Mode (input)（默认，从机）；1 = Master Mode (output)（主机）** | 见上（描述=0，图=1，矛盾） |
| 3 | **ASP_SCPOL_OUT** | SCLK（BCLK）**输出**驱动极性：0=Normal，1=Inverted（默认） | 1 |
| 2 | **ASP_SCPOL_IN** | SCLK（BCLK）**输入**极性(pad→logic)：0=Normal，1=Inverted（默认） | 1 |
| 1 | **ASP_LCPOL_OUT** | LRCK（WS）**输出**驱动极性：0=Normal（默认），1=Inverted | 0 |
| 0 | **ASP_LCPOL_IN** | LRCK（WS）**输入**极性：0=Normal（默认），1=Inverted | 0 |

> 使用提示：从机（本应用）用 `SCPOL_IN`(bit2) + `LCPOL_IN`(bit0)；主机模式用 `SCPOL_OUT`(bit3) + `LCPOL_OUT`(bit1)。
> 常见 I2S 反相（迟采/边沿）就在这里调。默认把 SCLK 反相、LRCK 不反相。

### 相邻寄存器概要
- **`0x040017` ASP LRCK Period 2**（= ASP_LCPR_MSB，p.119）：bit7:0 = ASP_LCPR_MSB，默认 0x00。与 0x040016(LSB) 组成 16 bit LRCK 周期，默认 ASP_LCPR=63。
- **`0x040019` ASP Frame Configuration**（7.3.11，p.120）：bit7:5 Reserved；**bit4 = ASP_STP**（帧起始相位：0=默认 LRCK 高→低开始，1=低→高开始）；**bit3 = ASP_5050**（LRCK 固定 50/50 占空：0=可编程(按 LCHI/LCPR)，1=默认 固定50%）；**bit2:0 = ASP_FSD**（帧起始延时，单位 ASP_SCLK 周期：000=0，001=0.5，010=默认 1.0，101=2.5，110/111=Reserved）。复位值约 `0x09`（bit3=1,bit0=1）。

---

## 2. `0x040010`–`0x040017`（ASP 主模式 SCLK/LRCK 分频器）
这些寄存器**仅在主模式(ASP_M/SB=1)下用于从内部 MCLK 生成 SCLK/LRCK**；从机模式下不参与时钟生成。

| 地址 | 寄存器名 | 字段 | 默认 | 详述页 |
|---|---|---|---|---|
| 0x040010 | ASP_Numerator 1 | bit7:0 = ASP_N_LSB | 0x01 | p.118 |
| 0x040011 | ASP_Numerator 2 | bit7:0 = ASP_N_MSB | 0x00 | p.118 |
| 0x040012 | ASP_Denominator 1 | bit7:0 = ASP_M_LSB | 0x08 | p.118 |
| 0x040013 | ASP_Denominator 2 | bit7:0 = ASP_M_MSB | 0x00 | p.119 |
| 0x040014 | ASP LRCK High Time 1 | bit7:0 = ASP_LCHI_LSB | 0x1F(31) | p.119 |
| 0x040015 | ASP LRCK High Time 2 | bit7:0 = ASP_LCHI_MSB | 0x00 | p.119 |
| 0x040016 | ASP LRCK Period 1 | bit7:0 = ASP_LCPR_LSB | 0x3F(63) | p.119 |
| 0x040017 | ASP LRCK Period 2 | bit7:0 = ASP_LCPR_MSB | 0x00 | p.119 |

- ASP_N = (ASP_N_MSB << 8) | ASP_N_LSB  → 默认 0x0001 = 1
- ASP_M = (ASP_M_MSB << 8) | ASP_M_LSB  → 默认 0x0008 = 8（p.118 描述 "Default ASP_M=8"；**注意 p.119 有一处误写 "=6"，以寄存器值 0x08 为准**）
- ASP_LCHI = (LCHI_MSB<<8)|LCHI_LSB → 默认 31（LRCK 高电平宽度，单位 SCLK 周期）
- ASP_LCPR = (LCPR_MSB<<8)|LCPR_LSB → 默认 63（LRCK 周期，单位 SCLK 周期，即每帧 SCLK 数）

### 生成公式（结合 Fig 4-20/4-21/4-22，p.48-49）
- **SCLK (BCLK) = MCLK × (ASP_N / ASP_M)**   （Fig 4-20：Internal MCLK → N/M 14bit 分频 → SCLK）
- **LRCK (WS) = SCLK / ASP_LCPR**，即帧长 = ASP_LCPR 个 SCLK 周期（Fig 4-21/4-22：xSP_LCPR 决定 LRCK/FSYNC 周期；有效范围 16~1536 SCLK）
- **LRCK 高电平宽度 = ASP_LCHI** 个 SCLK 周期，且必须满足 **ASP_LCHI < ASP_LCPR**（Fig 4-22 原文：LRCK-high width must be less than the LRCK period）
- 若 **ASP_5050=1（默认）**，LRCK 固定 50/50 占空，LCHI/LCPR 用于计算帧长。

> **待核实/推断**：手册用 “周期” 表述 LCPR。默认 LCPR=63、LCHI=31，配合 32bit×2=64 的 I2S 帧时，常按 **“寄存器值+1 = 实际周期”** 解释（63+1=64，31+1=32 → 50% 占空）。**请驱动作者以实测/示波器为准**，因为手册 Fig 4-22 措辞并未明确是 “+1” 还是直接取用。
> 你提到的原厂固件写 `1/0/8/0/31` 到 0x040010/011/012/013/014 —— 恰好就是这些寄存器的**默认值**（N=1、M=8、LCHI_LSB=31）。

### 附：`0x040004` CLKOUT Control（p.118）
- bit4:2 = CLKOUT_DIV：000=除2(默认)，001=除3，010=除4，011=除8，100-111 Reserved
- bit1:0 = CLKOUT_SEL：00=默认 XTAL/MCLK 路径，01=PLL 输出路径，10-11 Reserved

---

## 3. `0x01000C` Serial Port Sample Bit Size（7.1.8，p.114）
复位值 `0x05`（XSP=24bit, ASP=24bit）。

| Bit | Name | 编码 | 详述 |
|---|---|---|---|
| 7:4 | Reserved | — | 0 |
| 3:2 | XSP_SPSIZE | 00=32bit，01=24bit(默认)，10-11=Reserved | 辅助串口(XSP)采样位宽 |
| 1:0 | **ASP_SPSIZE** | **00=32bit，01=24bit(默认)，10=16bit，11=8bit** | 主串口(ASP)采样位宽 |

> **你的疑问“4/6”不存在**：ASP_SPSIZE 是 2bit 编码，只有 0/1/2/3 → 32/24/16/8bit。
> 因此：**24bit 写 ASP_SPSIZE=0b01（寄存器值 bit1:0=01，与 XSP 组合后整字节默认 0x05）；32bit 写 bit1:0=00；16bit 写 bit1:0=10；8bit 写 bit1:0=11**。
> 若要只写 ASP 位而保留 XSP：`val = (val & 0x0C) | asp_size`。

---

## 4. `0x010008` Serial Port Sample Rate（7.1.7，p.114）
（注意地址是 **0x010008**，不是 0x01000B）
复位值 `0x01` = 44.1kHz。bit7:4 Reserved；**bit3:0 = ASP_SPRATE**：

| ASP_SPRATE | 采样率 |
|---|---|
| 0000 | 32 kHz |
| 0001 | 44.1 kHz（默认） |
| 0010 | 48 kHz |
| 0011 | 88.2 kHz |
| 0100 | 96 kHz |
| 0101 | 176.4 kHz |
| 0110 | 192 kHz |
| 0111 | 352.8 kHz |
| 1000 | 384 kHz |
| 1001-1111 | Reserved |

> 寄存器原文：主/从模式都必须编程 ASP_SPRATE。若 ASP_SPRATE=384kHz 且处于主机模式，则 MCLK_INT 必须=24.576MHz；其它采样率 MCLK_INT 可为 22.5792 或 24.576MHz。
> 例：48kHz → 写 0x02。

---

## 5. `0x05000A` / `0x05000B` ASP Channel n Size and Enable（7.3.23/7.3.24，p.123）
（注：这两个寄存器位于地址 0x0500xx，Quick Reference / 基址表把该块标为 “DSP”，但内容为 ASP 接收通道配置。）
0x05000A = Channel 1，0x05000B = Channel 2。复位：0x05000A=`0x06`，0x05000B=`0x0E`。

| Bit | Name | 含义 | 复位 |
|---|---|---|---|
| 7:4 | Reserved | — | 0 |
| 3 | **ASP_RX_CHn_AP** | 通道**有效相位**（仅在 50/50 模式即 ASP_5050=1 时有效）：**CH1 默认 0 = LRCK/FSYNC 为低时输入；CH2 默认 1 = LRCK 为高时输入** | CH1=0，CH2=1 |
| 2 | **ASP_RX_CHn_EN** | 通道**使能**：0=输入数据不送入内部数据通路（描述写“默认”），**1=输入数据送入内部数据通路** | 描述=0，图=1（矛盾，见下） |
| 1:0 | **ASP_RX_CHn_RES** | 通道采样位宽：**00=8bit，01=16bit，10=24bit(默认)，11=32bit** | 10(24bit) |

### 你的 “0x07” 与 “0x0F” 是什么
- **0x07** = 0b0111 → CH1：AP=0、EN=1、RES=11(32bit)  （bit2=1 使能，bit1:0=11 即 32bit）
- **0x0F** = 0b1111 → CH2：AP=1、EN=1、RES=11(32bit)
- **两者都是“32bit + 通道使能”，唯一区别是 bit3（AP 有效相位）：CH1=0、CH2=1**。若处于 50/50 模式(ASP_5050=1)，此位决定该通道落在 LRCK 高/低半帧。
- 若无需区分 AP（非 50/50 模式），CH1/CH2 可都填 0x07（32bit+使能），或默认 0x06（24bit+使能）、0x0E。

> ⚠ 描述写 CHn_EN 默认=0（禁用），但寄存器 Default 行 bit2=1（可用/使能）。**以“驱动要出声必须置 EN=1”为准**，复位值以 Default 行 0x06/0x0E 为准。此为手册内部矛盾，已标注。

---

## 6. `0x090003` PCM Path Signal Control 1（7.5.5，p.129）
复位值 `0xA8`。

| Bit | Name | 含义 | 复位 |
|---|---|---|---|
| 7 | PCM_RAMP_DOWN | 滤波模式切换前的软音量斜坡：0=立即静音，1=(默认) 由 PCM_SZC 控制静音/解除 | 1 |
| 6 | PCM_VOL_BEQA | 声道 B 音量 = 声道 A：0=默认 独立控制，1=两声道都用 PCM_VOLUME_A（忽略 B） | 0 |
| 5:4 | **PCM_SZC** | **软斜坡/过零控制：00=立即，01=PCM 过零，10=(默认) 软斜坡，11=软斜坡+过零** | 10 (0b10) |
| 3 | **PCM_AMUTE** | 自动静音：0=禁用，1=(默认) 收到 8192 个连续 ±1/0 静态样本后静音，出现非静态样本即解除；逐通道独立 | 1 |
| 2 | PCM_AMUTEBEQA | 声道 B 自动静音条件=声道 A：0=默认 独立，1=仅当两声道同时满足 AMUTE 才静音 | 0 |
| 1 | **PCM_MUTE_A** | 声道 A 静音：0=默认 禁用，1=声道 A 输出静音（受 PCM_SZC 影响） | 0 |
| 0 | **PCM_MUTE_B** | 声道 B 静音：0=默认 禁用，1=声道 B 输出静音（受 PCM_SZC 影响） | 0 |

> **订正**：你原以为 “bit3 是软斜坡 SZC”，实际 **SZC = bit[5:4]**；**bit3 是 PCM_AMUTE**；静音是 **bit[1:0] = [MUTE_A, MUTE_B]**。
> 常见写：解除静音 → bit[1:0]=00；静音两声道 → bit[1:0]=11。若需要软斜坡，保持 PCM_SZC=10(默认) 即可。

### 附：`0x090000` PCM Filter Option（7.5.2，p.128，默认 0x00）
bit7 FILTER_SLOW_FAST(0=Fast默认,1=Slow)；bit6 PHCOMP_LOWLATB(0=Low-latency默认,1=Phase-comp)；bit5 NOS(0=off默认,1=on)；bit4:3 Reserved；bit2 PCM_WBF_EN(0=默认 禁用,1=宽平坦模式,仅 192kHz)；bit1 HIGH_PASS(0=默认 禁用,1=高通)；bit0 DEEMP_ON(0=默认 关,1=44.1kHz 去加重)。

### 附：`0x090004` PCM Path Signal Control 2（7.5.6，p.129，默认 0x00）
bit3 PCM_INV_A；bit2 PCM_INV_B；bit1 PCM_SWAP_CHAN(0=A/B 不交换,1=A↔B)；bit0 PCM_COPY_CHAN(0=默认,1=A 复制到 B)。

---

## 7. `0x090001` / `0x090002` PCM Volume B / A（7.5.3 / 7.5.4，p.128）
bit7:0 = PCM_VOLUME_x。**1/2 dB 步进，从 0dB 到 −127.5dB**。复位值 `0x78`（=−60dB）。

| 编码 | 音量 |
|---|---|
| 0x00 | 0 dB（最大，0 衰减） |
| 0x01 | −0.5 dB |
| 0x20 | **−16 dB**（0x20=32 → 32×0.5=16dB） |
| 0x78 | −60 dB（默认） |
| 0xFE | −127 dB（最小实际步进） |
| 0xFF | Digital mute（数字静音） |

> **订正你的“0xFF=最小”**：0xFF 是 **数字静音**，不是普通音量步进；**真实“最小量”是 0xFE=−127dB**。映射关系 **dB = −(0x__ × 0.5)**。
> 0x20 = −16dB；默认 0x78=−60dB。
> 音量变化受 PCM_SZC(bit[5:4]) 控制，用于防爆音。

---

## 8. `0x010006` System Clocking Control（7.1.6，p.114）
复位值 `0x06`。

| Bit | Name | 含义 | 复位 |
|---|---|---|---|
| 7:3 | Reserved | — | 0 |
| 2 | **MCLK_INT** | 内部 MCLK 期望频率：**0 = 24.576MHz，1 = 22.5792MHz（默认）** | 1 |
| 1:0 | **MCLK_SRC_SEL** | MCLK 来源：**00=Direct MCLK/XTAL（直通），01=PLL Mode，10=RCO Mode（默认），11=Reserved** | 2 (0b10) |

> 你问的 0x05/0x06/0x04：
> - **0x06**（bit2=1,bit[1:0]=10）→ **RCO** 模式 + MCLK_INT=22.5792MHz
> - **0x05**（bit2=1,bit[1:0]=01）→ **PLL** 模式 + 22.5792MHz
> - **0x04**（bit2=1,bit[1:0]=00）→ **Direct/XTAL 直通**模式 + 22.5792MHz
> - 频率由 bit2（MCLK_INT）控制：bit2=0 → 24.576MHz；bit2=1 → 22.5792MHz。低两位选来源。
> 板载晶振是 24.576MHz（见 AGENTS.md），若走晶振直通 → MCLK_SRC_SEL=00(直通)、MCLK_INT 需与晶振一致。

---

## 9. `0x020000` Power Down Control（7.1.10，p.115）
复位值：按各 bit 描述 “1=(Default) Powered down” → 全部置 1 = **`0xFE`**（bit0 Reserved=0）。
⚠ 手册图内 “Default” 行有歧义（p.115 图里 bit4/PDN_HP 似为 0 → 0xEE），但**每个 PDN 位描述都写 “1(Default) Powered down”**。**极性是确定的：1=掉电，0=上电。** 复位字节建议按 0xFE（全掉电）理解。

| Bit | Name | 含义 | 复位(按描述) |
|---|---|---|---|
| 7 | **PDN_XSP** | XSP 输入通路：0=上电，1=掉电 | 1 |
| 6 | **PDN_ASP** | ASP 输入通路：0=上电，1=掉电 | 1 |
| 5 | **PDN_DSDIF** | DSD 接口：0=上电，1=掉电 | 1 |
| 4 | **PDN_HP** | HPOUTx 掉电：0=上电(HP 驱动/DAC 激活)，1=掉电（置 1 时音频输出软斜坡到静音） | 1 |
| 3 | **PDN_XTAL** | 晶振：0=上电(XTAL 驱动生成 MCLK)，1=掉电 | 1 |
| 2 | **PDN_PLL** | PLL：0=上电，1=掉电 | 1 |
| 1 | **PDN_CLKOUT** | CLKOUT 输出：0=上电，1=掉电(CLKOUT 拉低) | 1 |
| 0 | Reserved | — | 0 |

> **CS43131_PWR_* 宏校验**（位位置确定，1=掉电）：`PDN_XSP=bit7, PDN_ASP=bit6, PDN_DSDIF=bit5, PDN_HP=bit4, PDN_XTAL=bit3, PDN_PLL=bit2, PDN_CLKOUT=bit1`。
> 上电 = 把要用的模块对应位**清零**（写 0）。例如启用 ASP+PLL+XTAL 并上电 HP：先清 PDN_ASP/PDN_PLL/PDN_XTAL/PDN_HP。

---

## 10. 其余次要寄存器
- **`0x020052` Crystal Setting（7.1.11，p.115）**：bit7:3 Reserved；**bit2:0 = XTAL_IBIAS**（晶振偏置电流）：010=15.0µA，**100=12.5µA(默认)**，110=7.5µA，其它 Reserved。复位值 `0x04`。
  （注意：Quick Reference p.106 把该寄存器标为 `0x020002` 是错的，**详述页为 `0x020052`**，以此为准。）
- **`0x080000` HP Output Control 1（7.5.1，p.127）**，复位值 `0x30`：
  - bit7 HP_CLAMPA：0=默认禁用(PDN_HP 置位且 HP_IN_EN 清零时 HPOUTA 被钳位到地)，1=PDN_HP 置位时释放 HPOUTA 钳位
  - bit6 HP_CLAMPB：同 bit7，对 HPOUTB
  - bit5:4 **OUT_FS**：输出满幅：00=0.5V，01=1V，10=1.41V，11=1.73V(默认)
  - bit3 HP_IN_EN：HPIN 开关：0=默认 断开，1=闭合
  - bit2 HP_IN_LP：HPIN 低功耗：0=默认 非低功耗，1=低功耗（仅当 HP_IN_EN=1；置 1 时 I²C 最高 400kHz）
  - bit1 Reserved
  - bit0 **+18dB_EN**：输出满幅 2V：0=默认 由 OUT_FS 决定，1=2V（仅 OUT_FS=11 且 HV_EN=1 时有效）
- **`0x050000/0x050001` ASP Channel n Location（7.3.22，p.123）**：bit7:0 = ASP_RX_CH1 / ASP_RX_CH2，TDM 帧内通道起始 SCLK 位置，0x00=SCLK0，0xFF=SCLK255，默认 0x00。

---

## 11. I2S/ASP 主从机接口时序/要求（Functional Description 4.9，p.47-49）

### 4.9.1 Master and Slave Timing（p.47）
> - 每个串口既可作时钟主机，也可作其它器件的主机时序从机。**若 xSP_M/S 置位 → 时钟主机；若清零 → 时钟从机**。
> - **主机模式**：xSP_SCLK 和 xSP_LRCK 是**从内部 MCLK 派生出来的输出**。
> - **从机模式**：xSP_SCLK 和 xSP_LRCK 是**输入**。虽然 CS43131 在从机模式不生成接口时序，但**期望的 LRCK 和 SCLK 格式仍须按主机模式同样方式编程**（见 Table 3-20）。
> - 两种模式下，串口采样率寄存器（xSP_SPRATE）都必须在使能串口**之前**按音频内容设定；ASP 播放 PCM 时 ASP_SPSIZE 也须在使能 ASP 前设定。

### 4.9.2 Power-Up, Power-Down, and Tristate（p.47）
> - xSP 有独立的掉电和 tristate（PDN_xSP / xSP_3ST）。xSP 主/从仅由 xSP_M/S 控制，与 PDN_xSP / xSP_3ST 无关。
> - PDN_xSP：若某串口 SDIN 不需要，置 PDN_xSP 断电，关闭该串口输入数据通路与时钟。
> - xSP_3ST：主机模式下置位会三态 SCLK/LRCK。置 xSP_3ST 前必须先断电该串口，且直到 xSP_3ST 清除前不得再上电。**从机模式下 xSP_3ST 不影响 SCLK/LRCK 功能（两者都是输入）**。

### 4.9.3 I/O（p.47）
> - ASP 端口 = SDIN1、SCLK1、LRCK1。XSP 端口 = SDIN2、SCLK2、LRCK2（与 DSD 接口共用）。SCLKx=串行数据移位时钟。

### 4.9.5 Clock Generation and Control（p.48-49，图 4-20/4-21/4-22）
> - 主机模式下，串口产生位时钟（xSP_SCLK）和左/右/帧同步信号（xSP_LRCK/FSYNC）。
> - **SCLK = MCLK × (N/M)**（Fig 4-20：MCLK → N/M 分频 → 极性/主从 mux → SCLK）。
> - **LRCK 周期 = ASP_LCPR（单位 SCLK 周期）**，帧长 16~1536 SCLK；**LRCK 高电平宽度 = ASP_LCHI（单位 SCLK 周期）**，且必须 < LRCK 周期（Fig 4-21/4-22）。ASP_5050=1 强制 50% 占空。
> - 主机模式下，xSP_SCPOL_IN/OUT 和 xSP_LCPOL_IN/OUT 控制 SCLK/LRCK 输出或输入极性（Fig 4-20/4-21）。通常输入 = 输出极性取反。

---

## 12. 引脚说明（Section 1，Table 1-3，p.6）
以 40-pin QFN 引脚号 / WLCSP 小球号列出（本机为 42-ball WLCSP，见 Fig 1-2 p.5）。方向列：I=输入，O=输出，I/O=双向。

| 名称 | QFN# | WLCSP | 类型 | 方向 | 说明 |
|---|---|---|---|---|---|
| XTI/MCLK | 37 | A4 | I | 输入 | 晶振/振荡器输入 或 主 MCLK 输入（内部弱下拉，迟滞 CMOS 输入） |
| XTO | 36 | A3 | O | 输出 | 晶振振荡器输出（弱下拉，CMOS 输出） |
| SCLK1 | 33 | B2 | I/O | 双向 | **串口 1 位时钟（BCLK/SLCK）**：主=CMOS 输出，从=读取（弱下拉） |
| LRCK1 | 38 | B3 | I/O | 双向 | **串口 1 左/右帧时钟（WS/LRCK）**：主=输出，从=读取（弱下拉） |
| SDIN1 | 2 | A6 | I | 输入 | 串口 1 音频数据输入（DIN）（弱下拉，迟滞 CMOS 输入） |
| DSDCLK/SCLK2 | 32 | B1 | I/O | 双向 | 串口 2 位时钟 / DSD 时钟（弱下拉） |
| DSDA/SDIN2 | 40 | A5 | I/O | 双向 | 串口 2 数据 / DSD 音频数据（弱下拉） |
| DSDB/LRCK2 | 29 | C1 | I/O | 双向 | 串口 2 字时钟 / DSD（弱下拉） |
| RESET | 28 | C4 | I | 输入 | 系统复位（低有效，启用电平转换；迟滞 CMOS 输入） |
| SDA | 39 | B4 | I/O | 双向 | 串行控制数据 I²C（CMOS 开漏输出，迟滞输入） |
| SCL | 1 | B5 | I | 输入 | 串行控制时钟 I²C（迟滞输入） |
| INT | 27 | C5 | I/O | 输出 | 中断（开漏低有效，可编程） |
| ADR | 30 | C2 | I | 输入 | I²C 地址位（I²C 从机模式） |
| CLKOUT | 30 | B2 | O | 输出 | 时钟输出（来自 PLL 或缓存晶振） |
| VL / VD / VA | 31/4/7 | A1/C6/A7 | 电源 | — | 逻辑/数字/模拟电源（典型 +1.8V） |

> 注：Fig 1-2 小球位置图（p.5）与 Table 1-3（p.6）在个别 ball 编号上不一致（如 SCLK1/LRCK1 的小球号），**以 Table 1-3 的编号说明为准**；QFN 引脚号与说明为权威。

---

## 13. 未找到 / 待核实（明确标记）
1. **`0x0B0000`（HV 模式）**：寄存器表无此地址。基址表（p.106）只列 0x01/02/04/08/0F 五块，无 0x0B。HV（高压）输出能力由 `0x080000` 的 `OUT_FS`(bit5:4) + `+18dB_EN`(bit0) 及相关模拟块控制，未见独立 0x0B0000 寄存器。**疑似地址记错**。
2. **`0x010010`（pop-free）**：全局寄存器只定义到 `0x01000D`（然后跳到 PLL 0x020000）；0x010010 无对应定义。**pop-free 行为由上电/掉电时序（Section 5）与 0x090003 的 PCM_SZC/PCM_RAMP_DOWN + 音量斜坡实现，非单一寄存器**。**疑似地址记错**。
3. **`0x080032`（HP 控制）**：Headphone/PCM 块有效地址为 0x080000-0x080001（以及 0x090000-0x0900xx）；未见 0x080032。**疑似地址记错**（可能想指 0x080000 或某滤波系数寄存器）。
4. **复位值歧义**：
   - `0x020000`：描述 “全 PDN=1”→0xFE，但图内 Default 行 bit4 似为 0→0xEE。**极性(1=掉电)确定，复位字节请实测**。
   - `0x040018`：bit4(ASP_M/SB) 描述默认=Slave(0)，但图 Default 行 bit4=1。**含义以描述为准：bit4=0=Slave,bit4=1=Master**。
   - `0x05000A/B`：CHn_EN 描述默认=0(禁用)，但 Default 行=1。**要出声需置 EN=1；复位值以 0x06/0x0E 为准**。
   - `0x010006` 复位值按描述=0x06（RCO+22.5792MHz）；若晶振 24.576MHz 走直通，需改为 0x00(直通+24.576) 或 0x02(RCO+24.576)。
