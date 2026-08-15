#!/usr/bin/env python3
"""Build LVGL binary caption font for desktop_pet SD card.

Output path on device: /sdcard/pet/font/caption.bin
(relative to content_root, default pet/)

Requires Node.js (npx lv_font_conv) and a CJK TTF/OTF, e.g. Source Han Sans SC
or Noto Sans SC.

Examples:
  py -3 tools/pet_tool/font/make_caption_bin.py --font path/to/NotoSansSC-Regular.otf
  py -3 tools/pet_tool/font/make_caption_bin.py --font X.otf --out D:/sdcard/pet/font/caption.bin
  py -3 tools/pet_tool/font/make_caption_bin.py --font X.otf --size 14 --bpp 4

Charset default: ASCII + CJK punctuation + full GB2312 (~6.7k Han).
Use --ascii-only to smoke-test the pipeline without a CJK face.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def gb2312_han_string() -> str:
    """Decode every valid GB2312 double-byte → Unicode (level 1+2 + symbols)."""
    chars: list[str] = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes((hi, lo)).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    # Stable unique order
    seen: set[str] = set()
    out: list[str] = []
    for c in chars:
        if c not in seen:
            seen.add(c)
            out.append(c)
    return "".join(out)


def build_symbols(ascii_only: bool) -> str:
    punct = "，。！？、；：""''（）【】《》…—·￥"
    if ascii_only:
        return punct
    return punct + gb2312_han_string()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", required=True, help="TTF/OTF with CJK glyphs")
    ap.add_argument(
        "--out",
        default=str(Path("tools/pet_tool/font/out/caption.bin")),
        help="Output LVGL .bin (copy to SD as pet/font/caption.bin)",
    )
    ap.add_argument("--size", type=int, default=14, help="Pixel size (default 14)")
    ap.add_argument("--bpp", type=int, default=4, choices=(1, 2, 4, 8), help="Bits per pixel")
    ap.add_argument("--ascii-only", action="store_true", help="Skip GB2312 (pipeline test)")
    ap.add_argument("--lv-font-conv", default="lv_font_conv", help="Converter binary / npx package")
    args = ap.parse_args()

    font = Path(args.font)
    if not font.is_file():
        print(f"error: font not found: {font}", file=sys.stderr)
        return 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    symbols = build_symbols(args.ascii_only)
    print(f"symbols: {len(symbols)} codepoints → {out}")

    # Prefer npx so users need not global-install
    npx = shutil.which("npx")
    if npx:
        cmd = [
            npx,
            "--yes",
            "lv_font_conv@1.5.2",
            "--font",
            str(font),
            "--size",
            str(args.size),
            "--bpp",
            str(args.bpp),
            "--format",
            "bin",
            "--no-compress",
            "--no-prefilter",
            "-r",
            "0x20-0x7F",
            "--symbols",
            symbols,
            "-o",
            str(out),
        ]
    else:
        conv = shutil.which(args.lv_font_conv)
        if not conv:
            print("error: need Node.js (npx) or lv_font_conv on PATH", file=sys.stderr)
            return 1
        cmd = [
            conv,
            "--font",
            str(font),
            "--size",
            str(args.size),
            "--bpp",
            str(args.bpp),
            "--format",
            "bin",
            "--no-compress",
            "--no-prefilter",
            "-r",
            "0x20-0x7F",
            "--symbols",
            symbols,
            "-o",
            str(out),
        ]

    # Windows cmd length limit: write symbols via temp file is not supported by
    # lv_font_conv; for GB2312 the argv is large but usually OK. If it fails,
    # fall back to writing a tiny stub and tell user to use WSL / shorter set.
    try:
        print("running:", " ".join(cmd[:8]), "... --symbols <omitted> ...")
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"error: lv_font_conv failed ({e.returncode})", file=sys.stderr)
        return e.returncode or 1
    except OSError as e:
        # Command line too long on some Windows setups
        print(f"error: cannot exec converter: {e}", file=sys.stderr)
        print("hint: run under WSL, or pass --ascii-only then expand ranges manually", file=sys.stderr)
        return 1

    if not out.is_file() or out.stat().st_size < 64:
        print(f"error: output missing/too small: {out}", file=sys.stderr)
        return 1

    print(f"ok: {out} ({out.stat().st_size} bytes)")
    print("copy to SD:  <card>/pet/font/caption.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
