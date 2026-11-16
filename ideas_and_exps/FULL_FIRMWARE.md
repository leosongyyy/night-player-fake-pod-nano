# 全量固件（单文件、烧 0x0 一次完成整片更新）

> **一句话**：把 `bootloader.bin` + `partition-table.bin` + **播放器固件** +
> **（里世界固件）** + **空白 otadata** + `spiffs/system.npf` 按分区表偏移拼成一个
> **16 MiB** 的 bin，整片烧到 **0x0** 即可完成更新，并**顺带清空 nvs**（用户配置不保留）。
> 字体（NPF1 位图全字库）已包含，不需要再单独刷 `font` 分区。
>
> ⚠ **2026-11-16 第六次分区重排：`pldata` 分区已删除**。播放列表缓存存在 SD 卡文件
> `/sdcard/.ecov/pldata.bin`（见 `PLDATA_ON_SDCARD.md`），整片烧 0x0 **不会影响播放列表**；
> 要强制重扫曲库就用设置菜单「立即重新扫描」（或删掉卡上那个文件）。腾出的 4MiB
> 给了里世界固件（`ota_0` 现在在 `0xC00000`）。下文属于**历史记录**的 pldata 段落保留
> 原样，只是当时的事实。

产物示例：`dist/FakePodNanoNightPlayer_release_full.bin`（16,777,216 B）。

---

## 一、镜像构成

| 偏移 | 大小 | 内容 | 来源 |
| --- | --- | --- | --- |
| `0x002000` | 15,376 B | bootloader | `build/<role>.<variant>/bootloader/bootloader.bin` |
| `0x008000` | 3,072 B | 分区表 | `build/<role>.<variant>/partition_table/partition-table.bin` |
| `0x009000` | 24 KiB | `nvs` —— **0xFF（清空）** | 填充 |
| `0x00F000` | 4 KiB | `phy_init` —— **0xFF（清空）** | 填充 |
| `0x010000` | ≈2.8 MiB | **表世界 / 播放器固件**（`factory`，5.93 MiB 槽） | `build/player.<variant>/` |
| `0x5FE000` | 8 KiB | `otadata` —— **0xFF（空白；启动选择实际走 RTC 字节，这 8KB 只是占位）** | 填充 |
| `0x600000` | 4,653,106 B | 系统字体 `system.npf`（NPF1 位图全字库，裸镜像） | `spiffs/system.npf` |
| `0xC00000` | ≈1.7 MiB | **里世界固件**（`ota_0`，WiFi对时 + USB传歌，4 MiB 槽） | `build/inner.<variant>/`（可 `--no-sync` 跳过） |
| 其余空隙 | — | 0xFF（擦除态） | 填充 |

镜像总长 = flash 大小 = **16 MiB**，最后一段 `ota_0` 正好顶到 `0x1000000`
（2026-11-16 第六次重排：删掉了 `pldata`，把 16MB 尾部的 4MiB 全给里世界固件；
播放列表已改存 SD 卡 `/sdcard/.ecov/pldata.bin`，见 `PLDATA_ON_SDCARD.md`）。

⚠ **bootloader 在 0x2000 而不是 0x0**（2026-11 迁到 ESP32-S31 后 `CONFIG_BOOTLOADER_OFFSET_IN_FLASH=0x2000`），
所以 `0x0 ~ 0x2000` 是 0xFF；脚本的偏移全部取自 `flasher_args.json`，不写死。

⚠ 2026-11 起是**双固件**方案（见 `INNER_WORLD.md`）：表世界（播放器）在 `factory`、
里世界固件（WiFi对时 + USB传歌）在 `ota_0`。矩阵是 `variant`（debug/release）×
`role`（player/inner），全量固件默认把**两份都**带上（`build/inner.<variant>/<项目名>.bin`
存在时，优先与表世界同 variant、回退另一个）；没有里世界固件的镜像也能正常装/用，
只是点「WiFi对时」/「USB传歌」会提示"里世界固件未烧录"。

## 二、为什么这样就能"清数据"

ESP32 擦除后的 flash 读出来就是 `0xFF`。合并时整片先填 `0xFF`，只把四段真实数据覆盖
进去，于是：

- `nvs`（0x9000）在镜像范围内但内容全是 `0xFF`；
- `esptool write_flash 0x0 <file>` 会擦除并写入 `0x0` ~ `0xFFFFFF` 整片，
  旧 nvs 的字节被 `0xFF` 覆盖（**播放列表在 SD 卡上，不受整片重刷影响**）。

所以烧完后的状态与"先 `erase_flash` 再分区烧录"等价，但只需一个文件、一条命令。
`phy_init` 也一并清空，首次启动会重新做射频校准（本固件不用 WiFi/BT，无影响）。

## 三、生成

脚本：`tools/make_full_firmware.py`（纯标准库，不依赖 esptool）。

```powershell
idfc build -t release                                  # 先构建表世界（release）
idfc build -t release -o role=inner                    # 再构建里世界（想带上 ota_0 才需要）
python tools/make_full_firmware.py -t release          # 生成 dist/<项目名>_release_full.bin
```

常用参数：

```powershell
python tools/make_full_firmware.py -t release -o dist\full_20261105.bin   # 指定输出名
python tools/make_full_firmware.py -t debug                               # 另一种构建类型（debug）
python tools/make_full_firmware.py -t debug --build-dir build\player.debug   # 指定构建目录
python tools/make_full_firmware.py -t release --no-sync                   # 不带里世界固件（ota_0 留空）
python tools/make_full_firmware.py -t release --sync-image build\inner.release\FakePodNanoNightPlayer.bin
python tools/make_full_firmware.py -t release --no-font                   # 不带字体
```

