#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成"全量固件"（单文件、从 0x0 一次烧录完成整片更新）。

把下面四段按分区表偏移拼进一个 16 MiB 的镜像，其余区域一律填 0xFF：

    0x000000  bootloader.bin
    0x008000  partition-table.bin
    0x010000  <项目名>.bin        （应用固件）
    0x800000  spiffs/system.ttf  （font 分区，裸 TTF）
    0xC00000~0x1000000            pldata，保持 0xFF（= 清空播放列表）

因为 nvs(0x9000)、phy_init(0xF000)、pldata(0xC00000) 都在镜像范围内且为 0xFF，
把它烧到 0x0 就等于"整片擦除后重刷"：配置(nvs)和播放列表(pldata)都会被清掉。

前提：先构建出目标配置（脚本只做合并，不负责构建）。
用法（在项目根目录）：

    idfc build -t release
    python tools/make_full_firmware.py -t release

产物默认写到 dist/<项目名>_<variant>_full.bin，并附 .sha256。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def parse_size(text: str) -> int:
    """把 '16MB' / '4M' / '0x6000' / '3072' 这类写法统一转成字节数。"""
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    m = re.fullmatch(r"(\d+)\s*([KMG]?)B?", text, re.IGNORECASE)
    if not m:
        raise ValueError(f"无法解析大小: {text!r}")
    value = int(m.group(1))
    unit = m.group(2).upper()
    scale = {"": 1, "K": 1024, "M": 1024**2, "G": 1024**3}[unit]
    return value * scale


def read_partitions(csv_path: Path) -> dict[str, dict]:
    """读取 partitions.csv，返回 {name: {offset, size}}（忽略注释与空行）。"""
    parts: dict[str, dict] = {}
    for raw in csv_path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        name, _ptype, _subtype, offset, size = cols[:5]
        parts[name] = {"offset": int(offset, 0), "size": parse_size(size)}
    return parts


def fmt_bytes(n: int) -> str:
    return f"{n} B ({n / 1024 / 1024:.2f} MiB)"


def collect_segments(build_dir: Path, flasher: dict, partitions: dict) -> list[tuple[int, str, bytes]]:
    """按偏移收集待合并的数据段。"""
    segments: list[tuple[int, str, bytes]] = []

    # 1) bootloader / 分区表 / 应用：直接复用 ESP-IDF 生成的 flasher_args.json
    for offset_str, rel_path in flasher["flash_files"].items():
        path = build_dir / rel_path
        if not path.is_file():
            raise FileNotFoundError(f"缺少构建产物: {path}")
        segments.append((int(offset_str, 0), rel_path, path.read_bytes()))

    # 2) 系统字体：裸 TTF，必须落在 font 分区偏移 0
    font_off = partitions["font"]["offset"]
    font_size = partitions["font"]["size"]
    font_path = ROOT / "spiffs" / "system.ttf"
    if not font_path.is_file():
        raise FileNotFoundError(f"缺少字体文件: {font_path}")
    font_data = font_path.read_bytes()
    if len(font_data) > font_size:
        raise ValueError(
            f"字体 {fmt_bytes(len(font_data))} 超过 font 分区 {fmt_bytes(font_size)}，"
            "请先调整 partitions.csv 的 font 大小"
        )
    segments.append((font_off, "spiffs/system.ttf", font_data))

    segments.sort(key=lambda x: x[0])

    # 3) 防呆：段不得重叠
    for (off_a, name_a, data_a), (off_b, _name_b, _data_b) in zip(segments, segments[1:]):
        end_a = off_a + len(data_a)
        if end_a > off_b:
            raise ValueError(f"数据段重叠: {name_a} 结束于 0x{end_a:X}，下一段起于 0x{off_b:X}")

    # 4) 应用不得超出 factory 分区
    app_path = build_dir / flasher["app"]["file"]
    if app_path.stat().st_size > partitions["factory"]["size"]:
        raise ValueError("应用固件超过 factory 分区大小")

    return segments


