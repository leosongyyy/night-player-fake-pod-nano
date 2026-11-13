#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 lv_font_conv 的 bin 预烘焙成 NPF1（NightPlayer Font v1）单字重镜像。

设计见 ideas_and_exps/NPF_FONT.md（原研究见 BINFONT_RESEARCH.md §7）：
  · 运行期结构全部离线算好：glyph_dsc 是 16B/字形（LV_FONT_FMT_TXT_LARGE 布局，LE），
    位图去交织并字节对齐，cmap 列表直接指向 blob；
  · 运行时只需 esp_partition_mmap + 填几个指针（本脚本产出的文件不依赖 LVGL 结构体定义，
    但字段布局必须与 LARGE=1 的 lv_font_fmt_txt_glyph_dsc_t 一致）；
  · kern 段默认丢弃（运行期不解析，中文界面无影响），需要时用 --keep-kern 保留。

用法：
  python tools/font/bin_font_prebake.py tempdir/all24_bpp2.bin spiffs/system.npf
  python tools/font/bin_font_prebake.py --no-verify in.bin out.npf

限制（原型范围）：
  · 单 face；多字号需要把多个 face 目录拼进同一文件（格式已预留 face_count）；
  · 只支持未压缩 bin（compression_id=0，生成时加 --no-compress）；
  · 校验是"重新解析产物并与源 bin 解码逐字节比对"，全字库会慢（分钟级）。
