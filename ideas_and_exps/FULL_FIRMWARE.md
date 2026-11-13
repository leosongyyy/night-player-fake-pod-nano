# 全量固件（单文件、烧 0x0 一次完成整片更新）

> **一句话**：把 `bootloader.bin` + `partition-table.bin` + 应用固件 + `spiffs/system.npf`
> 按分区表偏移拼成一个 **16 MiB** 的 bin，整片烧到 **0x0** 即可完成更新，并**顺带清空
> nvs 与 pldata**（配置和播放列表都不保留）。字体（NPF1 位图全字库）已包含，
> 不需要再单独刷 `font` 分区。

产物示例：`dist/FakePodNanoNightPlayer_v1.0.0.0_release_full_20261105.bin`（16,777,216 B）。

---

## 一、镜像构成

| 偏移 | 大小 | 内容 | 来源 |
| --- | --- | --- | --- |
| `0x000000` | 15,536 B | bootloader | `build/<cell>/bootloader/bootloader.bin` |
| `0x008000` | 3,072 B | 分区表 | `build/<cell>/partition_table/partition-table.bin` |
| `0x009000` | 24 KiB | `nvs` —— **0xFF（清空）** | 填充 |
| `0x00F000` | 4 KiB | `phy_init` —— **0xFF（清空）** | 填充 |
| `0x010000` | 2,195,872 B | 应用固件 `FakePodNanoNightPlayer.bin` | `build/<cell>/` |
| `0x600000` | 4,653,106 B | 系统字体 `system.npf`（NPF1 位图全字库，裸镜像） | `spiffs/system.npf` |
| `0xC00000` | 4 MiB | `pldata` —— **0xFF（清空）** | 填充 |
| 其余空隙 | — | 0xFF（擦除态） | 填充 |

镜像总长 = flash 大小 = **16 MiB**，最后一段 `pldata` 正好顶到 `0x1000000`。

## 二、为什么这样就能"清数据"

ESP32 擦除后的 flash 读出来就是 `0xFF`。合并时整片先填 `0xFF`，只把四段真实数据覆盖
进去，于是：

- `nvs`（0x9000）和 `pldata`（0xC00000）在镜像范围内但内容全是 `0xFF`；
- `esptool write_flash 0x0 <file>` 会擦除并写入 `0x0` ~ `0xFFFFFF` 整片，
  旧 nvs / pldata 的字节被 `0xFF` 覆盖。

所以烧完后的状态与"先 `erase_flash` 再分区烧录"等价，但只需一个文件、一条命令。
`phy_init` 也一并清空，首次启动会重新做射频校准（本固件不用 WiFi/BT，无影响）。

## 三、生成

脚本：`tools/make_full_firmware.py`（纯标准库，不依赖 esptool）。

```powershell
idfc build -t release                                  # 先构建目标配置
python tools/make_full_firmware.py -t release          # 生成 dist/<项目名>_release_full.bin
```

常用参数：

```powershell
python tools/make_full_firmware.py -t release -o dist\full_20261105.bin   # 指定输出名
python tools/make_full_firmware.py -t debug                               # 另一种构建类型（debug）
python tools/make_full_firmware.py -t debug --build-dir build\debug       # 指定构建目录
```

> ⚠ 2026-11 起构建矩阵只剩 `variant` 一个维度（`--dac-chip` / `--font` 参数已随
> `dac_chip` / `font` 维度一起移除），构建目录就是 `build/<variant>`。

脚本不写死偏移：bootloader/分区表/app 的偏移来自构建目录的 `flasher_args.json`，
`font` / `pldata` / `factory` 的位置和大小来自仓库根目录的 `partitions.csv`，
flash 大小取自 `flasher_args.json` 的 `flash_settings.flash_size`。
**改分区表或换字体后重跑脚本即可**，不需要改脚本本身。

生成时会做这些校验（任一失败直接报错退出）：

- bootloader magic `0xE9`、分区表 magic `AA 50`、应用镜像 magic `0xE9`、字体 NPF1 magic `"NPF1"`；
- `nvs` / `phy_init` / `pldata` 三个区域全为 `0xFF`；
- 四段数据与源文件逐字节一致；
- 字体不超过 `font` 分区、应用不超过 `factory` 分区、各段不重叠。

同时生成 `<输出名>.sha256`。

## 四、烧录

```powershell
# esptool v5+（命令带连字符）
esptool --chip esp32s3 -p COMx write-flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin

# esptool v4.x（下划线写法）
esptool.exe --chip esp32s3 --port COMx write_flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin

# 没有独立 esptool.exe 时，用 IDF 自带的：
python -m esptool --chip esp32s3 -p COMx write-flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin
```

