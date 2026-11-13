# BINFONT_RESEARCH —— LVGL 9.4 的 bin 字体加载剖析，与"整字库放 font 分区 + mmap + 零解析"方案
> ⚠ **（2026-11-14 更新）本文写于"升级后"的 `master` 环境（LVGL 9.4 / IDF 6.1）。此时
> §2～§6 的官方 bin loader 分析仍然适用；而 §7 提出的 NPF1 预烘焙方案**已经在两条分支落地**：
> `master`（LVGL 9.4，与本文环境一致）与 `legacy_s3`（LVGL 8.4）**共用同一份
> `spiffs/system.npf`**，运行时分别是 `main/core/fonts/font_npf.c`（9.4 侧多一个
> `dsc.stride = 0`）。最终格式、生成与刷写步骤见 [NPF_FONT.md](NPF_FONT.md)，本文只作调研记录。
> §9 的分区方案以 NPF_FONT.md 为准（最终定稿是 6MB @0x600000，不是本文讨论的 8~9MB）。**


> 调研时间：2026-11-11　适用分支：`master`（当时的优化方向；方案现已落地）
> 结论先行：**官方 bin 加载器慢在"逐字节 lv_fs 读 + 两遍扫描 + 全量复制/分配"，与字体放哪无关**；
> 要让 bin 字体达到"和 .c 一样快、比 TTF 省 PSRAM"，正确做法是**构建期把字体预烘焙成运行期结构**，
> 运行时只做一次 `esp_partition_mmap` + 填几个指针。全字库（34,461 个码位）在 24px+20px 两套字号下
> 约需 **8.11MB** 字体分区——当时 4MiB 装不下；实际落地只保留 24px 一套（4.44MB、font 分区 6MB），
> 20px 小字体继续用内嵌位图。

---

## 1. 目标与边界

- 用 **bin/位图字体**（构建期已栅格化），彻底跳过运行时 FreeType 光栅化；
- 字体像现在的 TTF 一样**单独放 `font` 分区**，运行时 `esp_partition_mmap` 读取，不进 RAM；
- 尽可能**降低 PSRAM 占用**（现在 TTF 的字形位图缓存实测填满约 **0.61MB**，见 `FontSystem.md` §8.2）；
- 允许自定义格式与自定义生成器（Python / Node 均可）。

不属于本次范围：换字库选型、改 UI 字号体系、bpp4/A8 画质升级（§9 会算它的体积代价）。

---

## 2. LVGL 9.4 官方 bin 加载路径（`lv_binfont_loader.c`）

### 2.1 入口

| API | 位置 | 说明 |
|---|---|---|
| `lv_binfont_create(path)` | `lv_binfont_loader.c:96` | 走 `lv_fs` 打开文件，分配 `lv_font_t`，调 `lvgl_load_font()` |
| `lv_binfont_create_from_buffer(buf,size)` | `:124` | **仅在 `LV_USE_FS_MEMFS` 打开时存在**；把 buffer 包成 MEMFS 路径，仍然走同一个解析器 |
| `lv_binfont_destroy(font)` | `:133` | 释放所有 `lv_malloc` 出来的结构 |

本项目当前 `CONFIG_LV_USE_FS_MEMFS` 未开，所以连 `_from_buffer` 都没有；即使打开，它也只是换了个"文件后端"，解析逻辑一字不变。

### 2.2 文件结构（实测 lv_font_conv 1.5.3）

每个表的"长度"字段**包含 8 字节标签头**（`read_label()` 在 `:217`：4B 长度 + 4B magic）：

```
[head]  ｜ 48 = 8 + font_header_bin_t(40B，含 2B 尾部 padding)
[cmap]  ｜ 8 + 4(subtable 数) + 16×N(表目录) + 各子表数据
[loca]  ｜ 8 + 4(字形数) + 2B 或 4B × N（index_to_loc_format 0/1）
[glyf]  ｜ 8 + Σ(每字形：位压缩头部 + 位图)
[kern]  ｜ 可选；tables_count < 4 表示没有（实测 3 = cmap/loca/glyf）
```