> ⚠ 2026-11 起构建矩阵只剩 `variant` 一个维度（`--dac-chip` 参数已随 `dac_chip` 维度一起移除），
> 构建目录就是 `build/<variant>`。`--font <路径>` **仍然可用**（默认 `spiffs/system.npf`），
> 另有 `--no-font` 跳过合并字体。
> **里世界固件**默认自动找 `build/inner.<variant>/<项目名>.bin`（优先与表世界同 variant，回退另一个）；找不到时只提示、不失败
> （想消除提示用 `--no-sync`，想指定别的路径用 `--sync-image`）。

脚本不写死偏移：bootloader/分区表/播放器/otadata 的偏移来自构建目录的 `flasher_args.json`，
`ota_0` / `font` / `factory` 的位置和大小来自仓库根目录的 `partitions.csv`，
flash 大小取自 `flasher_args.json` 的 `flash_settings.flash_size`。
**改分区表或换字体后重跑脚本即可**，不需要改脚本本身。

生成时会做这些校验（任一失败直接报错退出）：

- bootloader magic `0xE9`、分区表 magic `AA 50`、播放器镜像 magic `0xE9`（位置都按 `flasher_args.json`）；
- 里世界固件 magic `0xE9`（合并了才查），没合并时 `ota_0` 必须整段 `0xFF`；
- `otadata` 必须整段 `0xFF`（自定义 bootloader 只看 RTC 字节，空白 otadata 不影响启动）；
- 字体 NPF1 magic `"NPF1"`（合并了才查）；
- `nvs` / `phy_init` 两个区域全为 `0xFF`；
- 各段数据与源文件逐字节一致；
- 字体不超过 `font` 分区、播放器不超过 `factory`（5.93MB）、里世界固件不超过 `ota_0`（4MB）、各段不重叠。

同时生成 `<输出名>.sha256`。

## 四、烧录

```powershell
# esptool v5+（命令带连字符）
esptool --chip esp32s31 -p COMx write-flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin

# esptool v4.x（下划线写法）
esptool.exe --chip esp32s31 --port COMx write_flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin

# 没有独立 esptool.exe 时，用 IDF 自带的：
python -m esptool --chip esp32s31 -p COMx write-flash 0x0 dist\FakePodNanoNightPlayer_release_full_20261105.bin
```

- 本机是 ESP32-S31 的 **USB-Serial/JTAG**，esptool 会自动进下载模式，不用按键；
  失败时按住板上的 BOOT/电源键再上电（S31 没有 S3 时代那个「B 键 = GPIO0」的按键）。
- 写入 16 MiB：全 `0xFF` 的区段会被 esptool 压缩传输，实际通常 **1~2.5 分钟**
  （按本机字体 4.44 MiB / 28.9 s ≈ 1288 kbit/s 粗算，16 MiB 上限约 2.5 分钟），
  结束时出现 `Hash of data verified.` 即成功。
- ⚠ `idfc flash` **不能**用它：idfc 是按分区逐段烧录的，不认"整片单文件烧 0x0"。

## 五、烧完第一次开机会发生什么

- **NVS 为空** → `config_store` 使用默认配置（音量/亮度/循环/随机/EQ/槽位等全部回到出厂值）；
- **播放列表在 SD 卡上、不受影响** → 卡上有 `pldata.bin` 就直接命中缓存进主界面；
  没有则自动扫描一遍并生成（见 `PLDATA_ON_SDCARD.md`）；
- **字体已在镜像里** → 直接可显示，**不需要**再执行 `FONT_FLASH.md` 的单独刷字体命令。

## 六、和常规 `idfc flash` 的对比

| | `idfc flash` | 全量固件烧 0x0 |
| --- | --- | --- |
| 写入内容 | bootloader + 分区表 + **播放器** + 空白 otadata | bootloader + 分区表 + **播放器 + 里世界固件** + 空白 otadata + **字体** |
| 里世界固件（ota_0） | 不动（要另外 `esptool write-flash 0xC00000`） | 已包含（除非 `--no-sync`） |
| nvs（配置） | 保留 | **清空** |
| 播放列表（SD 卡） | 保留 | 保留（在卡上，整片刷不影响） |
| font 分区 | 不动（需另刷） | 已包含 |
| 适用场景 | 日常迭代、保留配置 | 首次安装、发版、配置彻底重置 |

> 两条路都会把 `otadata` 写成空白，也就是**烧完一定先启动 `factory` 里的播放器**——
> 不会出现"刚烧完却跑进里世界固件"的情况。

## 七、生成记录（2026-11-05，release；📅 S3 时代 1.x 的历史记录，当前 master 是 2.0.0.0-alpha-1）

### v1.0.0.0（📅 历史产物；当前版本号见 `CMakeLists.txt` 的 `PROJECT_VER`）

- 构建：`build/cs43131.ttffont.release`（当时的 cell 目录；现在只有 `build/<variant>`）
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
- 改分区表：现在的布局是 `factory`(5.93MB) → `otadata`(8KB) → `font`(6MB) → `ota_0`(4MB)，
  最后一段顶到 16MiB 末尾；`nvs`/`phy_init`/`font` 偏移不要动（见 `partitions.csv` 头部）。
- 若将来 flash 不是 16 MiB，脚本会按 `flasher_args.json` 的 flash 大小自动调整。
