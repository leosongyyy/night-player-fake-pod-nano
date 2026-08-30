# CS43131 音频调试中间结果文档

> 本文件是 CS43131 音频调试的中期记录，供新会话快速接手。最后更新：当前会话末（已能出声但波形为锯齿）。

## 一、项目目标

实现正弦波播放界面：44.1kHz/48kHz × 500Hz/1000Hz 四种组合 + 停止按钮 + 音量滑块。
音频 API 模块位于 `main/core/hardware/audio/`（`audio.h/.c` + `cs43131.h/.c`）。

## 二、硬件连接（已确认）

| 信号 | 连接 |
|---|---|
| GPIO41 → CS43131 RESET# | 3.3→1.8V 电平转换，低有效；RESET# 有 10kΩ 上拉到 1.8V |
| GPIO38 = LRCK/WS | I2S 帧时钟（当前为 ESP32 主机输出） |
| GPIO39 = DIN | I2S 数据输出 |
| GPIO40 = BCK | I2S 位时钟（当前为 ESP32 主机输出） |
| MCLK | 板载 24.576MHz 晶振接 CS43131 XTI/XTO（无源晶振） |
| I2C | 与触摸共用 GPIO2/3（SCL=2/SDA=3），经 **TXS0102DCUR** 电平转换：VCCA=1.8V 接 CS43131 侧（A 端口），VCCB=3.3V 接 ESP32 侧（B 端口），OE 接 1.8V |
| CS43131 供电 | VP=3.3V（VCC），VL/VD/VA 等 1.8V，固定供电 |
| 注意 | **CS43131 侧（1.8V）SCL/SDA 无外部上拉**（依赖 TXS0102 内部弱上拉） |

## 三、关键发现（本会话最大成果）

### 1. CS43131 的 I2C 地址是 0x30（不是 0x20！）

- 实测 0x30 读 ID 寄存器 = **0x43 0x13 0x10**（手册确认 CS43131 ID）
- **0x15 = CST820 触摸**（`esp_lcd_touch_cst820.h` 中 `ESP_LCD_TOUCH_IO_I2C_CST820_ADDRESS = 0x15`）
- **0x30 = CS43131**（之前一直误以为是触摸！）
- 0x6b = SY6970 充电 IC（读 0x010000 返回 0x7B ≠ CS43131 ID）、0x7e = 电量计/其它
- 之前所有"probe 0x20 NACK"的排查都是地址错了！

### 2. I2C 帧格式（原厂固件逆向确认）

- **写帧：5 字节 `[addr_hi][addr_mid][addr_lo][0x00][data]`**（第 4 字节固定 **0x00**）
  - 参考驱动（`reference_project/cs43131_espidf-main/`）用的 **0x01 是错的**（0x01 应为自动递增标志）
  - 4 字节帧 `[addr3][data]` 写"返回成功"但**寄存器值不生效**（实测读回验证）
- **读帧：4 字节 `[addr_hi][addr_mid][addr_lo][0x00]`** + 读 1 字节

### 3. I2C 速度

- 原厂固件用 **400kHz**；我们最终用 **200kHz**（400kHz 在 TXS0102 上长时间工作会 I2C 超时/卡死，触摸也受影响）

### 4. PLL 参数（手册 Table 4-5 + 原厂固件表一致）

- 44.1kHz：24.576MHz → 22.5792MHz：PREDIV=8(0x03), INT=0x49, FRAC=0x800000, OUT_DIV=10(0x0A), MODE=1, CAL=118(0x76)，MCLK_SRC=0x05
- 48kHz：24.576MHz → 24.576MHz：PREDIV=8(0x03), INT=0x40, FRAC=0, OUT_DIV=8(0x08), MODE=1, CAL=128(0x80)，MCLK_SRC=0x04（原厂固件此条 PLL 参数全 0，疑似 MCLK 直通）

### 5. 0x01000C（样本位宽）——参考驱动编码是错的！

- 参考驱动：32→0, 24→1, 16→2, 8→3
- **原厂固件写 4 或 6**（`a5==32 ? 4 : 6`）！当前代码用 **0x04**
- 若锯齿问题未解决，需查手册 0x01000C 真实位定义

### 6. ⚠ 最重大的待验证点：ASP 主从方向！

原厂固件写 **0x040010=1、0x040011=0、0x040012=8、0x040013=0、0x040014=31**：
- 疑似 **ASP Master 模式分频器**：24.576MHz/8 = 3.072MHz = 64×48kHz（BCK），LRCK = BCK/64 = 48kHz
- **如果 CS43131 是 ASP MASTER（自己产生 BCK/LRCK），ESP32 应为 I2S SLAVE**（I2S_ROLE_SLAVE，GPIO38/40 变输入）
- 我们当前把 CS43131 配成 slave（0x040018=0x0c bit4=0）+ ESP32 主机输出时钟 → **可能时钟冲突导致锯齿波/卡死**
- **下一步：查手册 0x040010-0x040014 定义 + 0x040018 的 ASP_M/SB 位**，若为 master 分频器则改造：
  - CS43131：0x040018 设 master（bit4=1）+ 写分频器 1/0/8/0/31
  - ESP32：I2S slave 模式（i2s_new_channel(I2S_ROLE_SLAVE)），只输出 DIN
  - 采样率由 CS43131 内部时钟决定（PLL 切 22.5792 时 44.1k；直通时 48k）