`font_header_bin_t` 关键字段（实测值，24px 全字库）：
`font_size=24, ascent=20, descent=-6, kerning_scale=16, index_to_loc_format=0,
advance_width_format=1, bits_per_pixel=2, xy_bits=6, wh_bits=6, advance_width_bits=10,
compression_id=0（`--no-compress`）`。

**每字形的头部位宽 `nbits = advance_width_bits + 2*xy_bits + 2*wh_bits`**，实测：

| 字号/子集 | nbits | mod 8 | 位图是否字节对齐 |
|---|---|---|---|
| 24px（ASCII 109 字形） | 30 | 6 | 否 |
| 24px 全字库 | **34** | **2** | 否 |
| 20px 全字库 | **32** | **0** | 是 |

> 位宽/对齐是**按字号和字库自动算出来的，不能假设**。这正是官方 loader 在 24px 下几乎全走"逐字节慢路径"的原因。

### 2.3 加载时做了哪些工作（`lvgl_load_font()`，`:476`）

1. `lv_malloc_zeroed(sizeof(lv_font_t))`（本项目 32 位目标约 36B）；
2. `lv_malloc(sizeof(lv_font_fmt_txt_dsc_t))`（约 20B）；
3. 读 head → 填 `line_height / base_line / subpx / underline_*`、`dsc.bpp / kern_scale / bitmap_format`；
4. cmap：分配 `lv_font_fmt_txt_cmap_t[cmap_num]`（24B/个）+ 临时 `cmap_table_bin_t[]`（16B/个）；
   逐个 seek 到子表数据，分配并读入 `unicode_list`（u16）和/或 `glyph_id_ofs_list`（u8/u16）；
5. loca：分配 `uint32_t[count+1]`，按格式读 u16/u32 偏移（临时数组，load 完即释放）；
6. **glyf 第一遍**：逐字形 seek + 位读头部（adv/ofs/box），算 `bmp_size`、累计 `bitmap_index`；
7. 分配整块 `glyph_bitmap`（= 全部字形位图之和），**第二遍**逐字形再 seek、跳过头部位、拷贝位图；
   若 `nbits % 8 != 0`，则**逐字节** `read_bits(8)` 并在最后一个字节左移对齐；
8. kern（若存在）：分配并读入 pair 或 class 表；
9. 挂上 `get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt`、`get_glyph_bitmap = lv_font_get_bitmap_fmt_txt`。

### 2.4 慢在哪：实测读写次数

`read_bits()`（`:186`）**每 8 个 bit 就调一次 `lv_fs_read(fp, &byte, 1)`**。用它数出来的读取次数：

| 字库 | 字形数 | 文件 | 估算 `lv_fs_read` 次数 |
|---|---|---|---|
| 24px ASCII+10 汉字 | 107 | 7,136 B | ~6,342 |
| 24px CJK 0x4E00–0x5FFF | 4,609 | 599,436 B | ~599,067 |
| 24px CJK 0x4E00–0x9FFF | 20,903 | 2,756,304 B | ~2,754,911 |
| **24px 全字库** | **34,462** | **4,380,532 B** | **~4,441,599** |
| 20px 全字库 | 34,462 | 3,175,204 B | ~310,083（恰好字节对齐，走快路径） |

对照历史记录：完整字库经 SPIFFS 运行时加载实测 **8 秒**（`main/core/fonts/system_font.h:23`）；
即使换成 font 分区 + mmap，读不再触发"关缓存/停核"，**4.4M 次函数调用 + 两遍扫描 + 4.1MB 位图复制**
仍然在几百 ms~秒级，并且：

- 峰值内存 = `glyph_dsc`（16B×N，全字库 **551KB**）+ `glyph_bitmap`（**4.10MB**）+ 临时 loca（138KB）+ cmap；
  本项目 `LV_USE_CUSTOM_MALLOC` 把这些全部分配到 **PSRAM**；
- 之后每帧绘制还要把 bpp 位图展开成 A8（`lv_font_get_bitmap_fmt_txt`，无字形缓存）。

### 2.5 结论：官方 loader 只适合"小字体/一次性"

CJK 全字库用官方 loader，等于每次开机把 4MB 位图**解析 + 复制**一遍，既慢又吃 PSRAM，
完全违背"放分区里 mmap"的初衷。`lv_binfont_create_from_buffer` 不改变这一点。

---

