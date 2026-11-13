#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成"全量固件"（单文件、从 0x0 一次烧录完成整片更新）。

把下面四段按分区表偏移拼进一个 16 MiB 的镜像，其余区域一律填 0xFF：

    0x000000  bootloader.bin
    0x008000  partition-table.bin
    0x010000  <项目名>.bin        （应用固件，不含字体）
    0x600000  spiffs/system.npf   （NPF1 全字库，4.44MiB；偏移/大小取自 partitions.csv）

因为 nvs(0x9000)、phy_init(0xF000)、pldata(0xC00000) 都在镜像范围内且为 0xFF，
把它烧到 0x0 就等于"整片擦除后重刷"：配置(nvs)和播放列表(pldata)都会被清掉。

⚠ 2026-11 起系统字体是 **font 分区里的 NPF1 位图全字库**（运行时 mmap，见
main/core/fonts/font_npf.c 与 ideas_and_exps/NPF_FONT.md），不再编译进固件、
也不再是 TTF；因此本脚本把 spiffs/system.npf 一并合并进来（`--no-font` 可跳过，
跳过时要另外刷字体）。

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


def collect_segments(build_dir: Path, flasher: dict, partitions: dict,
                     font_path: Path | None) -> list[tuple[int, str, bytes]]:
    """按偏移收集待合并的数据段。"""
    segments: list[tuple[int, str, bytes]] = []

    # bootloader / 分区表 / 应用：直接复用 ESP-IDF 生成的 flasher_args.json
    for offset_str, rel_path in flasher["flash_files"].items():
        path = build_dir / rel_path
        if not path.is_file():
            raise FileNotFoundError(f"缺少构建产物: {path}")
        segments.append((int(offset_str, 0), rel_path, path.read_bytes()))

    # 字体：NPF1 全字库放 font 分区（裸镜像，偏移从 partitions.csv 读）
    if font_path is not None:
        if "font" not in partitions:
            raise ValueError("partitions.csv 里没有 font 分区，无法合并字体")
        data = font_path.read_bytes()
        if data[:4] != b"NPF1":
            raise ValueError(f"{font_path} 不是 NPF1 字体镜像（magic 不是 NPF1）")
        if len(data) > partitions["font"]["size"]:
            raise ValueError(f"字体镜像 {len(data)} B 超过 font 分区 {partitions['font']['size']} B")
        segments.append((partitions["font"]["offset"], str(font_path), data))

    segments.sort(key=lambda x: x[0])

    # 防呆：段不得重叠
    for (off_a, name_a, data_a), (off_b, _name_b, _data_b) in zip(segments, segments[1:]):
        end_a = off_a + len(data_a)
        if end_a > off_b:
            raise ValueError(f"数据段重叠: {name_a} 结束于 0x{end_a:X}，下一段起于 0x{off_b:X}")

    # 应用不得超出 factory 分区
    app_path = build_dir / flasher["app"]["file"]
    if app_path.stat().st_size > partitions["factory"]["size"]:
        raise ValueError("应用固件超过 factory 分区大小")

    return segments


def verify_image(buf: bytearray, segments: list[tuple[int, str, bytes]], partitions: dict,
                 has_font: bool) -> None:
    """检查镜像里的关键 magic 与"必须为空"的区域。"""
    checks = [
        ("bootloader magic 0xE9", buf[0] == 0xE9),
        ("分区表 magic AA 50", buf[0x8000:0x8002] == b"\xAA\x50"),
        ("应用镜像 magic 0xE9", buf[0x10000] == 0xE9),
        # nvs / phy_init / pldata 必须是 0xFF，否则"清数据"不成立
        ("nvs 区为 0xFF", all(b == 0xFF for b in buf[0x9000:0xF000])),
        ("phy_init 区为 0xFF", all(b == 0xFF for b in buf[0xF000:0x10000])),
        (
            "pldata 区为 0xFF",
            all(b == 0xFF for b in buf[partitions["pldata"]["offset"]:partitions["pldata"]["offset"] + partitions["pldata"]["size"]]),
        ),
    ]
    # font 分区：合并了字体就必须是 NPF1 镜像，没合并就必须是空的
    if "font" in partitions:
        font_off = partitions["font"]["offset"]
        font_size = partitions["font"]["size"]
        if has_font:
            checks.append(("font 分区为 NPF1 镜像", buf[font_off:font_off + 4] == b"NPF1"))
        else:
            checks.append(("font 分区空置（0xFF）", all(b == 0xFF for b in buf[font_off:font_off + font_size])))

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if failed:
        raise SystemExit(f"镜像校验失败: {', '.join(failed)}")

    # 逐段确认字节与源文件完全一致
    for off, name, data in segments:
        if buf[off:off + len(data)] != data:
            raise SystemExit(f"镜像校验失败: {name} 在 0x{off:X} 处字节与源文件不一致")
    print("  [OK] 各数据段与源文件逐字节一致")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 bootloader / 分区表 / 应用 / 字体合并成单个全量固件（烧到 0x0）"
    )
    parser.add_argument("-t", "--variant", default="release", help="构建类型（debug/release，默认 release）")
    parser.add_argument("--build-dir", type=Path, default=None, help="构建目录，默认 build/<variant>")
    parser.add_argument("--font", type=Path, default=ROOT / "spiffs" / "system.npf",
                        help="NPF1 字体镜像，默认 spiffs/system.npf")
    parser.add_argument("--no-font", action="store_true", help="不合并字体（font 分区保持 0xFF）")
    parser.add_argument("-o", "--output", type=Path, default=None, help="输出文件，默认 dist/<项目名>_<variant>_full.bin")
    args = parser.parse_args()

    font_path = None if args.no_font else args.font
    if font_path is not None:
        if not font_path.is_absolute():
            font_path = ROOT / font_path
        if not font_path.is_file():
            print(f"错误：找不到字体镜像 {font_path}\n"
                  f"      生成方法见 tools/font/README.md；或用 --no-font 跳过（之后单独刷字体）",
                  file=sys.stderr)
            return 1

    build_dir = args.build_dir or ROOT / "build" / args.variant
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

    segments = collect_segments(build_dir, flasher, partitions, font_path)

    # 整片预填 0xFF，再把各段覆盖进去；未被覆盖处自然就是"擦除态"
    buf = bytearray(b"\xFF" * flash_size)
    for off, name, data in segments:
        buf[off:off + len(data)] = data
        print(f"  {name:<44} 0x{off:08X}  {fmt_bytes(len(data))}")

    print("\n校验：")
    verify_image(buf, segments, partitions, font_path is not None)

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
