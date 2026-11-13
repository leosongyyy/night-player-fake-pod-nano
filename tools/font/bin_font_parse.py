#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 lv_font_conv 的 `--format bin` 产物（按 LVGL 9.4 lv_binfont_loader.c 的算法）。

用途：
  · 核对 bin 的表布局、每字形头部位宽（nbits）、位图是否字节对齐；
  · 统计官方 loader 会做多少次 lv_fs_read、会申请多少内存；
  · 为 NPF1 预烘焙（tools/font/bin_font_prebake.py）提供体积账。

用法：
  python tools/font/bin_font_parse.py tempdir/all24_bpp2.bin
  python tools/font/bin_font_parse.py --json tempdir/all24_bpp2.bin

依赖：无（纯标准库）。
"""
import argparse
import json
import struct
import sys

HEAD_FMT = "<IHHHhhHhhHhH" + "B" * 10 + "hH"
HEAD_FIELDS = [
    "version", "tables_count", "font_size", "ascent", "descent",
    "typo_ascent", "typo_descent", "typo_line_gap", "min_y", "max_y",
    "default_advance_width", "kerning_scale", "index_to_loc_format",
    "glyph_id_format", "advance_width_format", "bits_per_pixel",
    "xy_bits", "wh_bits", "advance_width_bits", "compression_id",
    "subpixels_mode", "padding", "underline_position", "underline_thickness",
]

CMAP_TYPE_NAMES = {
    0: "FORMAT0_FULL",
    1: "SPARSE_FULL",
    2: "FORMAT0_TINY",
    3: "SPARSE_TINY",
}


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class BitReader:
    """逐 bit 读取，并能统计"官方 loader 会做多少次 lv_fs_read"。

    lv_binfont_loader.c 的 read_bits() 每消费 8 bit 就调用一次 lv_fs_read(1 字节)。
    """

    def __init__(self, buf, pos):
        self.buf = buf
        self.pos = pos          # 字节位置
        self.bit = 0            # 当前字节内已消费的 bit 数（MSB first）
        self.reads = 0          # 模拟的 lv_fs_read 次数

    def read(self, n):
        val = 0
        for _ in range(n):
            if self.bit == 0:
                self.reads += 1
            val = (val << 1) | ((self.buf[self.pos] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return val

    def read_signed(self, n):
        v = self.read(n)
        return v - (1 << n) if v & (1 << (n - 1)) else v


def parse(bin_path):
    buf = open(bin_path, "rb").read()
    out = {"file": bin_path, "size": len(buf)}

    # ---- head ----
    head_len, head_label = u32(buf, 0), buf[4:8].decode("latin1")
    if head_label != "head":
        raise SystemExit(f"不是 lv_font_conv bin：开头不是 head（{head_label!r}）")
    if head_len < 8 + struct.calcsize(HEAD_FMT):
        raise SystemExit(f"head 表长异常：{head_len}")
    hv = dict(zip(HEAD_FIELDS, struct.unpack_from(HEAD_FMT, buf, 8)))
    out["header"] = hv

    nbits = hv["advance_width_bits"] + 2 * hv["xy_bits"] + 2 * hv["wh_bits"]
    out["nbits"] = nbits
    out["nbits_mod8"] = nbits % 8

    # ---- cmap ----
    cmaps_start = head_len
    cmaps_len = u32(buf, cmaps_start)
    if buf[cmaps_start + 4:cmaps_start + 8] != b"cmap":
        raise SystemExit("找不到 cmap 表")
    cmap_num = u32(buf, cmaps_start + 8)
    cmaps, list_bytes = [], 0
    for i in range(cmap_num):
        o = cmaps_start + 12 + i * 16
        data_off, rstart, rlen, gstart, entries, fmt, _pad = struct.unpack_from("<IIHHHBB", buf, o)
        if fmt in (1, 3):          # SPARSE_*：unicode_list（u16×entries）
            list_bytes += 2 * entries
        if fmt == 0:               # FORMAT0_FULL：glyph_id_ofs_list（u8×entries）
            list_bytes += entries
        elif fmt == 1:             # SPARSE_FULL：glyph_id_ofs_list（u16×entries）
            list_bytes += 2 * entries
        cmaps.append({"range_start": rstart, "range_length": rlen,
                      "glyph_id_start": gstart, "entries": entries,
                      "type": fmt, "type_name": CMAP_TYPE_NAMES.get(fmt, "?")})
    out["cmap_num"] = cmap_num
    out["cmap_list_bytes"] = list_bytes
    out["cmaps"] = cmaps

    # ---- loca ----
    loca_start = cmaps_start + cmaps_len
    loca_len = u32(buf, loca_start)
    if buf[loca_start + 4:loca_start + 8] != b"loca":
        raise SystemExit("找不到 loca 表")
    loca_count = u32(buf, loca_start + 8)
    if hv["index_to_loc_format"] == 0:
        offs = list(struct.unpack_from("<" + "H" * loca_count, buf, loca_start + 12))
    elif hv["index_to_loc_format"] == 1:
        offs = list(struct.unpack_from("<" + "I" * loca_count, buf, loca_start + 12))
    else:
        raise SystemExit(f"未知 index_to_loc_format={hv['index_to_loc_format']}")
    out["glyph_count"] = loca_count

    # ---- glyf ----
    glyf_start = loca_start + loca_len
    glyf_len = u32(buf, glyf_start)
    if buf[glyf_start + 4:glyf_start + 8] != b"glyf":
        raise SystemExit("找不到 glyf 表")

    reads = 0
    bitmap_bytes = 0
    empty_glyphs = 0
    dsc = []
    for i in range(loca_count):
        br = BitReader(buf, glyf_start + offs[i])
        if hv["advance_width_bits"] == 0:
            adv = hv["default_advance_width"]
        else:
            adv = br.read(hv["advance_width_bits"])
            if hv["advance_width_format"] == 0:
                adv *= 16
        ofs_x = br.read_signed(hv["xy_bits"])
        ofs_y = br.read_signed(hv["xy_bits"])
        box_w = br.read(hv["wh_bits"])
        box_h = br.read(hv["wh_bits"])
        if i == 0:
            adv = box_w = box_h = ofs_x = ofs_y = 0
        nxt = offs[i + 1] if i + 1 < loca_count else glyf_len
        bmp_size = nxt - offs[i] - nbits // 8
        if i > 0 and box_w * box_h:
            bitmap_bytes += bmp_size
        elif i > 0:
            empty_glyphs += 1
        dsc.append((adv, box_w, box_h, ofs_x, ofs_y, bmp_size))
        reads += br.reads

    # 官方 loader 第二遍：跳头部位 + 拷贝位图
    pass2 = 0
    for i in range(1, loca_count):
        bmp_size = dsc[i][5]
        if bmp_size <= 0:
            continue
        pass2 += (nbits + 7) // 8          # 跳头部（read_bits 每 8bit 一次读）
        if nbits % 8 == 0:
            pass2 += 1                     # 快路径：整段一次 lv_fs_read
        else:
            pass2 += bmp_size              # 慢路径：逐字节 read_bits
    total_reads = reads + pass2

    # ---- kern ----
    kern_len = 0
    kern_at = glyf_start + glyf_len
    if kern_at + 8 <= len(buf) and buf[kern_at + 4:kern_at + 8] == b"kern":
        kern_len = u32(buf, kern_at)
    out["kern_bytes"] = kern_len

    out["empty_glyphs"] = empty_glyphs
    out["bitmap_bytes"] = bitmap_bytes
    out["glyph_dsc_bytes"] = loca_count * 16     # LARGE=1 时 16B/字形
    out["loader_reads"] = {"pass1": reads, "pass2": pass2, "total": total_reads}
    out["npf1_size_estimate"] = (64 + 48 + loca_count * 16 + cmap_num * 20
                                 + list_bytes + kern_len + bitmap_bytes)
    return out


def main():
    ap = argparse.ArgumentParser(description="解析 lv_font_conv bin（按 LVGL 9.4 loader 算法）")
    ap.add_argument("bin", help="lv_font_conv --format bin 生成的文件")
    ap.add_argument("--json", action="store_true", help="只输出 JSON")
    args = ap.parse_args()

    r = parse(args.bin)
    if args.json:
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return

    hv = r["header"]
    print(f"文件            : {r['file']}  ({r['size']} B)")
    print(f"version/tables  : {hv['version']} / {hv['tables_count']}（<4 表示无 kern）")
    print(f"size/bpp        : {hv['font_size']}px / {hv['bits_per_pixel']}bpp  "
          f"compression_id={hv['compression_id']}（0=未压缩）")
    print(f"line_height     : ascent {hv['ascent']} - descent {hv['descent']} "
          f"= {hv['ascent'] - hv['descent']}")
    print(f"每字形头部位宽   : nbits={r['nbits']}（mod 8 = {r['nbits_mod8']}，"
          f"{'字节对齐' if r['nbits_mod8'] == 0 else '不对齐→官方 loader 逐字节慢路径'}）")
    print(f"字形数（loca）  : {r['glyph_count']}（含 gid0；空字形 {r['empty_glyphs']}）")
    print(f"cmap            : {r['cmap_num']} 个子表，列表数据 {r['cmap_list_bytes']} B")
    print(f"kern            : {r['kern_bytes']} B")
    print("\n子表（前 8 个）：")
    for c in r["cmaps"][:8]:
        print(f"  U+{c['range_start']:04X}+{c['range_length']:<6d} gid_start={c['glyph_id_start']:<6d} "
              f"entries={c['entries']:<6d} {c['type_name']}")
    if r["cmap_num"] > 8:
        print(f"  ...（共 {r['cmap_num']} 个）")

    print("\n体积：")
    print(f"  位图（loader 会 malloc）: {r['bitmap_bytes']} B")
    print(f"  glyph_dsc（16B×N）      : {r['glyph_dsc_bytes']} B")
    print(f"  NPF1 预估               : {r['npf1_size_estimate']} B "
          f"({r['npf1_size_estimate'] / 1048576:.2f} MiB)")
    print("\n官方 loader 读取次数：")
    print(f"  第一遍（读头）          : {r['loader_reads']['pass1']}")
    print(f"  第二遍（读位图）        : {r['loader_reads']['pass2']}")
    print(f"  合计 lv_fs_read         : {r['loader_reads']['total']}")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()