## 3. 运行期真正需要什么

| 结构 | 大小（本项目，`CONFIG_LV_FONT_FMT_TXT_LARGE=y`） | 是否必须可写 |
|---|---|---|
| `lv_font_t` | 36B | 是（指针对齐/字段） |
| `lv_font_fmt_txt_dsc_t` | 20B | 是（含 3 个指针） |
| `lv_font_fmt_txt_glyph_dsc_t[]` | **16B/字形**（u32 bitmap_index, u32 adv_w, u16 box_w, u16 box_h, i16 ofs_x, i16 ofs_y） | 否，只读 |
| `lv_font_fmt_txt_cmap_t[]` | 24B/子表（含 2 个指针） | 是（指针），但只有几百字节 |
| cmap 的 `unicode_list` / `glyph_id_ofs_list` | 只读 | 否 |
| 位图 blob | 字体主体 | 否 |
| kern（可选） | 全字库仅 3,856B | 否 |

关键事实：

- **`glyph_dsc_t` 在 LARGE 模式下没有位域、没有 padding，恰好 16 字节**，可以离线按 LE 直接写进 blob；
  运行时把 `dsc.glyph_dsc` 指向 mmap 地址即可（建议在 C 里加 `_Static_assert(sizeof(...)==16)` 防 ABI 漂移）。
- `dsc` 与 `cmap` 数组含指针，不能直接放 flash；但它们只有几百字节，放内部 RAM 的 `.bss` 即可。
- **fmt_txt 没有字形位图缓存**：`lv_draw_sw_letter.c:156` 的"static bitmap 直读"快路径**只对 A8 生效**，
  bpp2 字体每帧都要走 `bpp → A8` 转换（`lv_font_get_bitmap_fmt_txt`）。这是 bin 方案唯一的运行时新开销（§8）。
- 官方 bin 的位图是"位压缩头部 + 位图"交错存放，且多数情况下位图起始**不在字节边界**（§2.2），
  所以无法把官方 bin 直接零拷贝喂给官方回调——必须先"脱交织 + 字节对齐"，也就是**构建期预烘焙**。

---

## 4. 全字库元数据（从 `spiffs/system.ttf` 直接提取）

用 `fontTools 4.68.0` 读 `cmap`（best cmap = 3/10 format 12）：

| 项目 | 数值 |
|---|---|
| 字形总数 `numGlyphs` | **50,951** |
| 可编码码位（cmap） | **34,461**（BMP 34,455 + SMP 6），U+0000 … U+1D30C |
| 连续区间 | 269 段（合并小间隔后 149 段，供 `lv_font_conv --range`） |
| `unitsPerEm` / hhea | 256 / ascent 267、descent −68、lineGap 0 |
| OS/2 | typo 196/−61/+16，win 267/68，weight 400 |

码位分布（≥20 的 Block）：

| Block | 码位 | Block | 码位 |
|---|---|---|---|
| CJK Unified Ideographs | 20,902 | CJK Compatibility | 105 |
| Hangul Syllables | 11,172 | Hangul Jamo | 99 |
| CJK Compatibility Ideographs | 302 | Basic Latin | 96 |
| Dingbats | 174 | IPA Extensions | 96 |
| Halfwidth/Fullwidth Forms | 164 | Hangul Compatibility Jamo | 94 |
| Box Drawing | 113 | Hiragana / Katakana | 90 / 90 |
| Arrows | 112 | Enclosed CJK Letters and Months | 77 |
| Enclosed Alphanumerics | 112 | Cyrillic / Math Operators | 66 / 59 |

> 注意 **Hangul Syllables 占 11,172 个码位**（韩文音节），是"全字库"体积的大头之一；
> 如果只服务中文/日文界面，可以砍掉它换分区余量（§7 给出两套账）。

---

## 5. 实测体积（lv_font_conv 1.5.3，Droid Sans Fallback，bpp2，`--no-compress`）

