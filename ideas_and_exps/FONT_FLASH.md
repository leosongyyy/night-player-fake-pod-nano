# 手动刷入系统字体（`font` 分区）

> **一句话**：固件里**不含**系统字体。刷完固件（app）之后，还要用 esptool 单独把
> `spiffs/system.ttf` 烧进 flash 的 `font` 分区：
>
> ```powershell
> esptool.exe --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf
> ```
>
> 分区表只**保留位置**（`partitions.csv` 的 `font @ 0x800000, 4MiB`），构建系统不参与
> 字体烧录 —— 2026-10-25 起从原来的 `font-flash` ninja 目标改成手动刷（原因见 §7）。

---

## 一、涉及的东西

| 项 | 值 |
| --- | --- |
| 分区名 / 类型 | `font`，`data` 类型 `subtype 0x41`（自定义子类型），见 `partitions.csv` |
| 分区偏移 | **`0x800000`**（8 MiB 处，64KB 对齐） |
| 分区大小 | **4 MiB**（`0x400000`） |
| 写入内容 | 仓库里的 **`spiffs/system.ttf`** 本身（裸二进制，**没有文件系统**，必须从分区偏移 0 开始） |
| 当前字体 | Droid Sans Fallback（AOSP，Apache-2.0），3,725,920 B = 3.554 MiB（18 张表 / 50951 字形；4 MiB 分区还剩 457 KiB） |
| 固件怎么读它 | `main/core/fonts/font_ttf.c`：`esp_partition_mmap` 整个分区 + 自写 `lv_fs` 驱动，运行期固定按 **`F:/system.ttf`** 打开（路径写死，没有 Kconfig 选项） |
| 相关文档 | `ideas_and_exps/FontSystem.md`（字体方案全过程）、`FontSymbols.md`（内嵌回退字库的字形清单） |

⚠ `spiffs/` 这个目录名是沿用早期资料的习惯，**里面不是 SPIFFS 镜像**，就是裸 TTF 文件本身
（外加一份许可文本 `DroidSansFallback-LICENSE.txt`，它只是仓库文件，**不刷进设备**）。

---

## 二、刷入命令

### 2.1 独立版 `esptool.exe`（推荐，装机的人手边一般都有）

```powershell
# 在项目根目录执行；COM12 换成设备实际串口
esptool.exe --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf
```

- 3.55 MiB 实测 **33.4 秒**（本机 ESP32-S3 内置 USB-Serial/JTAG，约 893 kbit/s；`--baud`
  在这种内置 USB 上影响很小，不加也行）。
- 本机是 ESP32-S3 的**内置 USB-Serial/JTAG**，esptool 会自动让芯片进下载模式，**不需要**
  手动按键或插拔；万一失败，先按住 **B 键**（GPIO0 strapping）再插线，或用 B 键 + 复位。
- 实测输出（esptool 4.12 + USB-Serial/JTAG）—— **这几个数字就是"写入正确"的证据**：

  ```
  Flash will be erased from 0x00800000 to 0x00b8dfff...
  Compressed 3725920 bytes to 1782175...
  Wrote 3725920 bytes (1782175 compressed) at 0x00800000 in 33.4 seconds (effective 893.1 kbit/s)...
  Hash of data verified.
  ```

  写入长度与 `spiffs/system.ttf` **完全一致（3,725,920 B）** → esptool 是按**原始字节**写入、
  **没有加镜像头**（这正是固件能读到 sfnt 头的原因）；末尾的 `Hash of data verified.` 就是
  esptool 写完后的回读校验，**出现这一行即可放心**。
  某些 esptool 版本还会多打一行 `Warning: Image file ... doesn't look like an image file`
  —— 同样是正常的。**不要**用 `elf2image`、也不要先做成 SPIFFS 镜像再刷（见 §6）。
- 若你的 esptool 是 **v5 及以上**且提示不认识 `write_flash`，把命令里的下划线换成连字符
  `write-flash`，参数完全一样。

### 2.2 用 ESP-IDF 自带的 esptool（没有独立 exe 时）

先按 `AGENTS.md` 激活 IDF 环境（本机 `C:\Espressif\tools\Microsoft.v5.5.X.PowerShell_profile.ps1`），然后：

```powershell
esptool.py --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf
# 或者不依赖 PATH：
python -m esptool --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf
```