def verify_image(buf: bytearray, segments: list[tuple[int, str, bytes]], partitions: dict) -> None:
    """检查镜像里的关键 magic 与"必须为空"的区域。"""
    font_off = next(off for off, name, _data in segments if name == "spiffs/system.ttf")

    checks = [
        ("bootloader magic 0xE9", buf[0] == 0xE9),
        ("分区表 magic AA 50", buf[0x8000:0x8002] == b"\xAA\x50"),
        ("应用镜像 magic 0xE9", buf[0x10000] == 0xE9),
        ("字体 sfnt magic 00 01 00 00", buf[font_off:font_off + 4] == b"\x00\x01\x00\x00"),
        # nvs / phy_init / pldata 必须是 0xFF，否则"清数据"不成立
        ("nvs 区为 0xFF", all(b == 0xFF for b in buf[0x9000:0xF000])),
        ("phy_init 区为 0xFF", all(b == 0xFF for b in buf[0xF000:0x10000])),
        (
            "pldata 区为 0xFF",
            all(b == 0xFF for b in buf[partitions["pldata"]["offset"]:partitions["pldata"]["offset"] + partitions["pldata"]["size"]]),
        ),
    ]
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if failed:
        raise SystemExit(f"镜像校验失败: {', '.join(failed)}")

    # 逐段确认字节与源文件完全一致
    for off, name, data in segments:
        if buf[off:off + len(data)] != data:
            raise SystemExit(f"镜像校验失败: {name} 在 0x{off:X} 处字节与源文件不一致")
    print("  [OK] 四段数据与源文件逐字节一致")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 bootloader / 分区表 / 应用 / 系统字体合并成单个全量固件（烧到 0x0）"
    )
    parser.add_argument("-t", "--variant", default="release", help="构建类型（debug/releasecandidate/release，默认 release）")
    parser.add_argument("--dac-chip", default="cs43131", help="dac_chip overlay（默认 cs43131）")
    parser.add_argument("--font", default="ttffont", help="font overlay（默认 ttffont）")
    parser.add_argument("--build-dir", type=Path, default=None, help="构建目录，默认 build/<dac>.<font>.<variant>")
    parser.add_argument("-o", "--output", type=Path, default=None, help="输出文件，默认 dist/<项目名>_<variant>_full.bin")
    args = parser.parse_args()

    build_dir = args.build_dir or ROOT / "build" / f"{args.dac_chip}.{args.font}.{args.variant}"
    flasher_path = build_dir / "flasher_args.json"
    if not flasher_path.is_file():
        print(f"错误：找不到 {flasher_path}\n请先执行：idfc build -t {args.variant}", file=sys.stderr)
        return 1
    flasher = json.loads(flasher_path.read_text(encoding="utf-8"))

    partitions = read_partitions(ROOT / "partitions.csv")
    flash_size = parse_size(flasher["flash_settings"]["flash_size"])

    project = Path(flasher["app"]["file"]).stem
    output = args.output or ROOT / "dist" / f"{project}_{args.variant}_full.bin"
    if not output.is_absolute():
        # 允许 "dist/xxx.bin" 这类相对路径，统一按项目根解析，避免受当前 shell 目录影响
        output = ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)

    print(f"构建目录 : {build_dir}")
    print(f"Flash    : {fmt_bytes(flash_size)}")
    print(f"输出     : {output}\n")

    segments = collect_segments(build_dir, flasher, partitions)

    # 整片预填 0xFF，再把各段覆盖进去；未被覆盖处自然就是"擦除态"
    buf = bytearray(b"\xFF" * flash_size)
    for off, name, data in segments:
        buf[off:off + len(data)] = data
        print(f"  {name:<44} 0x{off:08X}  {fmt_bytes(len(data))}")

    print("\n校验：")
    verify_image(buf, segments, partitions)

    output.write_bytes(buf)
    digest = hashlib.sha256(buf).hexdigest()
    (output.parent / (output.name + ".sha256")).write_text(f"{digest}  {output.name}\n", encoding="utf-8")

    print(f"\n已生成: {output}  ({fmt_bytes(len(buf))})")
    print(f"SHA256: {digest}")
    print(f"\n烧录（整片，含清空 nvs/pldata）:")
    print(f"  python -m esptool --chip esp32s3 -p COMx write-flash 0x0 \"{output.name}\"")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