| 字库 | 字形数 | bin 文件 | 位图字节（loader 会分配） | glyph_dsc（16B×N） | 预烘焙 NPF1（不含 cmap 列表） ||---|---|---|---|---|---|
| 24px ASCII+10 汉字 | 107 | 7,136 | 5,489 | 1,712 | ~7.2KB |
| 24px CJK 0x4E00–0x5FFF | 4,609 | 599,436 | 571,416 | 73,744 | 645,160（原型实测） |
| 24px CJK 0x4E00–0x9FFF | 20,903 | 2,756,304 | 2,629,496 | 334,448 | **2,965,632**（原型实测） |
| **24px 全字库** | 34,462 | 4,380,532 | 4,096,970 | 551,392 | **≈4.66MB**（4.44MiB） |
| **20px 全字库** | 34,462 | 3,175,204 | 2,891,656 | 551,392 | **≈3.45MB**（3.29MiB） |

- 两套全字库合计 **≈8.11MB（7.74MiB）**；再加 cmap 列表、对齐、kern（各 3,856B）约 +10KB。
- 20px 的位图比 24px 小 ~29%，与面积比 (20/24)²≈0.69 基本吻合。
- 全字库 24px 的 14 个空字形（无位图）在预烘焙时会被跳过。
- 参考：**bpp1** 全字库 24px bin 仅 2,158,252B（≈2.3MB NPF1），但 1bit 只有黑白两档，24px 汉字会明显发毛；
  **bpp4** 位图翻倍到 ~8.2MB（单 24px），不可行。

### 5.1 官方 loader 装载全字库的代价（同一份字体）

- 24px：`lv_fs_read` **≈444 万次**；`glyph_bitmap` 4.10MB + `glyph_dsc` 551KB 常驻 PSRAM；
- 之后每次绘制仍有 bpp→A8 展开；
  即：**bin 官方 loader 既没有 TTF 的按需光栅化，也没有 .c 的零解析**，是三者里最慢的。

---

## 6. 三条可行路线对比

| 路线 | 启动 | 常驻 PSRAM | flash | 运行时 | 结论 |
|---|---|---|---|---|---|
| **TTF + FreeType（现状）** | 快（只建 2 个字号） | 字形缓存实测 **~0.61MB** + FT 堆 | 3.55MB 字体 + FT 代码 | 首次绘制某字要光栅化（可能卡顿），之后命中缓存 | 画质最好，占 PSRAM |
| **内嵌 .c（老 binfont）** | 最快（零解析） | `.rodata` 被 XIP 拷进 PSRAM ≈ 字库大小 | **app +3.84MB**（实测 binfont app 3.84MB） | 每帧 bpp→A8 | 吃 flash/PSRAM，编译慢 |
| **官方 bin + 官方 loader** | **最慢**（4.4M 次读） | +4.1MB 位图 + 0.55MB dsc | 字体 4.4MB | 每帧转换 | ❌ 不可取 |
| **官方 bin + 内存版单遍解析** | 慢（仍要复制/分配） | 同上 | 同 | 同 | 仅比上面快常数倍 |
| **NPF1 预烘焙 + mmap（推荐）** | **≈0**（mmap + 填指针） | **≈2~8KB**（dsc/cmaps/face 目录） | 8.11MB（两套全字库） | 每帧 bpp→A8（可加 LRU） | ✅ 目标形态 |

补充：也可以在运行时写一个"自定义 backend"直接原地读官方 bin（loca 当指针、按需解位压缩头），
启动确实 ≈0，但每次取字形都要做位提取、并自己实现 cmap 查找与 bpp→A8；**画质/体积与 NPF1 相同，
代码更多、每帧更慢**，因此不如预烘焙（NPF1 仍用官方回调，只是数据换成字节对齐的）。

---

## 7. NPF1 v1 格式设计（NightPlayer Font）

目标：**元数据与位图都能被官方回调直接使用，运行时零解析、零拷贝**；多字号共用一个 blob。