⚠ 这里是**直接用 esptool**，不是 `idfc`：自从去掉 `font-flash` 目标后，`idfc` 侧已没有
任何"刷字体"的命令（`idfc idfpy ...` 也刷不了它）。好处是刷字体**不再需要 IDF 服务器、
不再需要 idf.py**，有 esptool 就行。

> 本机（开发机）没有独立的 `esptool.exe`，实测用的就是 IDF 自带那份，等价于激活 IDF 环境后的
> `esptool` 命令：
> ```powershell
> & "C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe" `
>   "C:\esp\v5.5.5\esp-idf\components\esptool_py\esptool\esptool.py" `
>   --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf
> ```
> （版本 `esptool.py v4.12.0`；实测 33.4 秒、MD5 与源文件一致，见 §2.1 / §4.2。）

### 2.3 完整装机顺序（新机器第一次）

```powershell
idfc flash                                                    # 1) 固件（bootloader + 分区表 + app），约 20 秒
esptool.exe --chip esp32s3 --port COM12 write_flash 0x800000 spiffs/system.ttf   # 2) 字体，约 30~40 秒
idfc monitor --timeout 15000                                  # 3) 看启动日志确认（§4）
```

两步的**顺序无所谓**（分区表在两个命令里都会正确落盘），但**两步都要做**。

> 发布固件时请把 `spiffs/system.ttf` 和 app 一起放进发布包（例如与
> `FakePodNanoNightPlayer.bin` 并列），否则装机的人会刷出"能开机但生僻字全变方框"的固件。

---

## 三、什么时候必须刷，什么时候不用

| 情况 | 要刷字体吗 |
| --- | --- |
| **第一次装本固件**（font 分区还是 `0xFF`） | ✅ **必须** |
| **换字体**（用新字体文件改名覆盖 `spiffs/system.ttf`，见 §6） | ✅ **必须** |
| 改了 `partitions.csv`，且 **`font` 分区起始偏移变了** | ✅ **必须**（旧数据在新偏移处读不到） |
| 改了 `partitions.csv`，但 `font` 起始偏移没变（只是大小变了，例如 8M→4M） | ❌ 不用（数据原样有效） |
| 平时 `idfc flash` 更新固件（app 改动、调界面、修 bug） | ❌ 不用 —— `idfc flash` **有意不含字体**，否则每轮烧录都要多花 30~40 秒 |
| 用 `font=binfont` 维度构建的固件（字库编在固件里，根本不用 font 分区） | ❌ 不用（该维度下分区闲置） |
| 只想清空/复位字体分区 | 直接重刷一遍字体即可（没有"删除"的概念） |

⚠ **注意 `0x800000` 与分区表的一致性**：偏移来自 `partitions.csv` 的 `font` 行，
改了分区表就要回到本文档改命令里的偏移。可用下面命令核对分区表（刷完后读回）：
`esptool.exe --chip esp32s3 --port COM12 read_flash 0x8000 0x1000 pt.bin`，
再用 IDF 的 `gen_esp32part.py pt.bin` 打印。

---

## 四、怎么确认刷成功了

### 4.1 首选：启动日志（ttffont 维度的固件）

字体是**延迟加载**的（只有走"正常进主界面"这条分支才加载，见 `FontSystem.md` §9 第 8 条），
所以要在进了主界面之后的那段日志里找：

```
I (1130) app_main: Run boot flow                                    ← 字体是延迟加载：从这里之后才加载
I (1134) font_ttf: 字体信息：TrueType(glyf)，18 张表，50951 个字形
I (1141) font_ttf: font 分区：offset=0x800000 size=4194304 B；字体文件实际长度=3725920 B（3.55 MiB）
I (1153) font_ttf: font 分区已 mmap 到 0x3c800000（4194304 B），字形读取走 memcpy
I (1174) font_ttf: TTF 字体就绪：F:/system.ttf 24px（行高 31，基准线 7）
I (1175) font_ttf: TTF 字体就绪：F:/system.ttf 20px（行高 26，基准线 6）
I (1181) font_ttf: 系统字体已切换到 TTF（FreeType，字形按需从 font 分区读取）
```

（2026-10-25 实测原样摘录；时间戳与 mmap 地址每次开机不同。）

- **最后一行 `系统字体已切换到 TTF` 就是"字体生效"的确证**，上面那两行 "TTF 字体就绪：… 24px/20px"
  说明两个字号都建好了。
