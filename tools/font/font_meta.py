#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提取 TTF 的字体元数据与全部可编码字形（cmap 覆盖），并生成 lv_font_conv 用的区间清单。

输出：
  <outdir>/font_meta.txt      人类可读报告
  <outdir>/font_ranges.txt    每行一个 "0xSTART-0xEND"，可直接喂给
                              `lv_font_conv -r <range>`（可重复）

用法：
  python tools/font/font_meta.py spiffs/system.ttf --outdir tempdir

依赖：fontTools（pip install fonttools）
"""
import argparse
import sys
from collections import Counter


def main():
    ap = argparse.ArgumentParser(description="提取 TTF 元数据 / cmap 覆盖 / 生成 range 清单")
    ap.add_argument("ttf", help="字体文件（如 spiffs/system.ttf）")
    ap.add_argument("--outdir", default="tempdir", help="输出目录（默认 tempdir）")
    ap.add_argument("--no-files", action="store_true", help="只打印，不写文件")
    args = ap.parse_args()

    try:
        from fontTools.ttLib import TTFont
        from fontTools import unicodedata as ftuni
    except ImportError:
        raise SystemExit("需要 fontTools：pip install fonttools")

    f = TTFont(args.ttf, fontNumber=0, lazy=True)
    glyph_order = f.getGlyphOrder()
    cmap_raw = f["cmap"]
    best = cmap_raw.getBestCmap()
    cps = sorted(best.keys())

    tables = [(st.platformID, st.platEncID, st.format, len(st.cmap)) for st in cmap_raw.tables]
    bmp = [c for c in cps if c <= 0xFFFF]
    smp = [c for c in cps if c > 0xFFFF]

    print(f"字体文件              : {args.ttf}")
    print(f"numGlyphs（字形总数） : {len(glyph_order)}")
    print(f"cmap 子表             : {tables}")
    print(f"可编码码位            : {len(cps)}（BMP {len(bmp)} / SMP {len(smp)}）")
    print(f"码位范围              : U+{cps[0]:04X} .. U+{cps[-1]:04X}")

    blocks = Counter()
    for c in cps:
        try:
            blocks[ftuni.block(chr(c))] += 1
        except Exception:
            blocks["<unknown>"] += 1
    print("\n按 Unicode Block 的码位数（>=20 才列出）：")
    for name, n in blocks.most_common():
        if n >= 20:
            print(f"  {name:45s} {n}")

    # ---- 连续区间（多段合并：间隔 <= 3 的相邻段合并，减少命令行参数）----
    runs = []
    start = prev = cps[0]
    for c in cps[1:]:
        if c == prev + 1:
            prev = c
            continue
        runs.append((start, prev))
        start = prev = c
    runs.append((start, prev))

    merged = []
    for a, b in runs:
        if merged and a - merged[-1][1] <= 3:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))

    print(f"\n连续区间              : {len(runs)} 段（合并小间隔后 {len(merged)} 段）")

    # ---- 字体度量 ----
    head, hhea, os2 = f["head"], f["hhea"], f["OS/2"]
    print(f"\nunitsPerEm={head.unitsPerEm}  hhea ascent/descent/lineGap="
          f"{hhea.ascent}/{hhea.descent}/{hhea.lineGap}")
    print(f"OS/2 typo={os2.sTypoAscender}/{os2.sTypoDescender}/{os2.sTypoLineGap}  "
          f"win={os2.usWinAscent}/{os2.usWinDescent}  weight={os2.usWeightClass}")

    if args.no_files:
        return

    import os
    os.makedirs(args.outdir, exist_ok=True)
    with open(os.path.join(args.outdir, "font_ranges.txt"), "w", encoding="ascii") as fp:
        for a, b in merged:
            fp.write(f"0x{a:X}-0x{b:X}\n")
    with open(os.path.join(args.outdir, "font_meta.txt"), "w", encoding="utf-8") as fp:
        fp.write(f"font={args.ttf}\nnumGlyphs={len(glyph_order)}\ncodepoints={len(cps)}\n")
        fp.write(f"bmp={len(bmp)} smp={len(smp)}\n")
        fp.write(f"runs={len(runs)} merged={len(merged)}\nblocks:\n")
        for name, n in blocks.most_common():
            fp.write(f"  {name}\t{n}\n")
    print(f"\n已写出：{args.outdir}/font_meta.txt、{args.outdir}/font_ranges.txt")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()
