#!/usr/bin/env python3
"""CLI: build /sdcard/pet pack.bin + optional boot/splash.bin."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from skin_core import (
    BG,
    DEFAULT_CFG,
    DEFAULT_OUT,
    SPLASH_CONTENT_DEFAULT,
    build_pack,
    build_splash,
    idle_color_from_cfg,
    load_cfg,
    parse_rgb,
)


def main() -> None:
    ap = argparse.ArgumentParser(description="pet_skin: build pack.bin (+ optional static splash)")
    ap.add_argument(
        "--splash",
        nargs="?",
        const="",
        default=None,
        metavar="IMAGE",
        help="Write boot/splash.bin (static); optional PNG/JPEG (else synthetic)",
    )
    ap.add_argument(
        "--fit",
        choices=("contain", "cover"),
        default="contain",
        help="How image fits into content box (default contain)",
    )
    ap.add_argument(
        "--bg",
        default=None,
        metavar="RGB",
        help="Splash canvas bg #RRGGBB or R,G,B (default #202020)",
    )
    ap.add_argument(
        "--size",
        type=int,
        default=None,
        metavar="PX",
        help=f"Content size inside fixed 240 canvas "
        f"(default: {SPLASH_CONTENT_DEFAULT} synthetic / 240 image)",
    )
    ap.add_argument(
        "--body-color",
        default=None,
        metavar="RGB",
        help="Synthetic body color #RRGGBB or R,G,B (default idle from pack.json)",
    )
    ap.add_argument("--cfg", type=Path, default=DEFAULT_CFG, help="pack.json path")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output pet/ root")
    args = ap.parse_args()

    cfg = load_cfg(args.cfg)
    out_dir = args.out
    print(build_pack(cfg, out_dir, args.cfg))

    if args.splash is not None:
        src = Path(args.splash) if args.splash else None
        if src is not None and not src.is_file():
            print(f"error: splash image not found: {src}", file=sys.stderr)
            raise SystemExit(1)
        bg = parse_rgb(args.bg) if args.bg else BG
        body = parse_rgb(args.body_color) if args.body_color else idle_color_from_cfg(cfg)
        print(build_splash(out_dir, src, args.fit, body, cfg, bg, args.size))


if __name__ == "__main__":
    main()