```
+0x00  Header (64B, LE)
        magic "NPF1" u32 | version u16 | face_count u16 | flags u16 | max_face u16
        crc32 u32（可选，覆盖从 face 目录到文件尾；初始化时校验可选）
        reserved 40B
+0x40  Face directory（每 face 48B）
        size_px u16 | bpp u8 | cmap_num u8 | flags u16 | glyph_count u32
        dsc_off u32 | cmap_off u32 | lists_off u32 | bitmap_off u32 | bitmap_size u32
        kern_off u32 | kern_class u8 | pad 3B | line_height i16 | base_line i16
        subpx u8 | underline_position i8 | underline_thickness u8 | pad 1B | kern_scale u16
+...   glyph_dsc 数组（每 face 16B/字形，与 LARGE 布局逐字节一致，LE）
+...   cmap 记录（每子表 20B：range_start u32, range_length u16, glyph_id_start u16,
        unicode_list_off u32(-1), ofs_list_off u32(-1), list_length u16, type u8, pad u8）
+...   lists（u16 unicode 列表 / u8 或 u16 偏移表；**直接指向 blob，运行时不复制**）
+...   kern 数据（可直接沿用官方 bin 的 kern 段，3.8KB；无 kern 时 off=0）
+...   位图 blob（按 gid 顺序、每个字形字节对齐、stride=0、**无压缩**）
```

运行时初始化（只需一次）：

```c
const esp_partition_t *p = esp_partition_find_first(DATA, 0x41, "font");
esp_partition_mmap(p, 0, npf_round_up(blob_len), ESP_PARTITION_MMAP_DATA, &base, &h);
/* 校验 magic/version/（可选 crc） */
s_font_dsc.glyph_bitmap = base + face->bitmap_off;
s_font_dsc.glyph_dsc    = (void *)(base + face->dsc_off);   /* 只读，指向 mmap */
s_font_dsc.cmaps        = s_cmaps;                          /* .bss，24B×cmap_num */
for (i...) { 把 cmap 记录里的 *_off 换成 base+off 指针；拷贝 range/type/list_length }
s_font = (lv_font_t){ .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
                      .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
                      .line_height = ..., .base_line = ..., .dsc = &s_font_dsc };
```

要点：

1. **`glyph_dsc` / 位图 / cmap 列表 / kern 都在 mmap 区**，运行时只写 3 个指针 + 拷 `cmap[]`（几百字节）；
2. `lv_font_t`、`dsc`、`cmaps[]` 放内部 RAM 的静态区（约 36 + 20 + 24×183 ≈ **4.5KB**）；
3. 必须 `bpp=2`、`bitmap_format=PLAIN(0)`、`stride=0`；本项目 `CONFIG_LV_USE_FONT_COMPRESSED=n`，
   生成器必须 `--no-compress`（官方默认会压缩：同一份 ASCII 子集 7,136B → 4,560B，`compression_id=1`）；
4. 在 C 里加 `_Static_assert`：`sizeof(lv_font_fmt_txt_glyph_dsc_t)==16`、
   `offsetof(...bitmap_index)==0` 等，防止 LVGL/编译选项变化导致 blob 失配；
5. `cmap_num` 受 `:9` 位域限制（≤511），实测全字库 183，安全；
6. `bitmap_index` 为 u32（LARGE=1），4MB+ 位图没问题（若关掉 LARGE 则只有 20bit=1MB，**不能用**）。

### 6.1 生成器（建议 Python，基于官方 bin 后处理）

- 输入：TTF + face 配置（字号、bpp、码位集合）+ 输出路径；
- 步骤：
  1. 对每个 face 调 `lv_font_conv --format bin --no-compress`（保证栅格化结果与 LVGL 一致）；
  2. 解析 bin（head/cmap/loca/glyf/kern，算法同 §2.3，但用内存指针、单遍）；
  3. 按 NPF1 布局重排：字形头部的位压缩字段解码成 16B 固定 dsc；位图**去交织 + 字节对齐**；
     cmap 偏移改 blob 内偏移；kern 原样搬运；
  4. 输出 `spiffs/system.npf`（烧到 font 分区），可选输出一份 `npf_report.txt`（体积/码位/校验和）；
- 校验：magic/version/长度/偏移合法；逐字形位图与官方 loader 解码结果**逐字节一致**
  （原型已在 20,903 字形的 CJK 全量上验证通过）；
- **参考实现已入库**：`tools/font/bin_font_prebake.py`（单 face 原型，含逐字形校验）、
  `tools/font/bin_font_parse.py`（解析/体积账）、`tools/font/font_meta.py`（TTF 元数据与 range 清单），
  用法见 `tools/font/README.md`；
