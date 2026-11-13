#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 NPF1 字体镜像：按 LVGL 8.4 `lv_font_fmt_txt.c` 的查找逻辑在 Python 里复算一遍。

做的事（全部脱离 LVGL 与硬件）：
  1. 解析 NPF1 头 / face 记录，做偏移合法性检查；
  2. 复刻 `get_glyph_dsc_id()` 的四种 cmap 查找（FORMAT0_TINY/FULL、SPARSE_TINY/FULL）；
  3. 给出 TTF 时：TTF cmap 的每个码位都必须能查到 gid，且该 gid 的 box 非空
     （只统计位图应为空的控制符），并打印缺字清单；
  4. 全量遍历时顺便统计每个字形的位图是否越界。

用法：
  python tools/font/npf_verify.py spiffs/system.npf --ttf tempdir/system.ttf
"""
import argparse
import struct
import sys

NPF_HDR_SIZE = 64
NPF_FACE_SIZE = 48
NPF_CMAP_SIZE = 20
FACE_FMT = "<HBBHBxIIIIIIIhhBbBxHxx"
CMAP_FMT = "<IHHIIHBB"
NOFF = 0xFFFFFFFF


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i16(b, o):
    return struct.unpack_from("<h", b, o)[0]


class Npf:
    def __init__(self, data):
        if data[:4] != b"NPF1":
            raise SystemExit("缺 NPF1 magic")
        self.data = data
        self.version = u16(data, 4)
        self.faces = u16(data, 6)
        self.max_face = u16(data, 10)
        self.total = u32(data, 16)
        if self.total != len(data):
            raise SystemExit(f"total_size={self.total} 与文件长度 {len(data)} 不一致")
        f = NPF_HDR_SIZE
        (self.size_px, self.bpp, self.cmap_num, self.flags, self.kern_class,
         self.glyph_count, self.dsc_off, self.cmap_off, self.lists_off,
         self.bitmap_off, self.bitmap_size, self.kern_off,
         self.line_height, self.base_line, self.subpx, self.ul_pos, self.ul_thk,
         self.kern_scale) = struct.unpack_from(FACE_FMT, data, f)
        assert self.dsc_off + self.glyph_count * 16 <= len(data)
        assert self.cmap_off + self.cmap_num * NPF_CMAP_SIZE <= len(data)
        assert self.bitmap_off + self.bitmap_size <= len(data)
        self.cmaps = []
        for i in range(self.cmap_num):
            (rs, rl, gs, uo, oo, ll, typ, _pad) = struct.unpack_from(
                CMAP_FMT, data, self.cmap_off + i * NPF_CMAP_SIZE)
            uni = None if uo == NOFF else list(
                struct.unpack_from("<" + "H" * ll, data, uo))
            ofs = None
            if oo != NOFF:
                if typ == 0:
                    ofs = list(data[oo:oo + ll])
                else:
                    ofs = list(struct.unpack_from("<" + "H" * ll, data, oo))
            self.cmaps.append({"start": rs, "len": rl, "gid": gs, "type": typ,
                               "uni": uni, "ofs": ofs, "ll": ll})

    def glyph(self, gid):
        o = self.dsc_off + gid * 16
        return struct.unpack_from("<IIHHhh", self.data, o)

    def gid_of(self, letter):
        """复刻 LVGL 8.4 get_glyph_dsc_id()（含 `rcp > range_length` 的既有写法）。"""
        if letter == 0:
            return 0
        for c in self.cmaps:
            rcp = (letter - c["start"]) & 0xFFFFFFFF
            if rcp > c["len"]:
                continue
            if c["type"] == 2:      # FORMAT0_TINY
                return c["gid"] + rcp
            if c["type"] == 0:      # FORMAT0_FULL
                return c["gid"] + c["ofs"][rcp]
            # sparse：在 unicode_list 里二分 rcp
            key = rcp & 0xFFFF
            lo, hi = 0, c["ll"]
            while lo < hi:
                mid = (lo + hi) // 2
                if c["uni"][mid] < key:
                    lo = mid + 1
                else:
                    hi = mid
            if lo < c["ll"] and c["uni"][lo] == key:
                if c["type"] == 3:  # SPARSE_TINY
                    return c["gid"] + lo
                return c["gid"] + c["ofs"][lo]  # SPARSE_FULL
            return 0
        return 0

    def bitmap_len(self, gid):
        _bi, _adv, bw, bh, _ox, _oy = self.glyph(gid)
        return (bw * bh * self.bpp + 7) // 8


def main():
    ap = argparse.ArgumentParser(description="校验 NPF1 字体镜像")
    ap.add_argument("npf")
    ap.add_argument("--ttf", help="可选：TTF 源文件，交叉核对 cmap 覆盖")
    ap.add_argument("--sample", type=int, default=0, help="只抽查 N 个码位（0=全量）")
    args = ap.parse_args()

    npf = Npf(open(args.npf, "rb").read())
    print(f"NPF1 v{npf.version}  {npf.total} B")
    print(f"  字号 {npf.size_px}px  bpp {npf.bpp}  行高 {npf.line_height}  基线 {npf.base_line}")
    print(f"  字形 {npf.glyph_count}  cmap {npf.cmap_num}  "
          f"位图 {npf.bitmap_size} B  下划线 {npf.ul_pos}/{npf.ul_thk}  kern_scale {npf.kern_scale}")

    # 全量检查位图偏移不越界
    bad = 0
    max_end = 0
    for gid in range(1, npf.glyph_count):
        bi = npf.glyph(gid)[0]
        n = npf.bitmap_len(gid)
        max_end = max(max_end, bi + n)
        if bi + n > npf.bitmap_size:
            bad += 1
    print(f"  位图最大末端 {max_end} / {npf.bitmap_size}（越界字形 {bad}）")
    if bad:
        raise SystemExit("位图越界")

    if not args.ttf:
        return

    from fontTools.ttLib import TTFont
    f = TTFont(args.ttf, fontNumber=0, lazy=True)
    cps = sorted(f["cmap"].getBestCmap().keys())
    if args.sample:
        cps = cps[:: max(1, len(cps) // args.sample)]
    miss, empty = [], []
    for cp in cps:
        gid = npf.gid_of(cp)
        if not gid:
            miss.append(cp)
            continue
        if cp > 0x1F and npf.bitmap_len(gid) == 0:
            empty.append(cp)
    print(f"  TTF cmap {len(cps)} 个码位 → 查不到 gid {len(miss)}，查到但位图为空 {len(empty)}")
    if miss[:10]:
        print("    缺:", " ".join(f"U+{c:04X}" for c in miss[:10]))
    if empty[:10]:
        print("    空:", " ".join(f"U+{c:04X}" for c in empty[:10]))
    if miss or empty:
        raise SystemExit(1)
    print("  交叉校验通过")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()
