# 手动刷入系统字体（`font` 分区，NPF1 全字库）
> 本文对应 **`master`**（ESP32-S31 + LVGL 9.4 + IDF 6.1）。字体方案的设计、格式与生成见
> [NPF_FONT.md](NPF_FONT.md)；`legacy_s3` 的同一套 NPF1 方案（ESP32-S3 + LVGL 8.4）除
> `--chip esp32s3` 与 esptool 路径外，流程与本文一致。

> 💡 **先看这里**：本文讲的是**日常 `idfc flash` 之后单独补刷字体**。如果刷的是
> `dist/` 里的**全量固件**（`tools/make_full_firmware.py` 生成，烧到 `0x0`），
> 字体已经在镜像里，**不用再执行本文任何命令**。全量固件说明见
> [`FULL_FIRMWARE.md`](FULL_FIRMWARE.md)。

> **一句话**：固件里**不含**系统字体。刷完 app 之后，还要用 esptool 单独把
> `spiffs/system.npf` 烧进 flash 的 `font` 分区：
>
> ```powershell
> python -m esptool --chip esp32s31 -p COM3 -b 1500000 write_flash 0x600000 spiffs\system.npf
> ```
>
> 分区表只**保留位置**（`partitions.csv`：`font @ 0x600000, 6MiB`），构建系统不参与
> 字体烧录 —— `idfc flash` **有意不含字体**（4.44MiB，每轮多烧 30~40 秒不值）。

---

## 一、涉及的东西

| 项 | 值 |
| --- | --- |
| 分区名 / 类型 | `font`，`data` 类型 `subtype 0x41`，见 `partitions.csv` |
| 分区偏移 | **`0x600000`**（6 MiB 处，64KB 对齐，供 `esp_partition_mmap`） |
| 分区大小 | **6 MiB**（`0x600000`） |
| 写入内容 | 仓库里的 **`spiffs/system.npf`**（NPF1 裸镜像，**没有文件系统**，必须从分区偏移 0 开始） |
| 当前字体 | Droid Sans Fallback（AOSP，Apache-2.0），24px/bpp2，34,461 个码位，**4,653,106 B = 4.44 MiB**（6 MiB 分区余约 1.34 MiB） |
| 固件怎么读它 | `main/core/fonts/font_npf.c`：`esp_partition_mmap` 镜像长度 + 填指针，字形位图/`glyph_dsc` 全程留在 Flash |
| 相关文档 | [NPF_FONT.md](NPF_FONT.md)（方案与格式）、`tools/font/README.md`（生成工具） |

⚠ `spiffs/` 这个目录名是沿用早期资料的习惯，**里面不是 SPIFFS 镜像**，就是裸字体文件
（外加 `DroidSansFallback-LICENSE.txt`，只是仓库文件，**不刷进设备**）。

---

## 二、刷入命令

### 2.1 用 IDF 自带 esptool（本机方式，推荐）

```powershell
# 在项目根目录执行；COM3 换成设备实际串口
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
    --chip esp32s31 -p COM3 -b 1500000 write_flash 0x600000 spiffs\system.npf
```

- 4.44 MiB 实测 **28.9 秒**（ESP32-S31 USB-Serial/JTAG，约 1288 kbit/s）。
- S31 的 USB 会自动进下载模式，**不需要**手动按键；万一失败，重新插线或按住板上的
  BOOT/电源键再上电。
- 实测输出（esptool 5.6）—— **这几个数字就是"写入正确"的证据**：

  ```
  Wrote 4653108 bytes (2236146 compressed) at 0x00600000 in 28.9 seconds (effective 1287.6 kbit/s)...
  Hash of data verified.
  ```

  `Wrote` 的字节数比源文件多 2（esptool 按 4 字节对齐补 `0xFF`），属正常；
  `Hash of data verified.` 是回读校验，**出现这一行即可放心**。

### 2.2 独立版 `esptool.exe`

参数完全一样：

```powershell
esptool.exe --chip esp32s31 --port COM3 write_flash 0x600000 spiffs/system.npf
# esptool v5+ 把 write_flash 改成了 write-flash，参数不变
```

⚠ 这里是**直接用 esptool**，不是 `idfc`：`idfc` 侧没有任何"刷字体"的命令
（`idfc idfpy ...` 也刷不了它）。好处是刷字体**不需要 IDF 服务器**。

### 2.3 完整装机顺序（新机器第一次）

```powershell
idfc flash -t release          # 1) bootloader + **分区表** + app，约 25 秒
# 2) 字体（首次/换字体才需要），约 30 秒
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
    --chip esp32s31 -p COM3 -b 1500000 write_flash 0x600000 spiffs\system.npf
idfc monitor --wait-for "App started"   # 3) 看启动日志确认（§四）
```

两步的**顺序无所谓**（分区表在 `idfc flash` 里就会正确落盘），但**两步都要做**。

> 发布固件时请把 `spiffs/system.npf` 与 app 一起放进发布包，否则装机的人会刷出
> "能开机但用户数据里的生僻字全变方框"的固件（UI 固定文案有内嵌兜底字库，不会白屏）。

---

## 三、什么时候必须刷，什么时候不用