- 构建集成：`tools/font/bin_font_prebake.py`（多 face 拼接待扩展）+ 一条 `idfc` 之外的手工/脚本命令；字体不进固件，
  刷机流程与现在 TTF 完全一致（esptool 写 font 分区），只是文件与偏移变了。

> 生成器只做"格式转换"，不做栅格化——栅格化交给 lv_font_conv，避免自研 hinting/AA 与 LVGL 不一致。

---

## 8. PSRAM / 性能账

| 项目 | TTF（现状） | NPF1（全字库 24+20） |
|---|---|---|
| 字体占 flash | 3.55MB（TTF） | 8.11MB（两套 bpp2 全字库） |
| 常驻 PSRAM | 字形位图缓存 ~0.61MB（实测填满）+ FT 堆/缓存结构 | **≈5KB**（dsc + cmaps + font） |
| 启动字体成本 | 建 2 个字号 + 首次绘制光栅化 | mmap + 填指针（≈0） |
| 每帧文字 | 命中缓存后主要是查表+blend | 查表 + **bpp2→A8 展开** + blend |
| 画质 | 8bit 灰度 AA | 2bit（4 档灰度，与旧 binfont 一致） |
| FreeType 代码 | 编入（占用 flash） | **不编入**（可去掉 `espressif/freetype` 依赖） |

- **PSRAM 净收益约 0.6MB**（不算 FT 内部堆，实际略多）；这是本次方案最直接的收益。
- **新增 CPU**：`lv_font_get_bitmap_fmt_txt` 的 bpp2 分支是逐像素 4 分支 switch；
  24px 汉字约 22×22=484 次/字，一屏 20 个字约 1 万次/帧——相对 QSPI 刷屏 21ms 可忽略；
  若以后要抠，可加 64~256 条的 A8 LRU（PSRAM 几十 KB，收益是省重复展开）。
- 启动不再有"新字形光栅化"抖动，首屏/列表滚动更可预测。

---

## 9. 分区方案（重点：pldata 不动，配置/播放列表不丢）

现状（`partitions.csv`）：`factory 0x10000 size 0x7F0000` / `font 0x800000 4M` / `pldata 0xC00000 4M`。
`pldata` 锚定在 0xC00000（**任何方案都不要动它**，否则旧播放列表作废）。

| 方案 | factory | font | 能装下 | 余量 |
|---|---|---|---|---|
| 现状 | 7.94MB | 4MiB @0x800000 | **连 24px 全字库都不够**（NPF1 4.66MB） | — |
| A（24px 全 + 20px 子集） | 5.94MB | 6MiB @0x600000 | 4.66 + ~0.05 = 4.71MB | ~1.29MB |
| **B（两套全字库，推荐）** | **3.94MB** | **8MiB @0x400000** | **8.11MB** | **~0.28MB** |
| C（两套全字库，留余量） | 2.94MB | 9MiB @0x300000 | 8.11MB | ~1.28MB |

- 应用固件现在 debug 1.86MB / release 2.09MB；方案 B 的 factory 3.94MB 仍有 ~1.85MB 余量，
  方案 C 只剩 ~0.85MB（S3 后续不再加功能的话够用，但更紧）。
- **20px 是否要全字库**：UI 里 20px 只用于状态栏（音量档位/电池电压等 ASCII），
  全字库 20px 只为"用户数据也用 20px 显示"这种未来场景；若不需要，选 A，分区压力小很多。
- 迁移影响：
  - 只改 `factory` 与 `font` 的边界，`nvs` / `pldata` 偏移与大小不变 → **用户配置、播放列表都不丢**；
  - `font` 起始偏移变了（0x800000 → 0x400000/0x300000）→ 字体必须重刷（本来就要换成 .npf）；
  - 分区表要重刷（0x8000），app 镜像位置 0x10000 不变，可顺带重刷 app；
  - `main/core/fonts/*` 按**分区标签**查找 font 分区，代码不需要硬编码偏移；需要改的是文档、
    `FONT_FLASH.md`、`make_full_firmware.py`（它从 `partitions.csv` 读偏移，但字体文件名要换成 `.npf`）。
- mmap 页数：8~9MB 字体按 64KB/页 = 128~144 页（另有 pldata 64 页、PSRAM 128 页）；
  ESP-IDF 在页数不足时会返回错误，实现时以实测为准（必要时只 mmap 实际 blob 长度而非整个分区）。