- **表数 / 字形数（当前 18 张表、50951 个字形）**可直接核对刷进去的是哪一份字体
  （两个数都取自 TTF 头部，换了字体就会变）。
- "字体信息"排在"font 分区：…"之前是正常的：前者由探测函数 `probe_font_file()` 打印，
  后者在它返回之后才打印。

⚠ 若用 `idfc monitor`，注意构建单元要选对（`idfc monitor -o font=ttffont`），
否则会出现 "Checksum mismatch between flashed and built applications" 之类的干扰信息。

### 4.2 备选：读回比对（不看设备界面时用）

**先试最快的：只读头部 16 字节**（0.1 秒），确认分区里确实躺着一个 sfnt 字体：

```powershell
esptool.exe --chip esp32s3 --port COM12 read_flash 0x800000 0x10 head.bin
Format-Hex head.bin
# 正常（当前 Droid Sans Fallback）：
#   00 01 00 00 00 12 01 00 00 04 00 20 47 44 45 46
#   ↑sfnt 版本 1.0  ↑18 张表        ↑第一个表的 tag "GDEF"
# 全 FF = 分区是空的（还没刷过字体）
```

**要逐字节确认才这样做**（PASS/FAIL 明确，但很慢）：

```powershell
# 按当前字体的实际长度读回（3,725,920 B = 0x38DA60）
esptool.exe --chip esp32s3 --port COM12 read_flash 0x800000 0x38DA60 readback.ttf
(Get-FileHash spiffs/system.ttf -Algorithm MD5).Hash -eq (Get-FileHash readback.ttf -Algorithm MD5).Hash
# → True 即一致
```

- ⚠ 全量读**实测 324 秒**（3.55 MiB；USB-Serial/JTAG 下只有约 91.9 kbit/s，比写入慢一个数量级），
  而且本机第一次读时撞上过一次 `A fatal error occurred: Corrupt data, expected 0x1000 bytes
  but received 0xfbc bytes` —— **重试一次即可**（重试后 MD5 与源文件完全一致）。
  所以日常校验优先用**启动日志**（§4.1）或上面的头部 16 字节。
- ⚠ 读回时**别把整个 4 MiB 分区读出来**再和字体文件比长度：分区尾部是 `0xFF` 填充，
  长度不同、内容只在字体文件长度内一致（所以上面按 `0x38DA60` 读）。
- 其实 `write_flash` 结束时的 `Hash of data verified.` 本身就是一次完整回读校验（见 §2.1），
  正常刷完不必再手工读回。

---

## 五、没刷 / 刷坏了会怎样

**不会变砖，也不会开不了机**：字体加载失败是**软失败**，界面会自动回退到编译进固件的
**内嵌位图字体**，日志里能明确看到原因：

```
E (xxxx) font_ttf: font 分区里不是 TTF/OTF（头 4 字节 = 0xffffffff）；0xFFFFFFFF 表示分区是空的 —— 刷完固件后还要用 esptool 把 spiffs/system.ttf 烧到 0x800000
E (xxxx) font_ttf: font 分区校验失败，回退内嵌位图字体（重刷字体分区可修复：esptool write_flash 0x800000 spiffs/system.ttf）
```

- 现象：界面正常可用，但**歌曲名/目录名里的生僻字、日文、韩文等会显示成方框**
  （回退字库只含系统 UI 用字 + ASCII；`ttffont` 维度下回退用的是精简版
  `system_global_font_lite`）。
- 处置：按 §2 重刷字体，重启即可，不需要重刷固件。
- 刷到一半断电/断线：同样只是"分区头不对 → 回退位图字体"，**重刷一遍**就好
  （esptool 的写入是按 4KB 扇区擦写的，重刷会覆盖整个字体范围）。

---

## 六、换字体（改名覆盖 + 重刷）

字体在固件里**没有名字也没有 Kconfig 选项**：运行期永远按 `F:/system.ttf` 打开，
所以换字体**不用改任何代码**：

1. 先确认新字体文件 **≤ 4 MiB**（分区上限；当前字体 3.55 MiB 已占 89%）。
   超了就得改 `partitions.csv`：调大 `font` 并**等量缩小 `factory`**，
   `pldata` 不用动（它锚定在分区表最末尾，见该文件注释）。
