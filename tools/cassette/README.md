# 袖珍磁带机 UI 素材工具链

本目录保存两个**离线**生成工具，产物（C 数组）入库，构建期不运行，与 `tools/font/` 的策略一致。

## 1. `make_tape_image.py` — 磁带图 PNG → `lv_image_dsc_t`

源素材：`/Volumes/mac_mini/龙虾/NightPlayer-S31-二次开发指引/assets/cassette/tape-original.png`（2048×2048 RGB PNG）。

```bash
python3 -m pip install pillow
python3 tools/cassette/make_tape_image.py --lq
```

产物：

| 文件 | 内容 | 体积 |
|---|---|---|
| `main/core/image/cassette_tape.h` | 两个描述符的 extern 声明 + `CAS_TAPE_IMG` 选择宏 | ~0.6 KB |
| `main/core/image/cassette_tape.c` | 320×320 RGB565 `cas_tape_320`，小端字节序 | 204,800 B（200 KiB） |
| `main/core/image/cassette_tape_lq.c` | 320×320 I2 索引版 `cas_tape_320_lq`（4 色调色板） | 25,616 B（25 KiB） |

脚本会打印体积核对（RGB565 必须等于 `320*320*2`）。默认使用 RGB565 版；`CONFIG_CASSETTE_TAPE_LQ=y` 时 CMake 改编 LQ 版，二者只有一个进固件。

两者都是 `const` 数组，链接到 Flash 走 XIP，**不占 PSRAM、不占内部 DMA 池**（对照 `core/image/default_cover.c` 的双版本模式）。

字节序说明：ESP32 为小端，`lv_color_hex()` 与 LVGL RGB565 快速路径都按"低字节在前"读取，故脚本逐个像素写 `lo, hi`。

## 2. `bake_font.sh` — 复古中文子集字体

字体源已下载到 `assets/fonts/`（站酷庆科黄油体 + JetBrains Mono，均 SIL OFL 1.1，
许可证同目录 `*-OFL.txt`）。脚本保留"缺源即跳过、编译不阻塞"策略：

```bash
npm i -g lv_font_conv        # 或 PATH 指向任意已安装位置
bash tools/cassette/bake_font.sh
# 自定义源字体：
ZCOOL_TTF=/path/to/... MONO_TTF=/path/to/... bash tools/cassette/bake_font.sh
```

- 默认源：**站酷庆科黄油体**（ZCOOL QingKe HuangYou，中文正文）+ **JetBrains Mono Regular**（真等宽数字，digit advance 恒 0.6em）。
- 字符表：`glyphs_cn.txt`（从 7 模块字符串字面量自动抽取，429 字符，含 ASCII/全角标点）与 `glyphs_mono.txt`（ASCII 可打印 + 度/温度符号，109 字符）。
- 参数：`--size 24 --bpp 2 --format lvgl`，与 NPF1 正文同高；等宽子集用 `--lv-fallback cas_font_cn`，中文/全角标点自动回退中文字库，不会出方块。
- 成功时生成 `main/core/fonts/cassette_font_cn_baked.c`、`cassette_font_mono_baked.c` 与 `cassette_font_baked.h`（符号 `cas_font_cn` / `cas_font_mono`）。两个字库必须分文件：lv_font_conv 的 `glyph_bitmap` 等是 file-static 符号，合并会重名。CMake 按 `EXISTS` 自动登记；`CONFIG_CASSETTE_FONT_FALLBACK` 默认已设为 `n`，直接构建即启用。
- 字体源或 `lv_font_conv` 缺失时脚本打印 `SKIP` 并 `exit 0`，保持 FALLBACK 路径（`UI_SYSTEM_FONT` + `&lv_font_montserrat_24`），**不会让编译卡住**。

`lv_font_conv` 版本注意：1.5.3 用 `--lv-font-name`（旧版是 `--name`）；脚本已按 1.5.3 校正。

## 3. 授权

ZCOOL QingKe HuangYou 与 JetBrains Mono 均为 SIL Open Font License 1.1；子集 C 数组随固件分发时必须保留字体名称与 OFL 授权声明（已写入 `cassette_font_baked.c` 头部，完整文本见 `assets/fonts/*-OFL.txt`）。