### 7. 其它原厂固件配置

- 0x020052 = 4（晶振设置，我们写 2，可改）
- 0x0F0000 读（中断标志）、0x0F0010 = 0xE7（写）
- GPIO41 复位：拉低→延时→拉高→延时（与我们一致）
- 原厂 I2C 初始化：new i2c driver，bus 结构体 0x3FCA80D4，add_device 参数 a11=3/a12=2（SDA=3/SCL=2）、clk=0x61A80=400kHz

## 四、当前代码状态（能编译、能出声但锯齿）

| 文件 | 状态 |
|---|---|
| `main/core/hardware/audio/cs43131.c` | I2C 地址 0x30、5 字节写帧(0x00)、4 字节读帧、PLL 动态切换、init 序列、读回验证日志、候选地址探测（可简化） |
| `main/core/hardware/audio/audio.c` | I2S 主机 32bit slot、BCLK 反相(bclk_inv=true)、mclk 256×fs、正弦查表(sine<<16)、i2s enable 状态跟踪 |
| `main/core/hardware/touch.c` | I2C **200kHz** |
| `main/main.c` | 开头有 early-probe 诊断代码（可删） |
| `main/ui/main_ui.c` | 播放界面（4 按钮+停止+滑块+状态标签） |
| 其它修复 | LVGL 任务栈 8K、mem_monitor 栈 4K、audio_tone 缓冲 static |

## 五、实测结果

1. **I2C 完全打通**：CS43131 ID=0x43/0x13/0x10 @0x30；读回验证：0x020000=0xA2（XTAL/PLL/ASP/HP 全部上电）、0x010006=0x04（PLL+24.576）、0x090003=0x0C（不静音）、0x090001=0x20（音量）、0x05000A=0x07（32bit slot）——**全部正确**
2. **能出声**（44.1k 和 48k 都出声）——I2C+I2S+CS43131 链路基本打通
3. **问题 A：波形像锯齿波而非正弦波**（44.1k/48k 均如此）
4. **问题 B：播放 500Hz 时"kata"一声后播放卡死**，后续所有播放无效（疑似 CS43131 时钟/总线异常，可能与 AMUTE 自动静音 0x090003 bit3=1 或 I2S 停止时序有关）
5. **问题 C：播放 1kHz @44.1k 和 @48k 音调听起来相同**（这可能意味着采样率切换已经生效）

## 六、下一步建议（按优先级）

1. **查手册 0x040010-0x040014 与 0x040018 定义**，确认 CS43131 ASP 主从方向；若为 master：
   - CS43131 配 master + 分频器 1/0/8/0/31
   - ESP32 I2S 改 slave（I2S_ROLE_SLAVE，GPIO38/40 输入）
   - 采样率由 CS43131 决定（44.1k 需 PLL 22.5792；48k 直通）
2. **解决锯齿波**：确认 0x01000C 位定义（原厂用 4/6）；数据在 32bit slot 中的对齐（当前 sine<<16，可试 sine、sine<<8）
3. **解决 500Hz kata 卡死**：检查 AMUTE（可写 0x090003 bit3=0 关闭自动静音）、I2S 停止/启动时序、PLL 切换稳定性
4. **确认采样率切换**：播放后读回 0x010006/0x01000B 验证；I2S 实际 fs 与 CS43131 匹配
5. **清理代码**：删 main.c early-probe、cs43131.c 候选地址循环（固定 0x30）、读回验证日志可选保留

## 七、参考资料

- 原厂固件（逆向来源）：`official_firmware/固件cs43131_1.1.2_播放器版_单文件/firmware.bin`（1.65MB，IROM 段 file_offs 0xB0018 / vaddr 0x42000020；CS43131 驱动代码约 0x420089C8-0x42008E00，常量池 0x420006xx/0x420007xx，PLL 表 file 0x500-0x6D0）
- 反汇编方法：`xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=0x42000020 <提取的IROM段>`（IROM 段 = firmware.bin[0xB0018..0xB0018+0xCB1F8]）
- 手册转写：`cs43131_datasheet_screenshot/`（PLL 公式/参数表、I2C 时序、I2C 从口特性、0x05000A/B、0x010006、0x040018/19、0x01000D 等）
- 参考驱动（⚠ 0x01 帧字节与 0x01000C 编码是错的）：`reference_project/cs43131_espidf-main/`
- 手册补充：0x090003 默认不静音（bit1/0=0），bit3=1（AMUTE 自动静音）；0x01000C 默认 24bit；ID=0x43/0x13/0x10