- 本机是 ESP32-S3 **内置 USB-Serial/JTAG**，esptool 会自动进下载模式，不用按键；
  失败时按住 B 键（GPIO0）再插线。
- 写入 16 MiB：全 `0xFF` 的区段会被 esptool 压缩传输，实际通常 **1~2.5 分钟**
  （按本机 font 分区 3.55 MiB / 33.4 s ≈ 893 kbit/s 粗算，16 MiB 上限约 2.5 分钟），
  结束时出现 `Hash of data verified.` 即成功。
- ⚠ `idfc flash` **不能**用它：idfc 是按分区逐段烧录的，不认"整片单文件烧 0x0"。

## 五、烧完第一次开机会发生什么

- **NVS 为空** → `config_store` 使用默认配置（音量/亮度/循环/随机/EQ/槽位等全部回到出厂值）；
- **pldata 为空** → 播放列表头校验失败 → 正常走"检测到列表无效 → 一键重新扫描"流程；
- **字体已在镜像里** → 直接可显示，**不需要**再执行 `FONT_FLASH.md` 的单独刷字体命令。

## 六、和常规 `idfc flash` 的对比

| | `idfc flash` | 全量固件烧 0x0 |
| --- | --- | --- |
| 写入内容 | bootloader + 分区表 + app | bootloader + 分区表 + app + **字体** |
| nvs（配置） | 保留 | **清空** |
| pldata（播放列表） | 保留 | **清空** |
| font 分区 | 不动（需另刷） | 已包含 |
| 适用场景 | 日常迭代、保留配置 | 首次安装、发版、配置/数据彻底重置 |

## 七、生成记录（2026-11-05，release）

### v1.0.0.0（当前产物）

- 构建：`build/cs43131.ttffont.release`（`idfc build -t release`）
- 镜像：`dist/FakePodNanoNightPlayer_v1.0.0.0_release_full_20261105.bin`，16,777,216 B
- SHA256：`66b1da94e96928d81b0d42f36cbe5fbf580f3a06150983070f667c6d9e17c1ba`
- 版本号：设置页「关于」= `1.0.0.0`（`CONFIG_FIRMWARE_VERSION_STRING`）；
  app descriptor version = `1.0.0.0`（`CMakeLists.txt` 的 `PROJECT_VER`，此前默认取
  `git describe`，会显示成 `0.9.0.0_alpha_5-25-g...`）
- 静态验证：四个 magic 全部正确；nvs / phy_init / pldata 全 `0xFF`；四段与源文件逐字节一致。

### 实机验证（2026-11-05，COM12，ESP32-S3 MAC 14:c1:9f:3a:a2:6c）

**v1.0.0.0 刷入 + 恢复旧备份**（版本号变更后重新生成的全量镜像）：

- `write-flash 0x0`：100.1 s、`Hash of data verified.`；
- 写回 nvs（0x9000）+ pldata（0xC00000）：两个 `Hash of data verified.`；
- 启动日志：`App version: 1.0.0.0`、`playlist loaded: dirs=173 songs=9042`、
  `loop=2, shuffle=1`、恢复上次曲目并自动播放（44.1 kHz / 320 kbps）、
  亮度恢复 139、`App started`。

更早（版本号变更前、同内容镜像）还验证过空数据启动与首次恢复：

1. 先 `read-flash` 备份 nvs（0x9000，24KB）与 pldata（0xC00000，4MB）；
2. 全量镜像 `write-flash 0x0`：99.6 s、`Hash of data verified.`；
3. 空数据启动：`boot_flow: playlist data invalid, prompt to scan` + `App started`
   → 整片擦写与清空 nvs/pldata 生效；
4. 写回 nvs/pldata（`Hash of data verified.`）后启动：
   `playlist loaded: dirs=173 songs=9042`、`loop=2, shuffle=1`、恢复上次曲目并自动播放
   （44.1 kHz / 320 kbps）、亮度恢复 139 → 列表、配置、音频链路全部正常。

备份文件与完整记录在 `dist/backup_20261105/`（`manifest.txt` + 日志，含
`flash_v1.0.0.0.log` / `restore_v1.0.0.0.log` / `boot_v1.0.0.0.log`）。

## 八、维护提示

- 脚本产物在 `dist/`（已被 `.gitignore` 忽略），不入库；发版时上传到 release 即可。
- 换字体：用 `tools/font` 重新烘焙 `spiffs/system.npf`（见 `NPF_FONT.md` §4）后重跑脚本，全量固件会自动带上新字体。
- 改分区表：确保 `pldata` 仍锚定在 flash 末尾（本脚本按 16 MiB 整片填充，若 pldata 结束
  位置小于 flash 大小，中间会自然填 `0xFF`，同样算"清空"）。
- 若将来 flash 不是 16 MiB，脚本会按 `flasher_args.json` 的 flash 大小自动调整。
