# tools/font —— NPF1 全字库的生成 / 校验工具

配套文档：`ideas_and_exps/NPF_FONT.md`（LVGL 9.4 可行性、NPF1 格式、刷写与实测数据）；
调研背景：`ideas_and_exps/BINFONT_RESEARCH.md`（官方 bin loader 为什么不能直接用、体积账）。

这些脚本**不属于固件构建链**，`idfc build` 时不会运行。产物 `spiffs/system.npf` 入库，
固件运行期由 `main/core/fonts/font_npf.c` mmap 直读。

| 脚本 | 用途 | 依赖 |
|---|---|---|
| `font_meta.py` | 从 TTF 提取元数据（numGlyphs、cmap 覆盖、Unicode Block 分布、hhea/OS2 度量），并生成 `lv_font_conv -r` 用的区间清单 | `pip install fonttools` |
| `bin_font_parse.py` | 解析 lv_font_conv 的 bin：表布局、每字形位宽（nbits）、位图是否字节对齐、体积/内存账 | 无（标准库） |
| `bin_font_prebake.py` | 把 bin 预烘焙成 **NPF1** 镜像（16B/字形 dsc + 字节对齐位图 + cmap 偏移表；kern 默认丢弃），并逐字形与源 bin 比对 | 无（标准库） |
| `npf_verify.py` | 按 LVGL 8.4 `get_glyph_dsc_id()` 的逻辑在 Python 里复算 NPF1 的 cmap 查找，并可与 TTF cmap 交叉核对覆盖（推荐每次生成后都跑） | `fonttools`（只在使用 `--ttf` 时需要） |

## 依赖安装（本机一次性）

```powershell
# lv_font_conv（Node，用来把 TTF 栅格化成 bin）
# 沙箱下默认 npm 缓存在 %APPDATA% 不可写，必须把缓存重定向到工作区
npm install --prefix tempdir\fontconv --cache tempdir\npm-cache lv_font_conv

# fontTools（Python，font_meta.py / npf_verify.py 用）
python -m pip install --target tempdir\pylib --cache-dir tempdir\pip-cache fonttools
$env:PYTHONPATH = "tempdir\pylib"     # 或直接 pip install --user fonttools
```

## 生成 `spiffs/system.npf`（当前 24px 全字库）

```powershell
# 0) 取源字体（仓库里只存 NPF 镜像，不再存 TTF；从删除 TTF 之前的提交里取，
#    例：git log --diff-filter=D --oneline -- spiffs/system.ttf 找到删除提交的父提交）
git show <ttf_commit>:spiffs/system.ttf > tempdir\system.ttf

# 1) cmap → 区间清单
python tools/font/font_meta.py tempdir\system.ttf --outdir tempdir

# 2) 栅格化（必须 --no-compress：本项目没开 LV_USE_FONT_COMPRESSED）
$ranges = Get-Content tempdir\font_ranges.txt
$argv = @('--font','tempdir\system.ttf','--size','24','--bpp','2','--no-compress',
          '--format','bin','-o','tempdir\all24_bpp2.bin')
foreach ($r in $ranges) { $argv += '--range'; $argv += $r }
& tempdir\fontconv\node_modules\.bin\lv_font_conv.cmd @argv

# 3) 预烘焙（约 4 秒，含逐字形校验）
python tools/font/bin_font_prebake.py tempdir\all24_bpp2.bin spiffs\system.npf

# 4) 复算 + 与 TTF cmap 交叉核对
python tools/font/npf_verify.py spiffs\system.npf --ttf tempdir\system.ttf
```

当前产物：**4,653,106 B（4.44 MiB）**，34,462 个字形、183 个 cmap、行高 32、基线 7。

## NPF1 实现范围

- **单 face**：格式头预留 `face_count` 与 48B face 记录（当前只放 24px；20px 小字体仍编译进固件）；
- 只接受未压缩 bin（`compression_id=0`）；
- kern 默认丢弃（运行期不解析，`--keep-kern` 仅把原始段搬进镜像，运行时仍忽略）；
- 校验方式：重新解析产物，把每个字形的 `adv_w/box/ofs` 与位图字节与源 bin 解码结果逐字节比对，
  并检查每条 cmap 记录的偏移落在列表区内；
- 格式与 LVGL 9.4 结构体的对应关系、`_Static_assert` 约束见 `NPF_FONT.md` §2；改格式必须同时改
  `bin_font_prebake.py` 与 `main/core/fonts/font_npf.c`（并升 `NPF_VERSION`）。
- `npf_verify.py` 复算用的是 LVGL 8.4 的 `get_glyph_dsc_id()`（`rcp > range_length`）；9.4 把它
  改成了 `>=`。这个 off-by-one 只影响"每个区间末尾之后那一个本来就不存在的码位"，对真实
  码位没有差别（同一份镜像在两条分支上通用）。

## 安全提示

- `tempdir/` 是中间产物的家（已 gitignore）；`spiffs/system.npf` **入库**，`spiffs/` 下还有
  `DroidSansFallback-LICENSE.txt`（字体许可，必须随发布包一起给）；
- 生成后先确认镜像 < 6 MiB（超了就调大 `font` 分区并等量缩小 `ota_0`，**`nvs`/`phy_init` 不要动**），
  再按 `ideas_and_exps/FONT_FLASH.md` 刷到 `0x600000`。