---

## 10. 风险与待验证

1. **画质**：bpp2 只有 4 档灰度，比 FreeType 的 8bit AA 粗糙；旧 binfont 就是 bpp2，属已知可接受。
   要 bpp4 就得 24px 单套 ~8.2MB，与 20px 无法共存。
2. **每帧转换**：无字形缓存；如果实测某页面掉帧，再考虑 LRU（不要一上来就加）。
3. **格式耦合**：`glyph_dsc` 布局依赖 `LV_FONT_FMT_TXT_LARGE=y` 与 32 位 ABI；用 `_Static_assert` 锁死，
   生成器头部写 version，运行时不匹配就回退内嵌 lite 字体（与 TTF 失败时的行为一致）。
4. **缓存关闭窗口**：pldata 重写会关外部缓存，此时 mmap 的字体不可访问——但渲染与写 flash 不并发
   （现架构已由 `deep_worker`/暂停渲染保证），与现在 TTF mmap 的约束相同（`FontSystem.md` §6.3）。
5. **全量生成时间**：34k 字形 lv_font_conv 一次约 1~3 分钟（实测 24px 全字库几分钟内完成），
   属构建期成本，不影响固件。
6. **生成器与运行时版本绑定**：NPF1 version 写进文件头；改格式必须同时改生成器与固件。
7. **全字库的 149 段 range 是"合并 ≤3 空位"后的结果**；生成器应直接按 cmap 码位输出，
   避免把字库里不存在的码位也栅格化（会多出字形/体积）。

---

## 11. 建议实施顺序（研究结论落地时）

1. 先按方案 A（6MiB font，24px 全 + 20px ASCII 子集）跑通 NPF1：生成器 + 运行时替换 `font_ttf.c`，
   构建、烧录、真机验证画质/PSRAM/帧率；
2. 若确认 20px 也要全量，再切方案 B/C（8~9MiB），只改 `partitions.csv` 两行 + 重刷分区表与字体；
3. 用 `idfc size` 对比切换前后 app 体积（去掉 FreeType 后应明显变小），用 `mem_monitor` 对比 PSRAM；
4. 保留内嵌 lite 位图字体作为 fallback（NPF 缺失/校验失败时用），与现在 TTF 的回退逻辑一致；
5. 换字体流程固定为：`tools/font/bin_font_prebake.py` → `esptool write_flash <font_off> spiffs/system.npf`。

---

## 12. 附录：复现方法与原始数据

工具（本机实测）：

```powershell
# npm 缓存必须重定向到工作区（默认 %APPDATA%\npm-cache 在沙箱下不可写）
npm install --prefix tempdir\fontconv --cache tempdir\npm-cache lv_font_conv   # 1.5.3
& tempdir\fontconv\node_modules\.bin\lv_font_conv.cmd `
    --font spiffs\system.ttf --size 24 --bpp 2 --no-compress --format bin `
    -o tempdir\all24_bpp2.bin --range 0x4E00-0x9FFF

# 解析 / 体积账 / 预烘焙（工具已入库，说明见 tools/font/README.md）
python tools\font\bin_font_parse.py tempdir\all24_bpp2.bin
python tools\font\bin_font_prebake.py tempdir\all24_bpp2.bin tempdir\system.npf
```

元数据（fontTools 4.68.0，由 `tools/font/font_meta.py` 提取）：`numGlyphs=50951`、cmap=34,461；
`lv_font_conv` 无 `--no-compress` 时默认压缩（`compression_id=1`），本项目的
`CONFIG_LV_USE_FONT_COMPRESSED` 为 n，**必须显式 `--no-compress`**。

原型验证（真实跑通）：把 24px CJK 0x4E00–0x9FFF 的 bin 按 NPF1 思路重排（`tools/font/bin_font_prebake.py`），
生成 2,965,632B 的预烘焙镜像，逐字形与官方 loader 的解码结果比对——**20,903 个字形位图与
adv/box/ofs 全部一致**，cmap 查找抽查通过。全字库两套的 NPF1 体积是按同一模型由
bin 的 `bitmap + 16B×glyph + cmap + kern` 推算，尚未做整包生成/校验（实施第 1 步补上）。