"""
import argparse
import struct
import sys
import zlib

HEAD_FMT = "<IHHHhhHhhHhH" + "B" * 10 + "hH"
HEAD_FIELDS = [
    "version", "tables_count", "font_size", "ascent", "descent",
    "typo_ascent", "typo_descent", "typo_line_gap", "min_y", "max_y",
    "default_advance_width", "kerning_scale", "index_to_loc_format",
    "glyph_id_format", "advance_width_format", "bits_per_pixel",
    "xy_bits", "wh_bits", "advance_width_bits", "compression_id",
    "subpixels_mode", "padding", "underline_position", "underline_thickness",
]

NPF_MAGIC = b"NPF1"
NPF_VERSION = 1
NPF_HDR_SIZE = 64
NPF_FACE_SIZE = 48
NPF_CMAP_SIZE = 20

# ---- 文件头（64B，全部小端；运行期 main/core/fonts/font_npf.c 按同样偏移读取）----
#   +0  "NPF1"
#   +4  version u16
#   +6  face_count u16
#   +8  flags u16
#   +10 max_face u16（最大字号 px，仅信息用）
#   +12 crc32 u32（覆盖 [64, total_size)）
#   +16 total_size u32（整个文件字节数，运行期据此决定 mmap 长度）
#   +20 保留 44B
# ---- face 记录（48B）----
#   +0 size_px u16 | +2 bpp u8 | +3 cmap_num u8 | +4 flags u16 | +6 kern_class u8 | +7 pad
#   +8 glyph_count u32 | +12 dsc_off | +16 cmap_off | +20 lists_off | +24 bitmap_off
#   +28 bitmap_size | +32 kern_off | +36 line_height i16 | +38 base_line i16
#   +40 subpx u8 | +41 underline_position i8 | +42 underline_thickness u8 | +43 pad
#   +44 kern_scale u16 | +46 pad 2B
FACE_FMT = "<HBBHBxIIIIIIIhhBbBxHxx"
# ---- cmap 记录（20B）----
#   +0 range_start u32 | +4 range_length u16 | +6 glyph_id_start u16
#   +8 unicode_list_off u32(-1=无) | +12 glyph_id_ofs_off u32(-1=无)
#   +16 list_length u16 | +18 type u8 | +19 pad
CMAP_FMT = "<IHHIIHBB"


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class BitReader:
    def __init__(self, buf, pos):
        self.buf, self.pos, self.bit = buf, pos, 0

    def read(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.buf[self.pos] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return v

    def read_signed(self, n):
        v = self.read(n)
        return v - (1 << n) if v & (1 << (n - 1)) else v


def parse_bin(buf):
    """解析官方 bin，返回 header、cmap、字形 dsc/位图与交叉映射。"""
    head_len = u32(buf, 0)
    if buf[4:8] != b"head":
        raise SystemExit("输入不是 lv_font_conv bin（缺 head）")
    hv = dict(zip(HEAD_FIELDS, struct.unpack_from(HEAD_FMT, buf, 8)))
    if hv["compression_id"] != 0:
        raise SystemExit("bin 是压缩的（compression_id=1）：请用 lv_font_conv --no-compress 重新生成")
    nbits = hv["advance_width_bits"] + 2 * hv["xy_bits"] + 2 * hv["wh_bits"]

    # cmap
    cmaps_start = head_len
    cmaps_len = u32(buf, cmaps_start)
    cmap_num = u32(buf, cmaps_start + 8)
    cmaps = []
    for i in range(cmap_num):
        data_off, rstart, rlen, gstart, entries, fmt, _ = \
            struct.unpack_from("<IIHHHBB", buf, cmaps_start + 12 + i * 16)
        p = cmaps_start + data_off
        uni = ofs = None
        if fmt in (1, 3):
            uni = list(struct.unpack_from("<" + "H" * entries, buf, p))
            p += 2 * entries
        if fmt == 0:
            ofs = list(buf[p:p + entries])
        elif fmt == 1:
            ofs = list(struct.unpack_from("<" + "H" * entries, buf, p))
        cmaps.append({"range_start": rstart, "range_length": rlen, "gid_start": gstart,
                      "type": fmt, "entries": entries, "unicode": uni, "ofs": ofs})

    # loca
    loca_start = cmaps_start + cmaps_len
    loca_len = u32(buf, loca_start)
    count = u32(buf, loca_start + 8)
    if hv["index_to_loc_format"] == 0:
        offs = list(struct.unpack_from("<" + "H" * count, buf, loca_start + 12))
    else:
        offs = list(struct.unpack_from("<" + "I" * count, buf, loca_start + 12))
    glyf_start = loca_start + loca_len
    glyf_len = u32(buf, glyf_start)

    # glyf：头 → dsc；位图去交织（与 loader 第二遍逐字节一致）
    glyphs = []
    blob = bytearray()
    for i in range(count):
        br = BitReader(buf, glyf_start + offs[i])
        adv = hv["default_advance_width"] if hv["advance_width_bits"] == 0 else \
            br.read(hv["advance_width_bits"])
        if hv["advance_width_format"] == 0:
            adv *= 16
        ofs_x = br.read_signed(hv["xy_bits"])
        ofs_y = br.read_signed(hv["xy_bits"])
        box_w = br.read(hv["wh_bits"])
        box_h = br.read(hv["wh_bits"])
        if i == 0:
            adv = box_w = box_h = ofs_x = ofs_y = 0
        nxt = offs[i + 1] if i + 1 < count else glyf_len
        bmp_size = nxt - offs[i] - nbits // 8
        bmp = b""
        if i > 0 and box_w * box_h:
            if nbits % 8 == 0:
                q = glyf_start + offs[i] + nbits // 8
                bmp = buf[q:q + bmp_size]
            else:
                tmp = bytearray()
                for _ in range(max(0, bmp_size - 1)):
                    tmp.append(br.read(8))
                if bmp_size > 0:
                    tmp.append(br.read(8 - nbits % 8) << (nbits % 8))
                bmp = bytes(tmp)
        glyphs.append({"bitmap_index": len(blob), "adv_w": adv, "box_w": box_w, "box_h": box_h,
                       "ofs_x": ofs_x, "ofs_y": ofs_y, "bmp": bmp})
        blob += bmp

    # kern（整段搬运）
    kern_at = glyf_start + glyf_len
    kern = b""
    kern_class = 0
    if hv["tables_count"] >= 4 and kern_at + 8 <= len(buf) and buf[kern_at + 4:kern_at + 8] == b"kern":
        klen = u32(buf, kern_at)
        kern = buf[kern_at:kern_at + klen]
        kern_class = kern[8] if len(kern) > 8 else 0

    return hv, nbits, count, cmaps, glyphs, blob, kern, kern_class


def main():
    ap = argparse.ArgumentParser(description="lv_font_conv bin → NPF1 预烘焙镜像")
    ap.add_argument("bin", help="输入 bin（--no-compress）")
    ap.add_argument("out", help="输出 .npf")
    ap.add_argument("--no-verify", action="store_true", help="跳过与源 bin 的逐字形比对")
    ap.add_argument("--keep-kern", action="store_true",
                    help="保留 bin 的 kern 段（默认丢弃：运行期不解析 kern，中文界面无影响，可省 3.8KB）")
    args = ap.parse_args()

    buf = open(args.bin, "rb").read()
    hv, nbits, count, cmaps, glyphs, blob, kern, kern_class = parse_bin(buf)
    if not args.keep_kern:
        kern, kern_class = b"", 0

    dsc_off = NPF_HDR_SIZE + NPF_FACE_SIZE
    cmap_off = dsc_off + count * 16
    lists_off = cmap_off + len(cmaps) * NPF_CMAP_SIZE

    lists = bytearray()
    recs = bytearray()
    for c in cmaps:
        u_off = o_off = 0xFFFFFFFF
        if c["unicode"] is not None:
            u_off = lists_off + len(lists)
            lists += struct.pack("<" + "H" * len(c["unicode"]), *c["unicode"])
            if len(lists) % 2:
                lists += b"\x00"
        if c["ofs"] is not None:
            o_off = lists_off + len(lists)
            if c["type"] == 0:
                lists += bytes(c["ofs"])
            else:
                lists += struct.pack("<" + "H" * len(c["ofs"]), *c["ofs"])
            if len(lists) % 2:
                lists += b"\x00"
        recs += struct.pack(CMAP_FMT, c["range_start"], c["range_length"], c["gid_start"],
                            u_off, o_off, c["entries"], c["type"], 0)

    kern_off = lists_off + len(lists)
    pre_kern_pad = b""
    if kern:
        pad = (4 - kern_off % 4) % 4
        pre_kern_pad = b"\x00" * pad
        kern_off += pad
    bitmap_off = kern_off + len(kern)
    post_pad = b""
    pad = (4 - bitmap_off % 4) % 4
    if pad:
        post_pad = b"\x00" * pad
        bitmap_off += pad

    dscs = bytearray()
    for g in glyphs:
        dscs += struct.pack("<IIHHhh", g["bitmap_index"], g["adv_w"], g["box_w"],
                            g["box_h"], g["ofs_x"], g["ofs_y"])

    face = struct.pack(FACE_FMT,
                       hv["font_size"], hv["bits_per_pixel"], len(cmaps), 0,
                       kern_class,
                       count, dsc_off, cmap_off, lists_off, bitmap_off, len(blob),
                       kern_off if kern else 0,
                       hv["ascent"] - hv["descent"], -hv["descent"],
                       hv["subpixels_mode"],
                       hv["underline_position"], hv["underline_thickness"],
                       hv["kerning_scale"])
    assert len(face) == NPF_FACE_SIZE, len(face)

    header = struct.pack("<4sHHHHI", NPF_MAGIC, NPF_VERSION, 1, 0, hv["font_size"], 0)
    header = header.ljust(NPF_HDR_SIZE, b"\x00")

    body = (face + bytes(dscs) + bytes(recs) + bytes(lists) + pre_kern_pad
            + bytes(kern) + post_pad + bytes(blob))
    data = bytearray(header + body)
    crc = zlib.crc32(data[NPF_HDR_SIZE:]) & 0xFFFFFFFF
    struct.pack_into("<I", data, 12, crc)
    struct.pack_into("<I", data, 16, len(data))   # total_size，供运行期 mmap 用

    open(args.out, "wb").write(data)

    bitmap_bytes = len(blob)
    print(f"输入            : {args.bin} ({len(buf)} B)")
    print(f"字形数          : {count}（含 gid0）")
    print(f"cmap 子表       : {len(cmaps)}")
    print(f"kern            : {len(kern)} B{' (class=' + str(kern_class) + ')' if kern else ''}")
    print(f"输出            : {args.out} ({len(data)} B)")
    print(f"  header/face   : {NPF_HDR_SIZE}+{NPF_FACE_SIZE}")
    print(f"  glyph_dsc     : {count * 16} B @0x{dsc_off:X}")
    print(f"  cmap 记录     : {len(cmaps) * NPF_CMAP_SIZE} B @0x{cmap_off:X}")
    print(f"  cmap 列表     : {len(lists)} B @0x{lists_off:X}")
    print(f"  kern          : {len(kern)} B @0x{kern_off:X}")
    print(f"  bitmap        : {bitmap_bytes} B @0x{bitmap_off:X}")

    if args.no_verify:
        return

    # ---- 校验：重新解析产物，与源 bin 解码结果逐字节比对 ----
    npf = data
    assert npf[:4] == NPF_MAGIC
    assert u32(npf, 16) == len(npf), "total_size 与文件长度不一致"
    assert zlib.crc32(npf[NPF_HDR_SIZE:]) & 0xFFFFFFFF == u32(npf, 12), "CRC 不匹配"
    face_vals = struct.unpack_from(FACE_FMT, npf, NPF_HDR_SIZE)
    (size_px, bpp, cmap_n, _flags, kclass, gcount, f_dsc, f_cmap, f_lists, f_bmp, bmp_size,
     f_kern, line_h, base_l, subpx, ul_pos, ul_thk, kscale) = face_vals
    assert gcount == count and bmp_size == bitmap_bytes
    assert line_h == hv["ascent"] - hv["descent"] and base_l == -hv["descent"]
    assert f_lists + len(lists) <= f_bmp, "cmap 列表与位图区重叠"
    for i, g in enumerate(glyphs):
        bi, adv, bw, bh, ox, oy = struct.unpack_from("<IIHHhh", npf, f_dsc + i * 16)
        assert (adv, bw, bh, ox, oy) == (g["adv_w"], g["box_w"], g["box_h"], g["ofs_x"], g["ofs_y"]), i
        n = (bw * bh * bpp + 7) // 8
        assert npf[f_bmp + bi: f_bmp + bi + n] == g["bmp"][:n], i

    # cmap 记录：偏移必须落在 lists 区内且类型/长度与源 bin 一致
    for i, c in enumerate(cmaps):
        rec = struct.unpack_from(CMAP_FMT, npf, f_cmap + i * NPF_CMAP_SIZE)
        assert rec[0] == c["range_start"] and rec[1] == c["range_length"] and rec[2] == c["gid_start"]
        for off in (rec[3], rec[4]):
            assert off == 0xFFFFFFFF or f_lists <= off < f_bmp
    print(f"校验通过        : {count} 个字形的 dsc/位图、{len(cmaps)} 个 cmap 记录均与源 bin 一致")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()