2. 把新字体**改名成 `system.ttf` 覆盖** `spiffs/system.ttf`（建议先把旧的备份到仓库外）。
3. 按 §2 刷入（约 30~40 秒），按 §4 核对日志里的**字形数**是不是新字体的。
4. ⚠ **复核行高**：新字体的 `line_height` 会影响列表行高与布局（`virtual_list` 用
   `lv_font_get_line_height` 算行高；`settings_page.c` 里那个两行文字图标用的是
   `text_line_space=-6`，是按 20px 行高 26 调的）。换字体后如果界面挤了/空了，先查这里。
5. ⚠ 若同时维护 `font=binfont` 维度（内嵌位图字库），记得它的字形清单是**另一套**，
   要按 `ideas_and_exps/FontSymbols.md` 重新生成 `.c` 文件。

**为什么不能用 SPIFFS/文件系统**：字体只需一个文件、只读、按偏移直接 mmap 最快；
装文件系统要多占 0.4~0.8 MiB 分区、还要每轮构建生成镜像、运行时挂载扫描。
实测与选型记录见 `FontSystem.md` §2。

---

## 七、为什么不再用 `font-flash` ninja 目标（2026-10-25 改版）

旧方案：`main/CMakeLists.txt` 里

```cmake
esptool_py_flash_target(font-flash ...)
esptool_py_flash_to_partition(font-flash "font" spiffs/system.ttf)
```

注册出一条 `idfc idfpy font-flash` 命令，好处是"不用记偏移"。实际用起来有两个问题：

1. **太重**：字体是"装一次就不动"的数据，为它维护一条 CMake/ninja 目标、还要在文档里
   专门解释它，性价比太低；而装机的人手边本来就有 esptool，一条命令更直白。
2. **端口传不进去**（当时的实测坑，现已不相关，留作记录）：`idf.py <自定义目标>` 只是把
   它当 **ninja 目标**跑，而 esptool 的端口是**烧录那一刻**才读环境变量 `ESPPORT`
   （`components/esptool_py/get_port_args.cmake` 故意不在配置期展开，好让端口不用重配就能改）。
   于是：

   | 做法 | 结果 |
   | --- | --- |
   | `idfc idfpy -p COM12 font-flash` | ✗ `-p` 传不进去，esptool 逐个串口试 |
   | 客户端 `$env:ESPPORT="COM12"` 后 `idfc idfpy font-flash` | ✗ 仍不行 —— ninja 是 **idfc 服务器**拉起的，客户端环境变量传不过去 |
   | 给 idfc 服务器进程设 `ESPPORT` 再 `start-server` | ✓ 可用但很绕 |

   直接调用 esptool 没有这个问题：端口是自己命令行里给的。

现在 `main/CMakeLists.txt` 里那段只剩一个构建期 `message(STATUS ...)` 提示（提醒"字体要手动刷"），
分区表与运行期代码都没有变化 —— 也就是说**这个改版对固件本身零影响**。

---

## 八、常见问题

| 现象 | 原因 / 处置 |
| --- | --- |
| `could not open port 'COM12': PermissionError(13, '拒绝访问。')` / 端口被占用 | 串口被别的程序占着：先关掉 `idfc monitor`、串口助手、Arduino 监视器等 |
| `Failed to connect to ESP32-S3: No serial data received` | 芯片没进下载模式：按住 **B 键**（GPIO0）再插线/复位；或换一根**带数据线**的 USB 线 |
| 日志里出现 `Warning: Image file ... doesn't look like an image file` | **正常**：说明按原始字节写入、没加镜像头（本机 esptool 4.12 实测不会打这行，老版本才有） |
| `read_flash` 报 `A fatal error occurred: Corrupt data, expected 0x1000 bytes but received ...` | USB-Serial/JTAG 上读大块偶发失败：**重试**即可；日常只读头部 16 字节（§4.2） |
| `read_flash` 读 3.55 MiB 要五分多钟 | 正常：USB-Serial/JTAG 的读取只有约 92 kbit/s（写入是 893 kbit/s），比写入慢一个数量级 |
| 刷完仍缺字变方框 | 分区偏移或文件长度不对（按 §4.2 读回比对）；或字体文件本身不含那些字 |
| 想确认设备里到底是哪份字体 | 启动日志的**表数 / 字形数**（当前 18 张表、50951 字形）；也可按 §4.2 读回算 MD5 |
| 只想验证"没有字体也能用" | 用 `idfc build -o font=binfont` 构建内嵌位图字库版本，它完全不碰 font 分区 |