| 情况 | 要刷字体吗 |
| --- | --- |
| **第一次装本固件**（font 分区还是 `0xFF` 或还是旧 TTF） | ✅ **必须** |
| **换字体/改字号**（重新生成 `spiffs/system.npf`，见 NPF_FONT.md §4） | ✅ **必须** |
| 改了 `partitions.csv`，`font` 起始偏移变了（例如这次 0x800000 → 0x600000） | ✅ **必须** |
| 改了 `partitions.csv`，但 `font` 起始偏移没变（只改大小） | ❌ 不用（数据原样有效） |
| 平时 `idfc flash` 更新 app | ❌ 不用（有意不含字体） |
| 只想"复位"字体分区 | 重刷一遍即可（没有删除的概念） |

⚠ **`0x600000` 与分区表必须一致**：偏移来自 `partitions.csv` 的 `font` 行，改了分区表就回来改命令。
核对办法（读回分区表再用 IDF 解析）：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
    --chip esp32s31 -p COM3 read_flash 0x8000 0x1000 pt.bin
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" `
    C:\esp\v6.1\esp-idf\components\partition_table\gen_esp32part.py pt.bin
```

---

## 四、怎么确认刷成功了

### 4.1 首选：启动日志

字体在 `main()` 里紧跟 `lvgl_port_init` 之后挂载（早于开机封面），所以**开机很早就打印**：

```
I (1072) font_npf: NPF1 全字库已挂载: 24px bpp2, 34462 个字形, 183 个 cmap, 镜像 4653106B @0x600000, 行高 32
I (1083) system_font: 系统字体：NPF1 全字库（34462 个字形）
```

- `已挂载 … 34462 个字形 / 183 个 cmap / 镜像 4653106B` 就是"字体生效"的确证；
- 刷错/没刷时会看到 `W (…) font_npf: font 分区里不是 NPF1 镜像（多半是旧 TTF 或没刷字体），回退内嵌字库`
  以及 `system_font: 使用内嵌精简字库兜底` —— 能开机，但生僻字缺字；
- 时间戳每次开机不同，mmap 地址不打印（走 `esp_partition_mmap` 的 data 映射）。

### 4.2 备选：读回比对（不看设备时用）

**只读头部 4 字节**（0.1 秒）就够判断"里面是不是 NPF1"：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
    --chip esp32s31 -p COM3 read_flash 0x600000 0x10 head.bin
Format-Hex head.bin
# 正常：4E 50 46 31 01 00 01 00 00 00 18 00 xx xx xx xx
#       ↑"NPF1"   ↑ver=1 ↑faces=1        ↑crc32 开头
# 全 FF = 分区是空的（还没刷字体）；00 01 00 00… = 还是旧 TTF（TTF 时代刷的）
```

**要逐字节确认才这样做**（读回比写入慢一个数量级，4.44MiB 约 5~8 分钟）：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
    --chip esp32s31 -p COM3 read_flash 0x600000 0x470032 readback.npf
(Get-FileHash spiffs\system.npf -Algorithm MD5).Hash -eq (Get-FileHash readback.npf -Algorithm MD5).Hash
# → True 即一致（0x470032 = 4,653,106 = 当前镜像长度；换字体后按 npf_verify.py 打印的长度改）
```

---

## 五、出错表现与排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 开机日志 `回退内嵌字库` | 分区里不是 NPF1 / 长度不对 / mmap 失败 | 按 §二 重刷；确认 `partitions.csv` 与命令偏移一致 |
| 界面固定文案正常、**歌名几乎全是方框** | 正是"回退到精简字库"的表现 | 同上 |
| 开机就重启/花屏 | 分区表与固件不匹配（`factory` 越界） | `idfc flash` 重刷（含分区表），必要时整片擦除 |
| `esptool` 写失败 | 串口被 `idfc monitor` 占用 | 先关掉 monitor 窗口/进程再刷 |
| 写入后仍报 `回退`，读回头部是旧数据 | 写到了旧偏移（0x800000） | 检查命令里的地址应为 `0x600000` |

---

## 六、更换字体 / 改字号（2026-11 起）

字体不再"改名覆盖"就行，必须**重新预烘焙 NPF1 镜像**（因为运行期不做任何解析）：

1. 准备新 TTF 或改 `--size` / `--bpp`（bpp 必须是 **2**，见 NPF_FONT.md §7）；
2. 按 [NPF_FONT.md](NPF_FONT.md) §4 的四条命令生成 `spiffs/system.npf`；
3. `python tools/font/npf_verify.py spiffs/system.npf --ttf <ttf>` 交叉校验通过；
4. 确认镜像长度 < 6 MiB（超了就调大 `font` 并等量缩小 `ota_0`，**`nvs`/`phy_init` 不要动**）；
5. 刷 `0x600000`（§二），看 §四 的日志。

> 只改 `24px` 的字形来源而不改 bpp 时，`glyph_dsc` 布局不变，**固件不用重编**——
> 换 `.npf` 即可。相反，如果改了 LVGL 大版本或 `LV_FONT_FMT_TXT_LARGE`，
> `font_npf.c` 里的 `_Static_assert` 会在编译期直接报错，必须重新核对格式。

---

## 七、为什么不放进构建系统

历史上 2026-10 之前由 `main/CMakeLists.txt` 的 `font-flash` ninja 目标代刷，问题：

- 每轮 `idfc build` 之后都可能触发一次 40 秒的字体写入，调试迭代极慢；
- 字体与固件版本被绑死，换字体要动构建脚本；

2026-10-25 起改成**手动一条 esptool 命令**，权责清晰：`idfc flash` 只管 app，
字体只在"首次/换字体/分区偏移变了"时补刷。本次（2026-11-11）改用 NPF1 后
这一段流程**没有变**，只是文件与偏移换成 `spiffs/system.npf` / `0x600000`。
