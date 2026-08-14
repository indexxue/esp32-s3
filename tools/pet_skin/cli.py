#!/usr/bin/env python3
"""CLI: bind assets, check, build pack.bin + optional boot/splash.bin."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from skin_core import (
    DEFAULT_CFG,
    DEFAULT_OUT,
    SPLASH_CONTENT_DEFAULT,
    SPLASH_SIZE,
    bind_assets,
    build_pack,
    build_splash,
    check_pack,
    check_has_errors,
    export_skin_zip,
    idle_color_from_cfg,
    import_asset_folder,
    install_splash_image,
    load_cfg,
    parse_rgb,
    save_cfg,
    set_splash_cfg,
    splash_params,
    splash_src_from_assets,
)


def _log_lines(lines: list[str]) -> None:
    for line in lines:
        print(line)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="pet_skin: bind PNG assets → pack.bin + optional static splash"
    )
    ap.add_argument(
        "--import",
        dest="import_dir",
        type=Path,
        default=None,
        metavar="DIR",
        help="Copy known filenames (idle_0.png … splash.png) into assets/",
    )
    ap.add_argument(
        "--bind",
        action="store_true",
        help="Scan assets/ into pack.json source/sources, then save",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="Validate clips/assets; do not write binaries",
    )
    ap.add_argument(
        "--no-pack",
        action="store_true",
        help="Skip writing pack.bin / body/*.bin",
    )
    ap.add_argument(
        "--splash",
        nargs="?",
        const="",
        default=None,
        metavar="IMAGE",
        help="Write boot/splash.bin; omit IMAGE to use assets/splash.png or synthetic",
    )
    ap.add_argument(
        "--fit",
        choices=("contain", "cover"),
        default=None,
        help="How image fits into content box (default: pack.json splash.fit or contain)",
    )
    ap.add_argument(
        "--bg",
        default=None,
        metavar="RGB",
        help="Splash canvas bg #RRGGBB or R,G,B (default: pack.json splash.bg or #202020)",
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
    ap.add_argument(
        "--zip",
        nargs="?",
        const="",
        default=None,
        metavar="FILE",
        help="Write pet.zip for web upload (default: <out>/../pet.zip)",
    )
    args = ap.parse_args()

    cfg_path = args.cfg
    cfg = load_cfg(cfg_path)
    base = cfg_path.resolve().parent
    assets = base / "assets"

    if args.import_dir is not None:
        _log_lines(import_asset_folder(args.import_dir, assets))
        args.bind = True

    if args.bind:
        notes = bind_assets(cfg, base)
        _log_lines(notes)
        save_cfg(cfg, cfg_path)
        print(f"saved {cfg_path}")

    if args.check:
        warns = check_pack(cfg, cfg_path)
        if not warns:
            print("check: ok")
            return
        print("check:")
        _log_lines(warns)
        if check_has_errors(warns):
            raise SystemExit(1)
        return

    out_dir = args.out
    if not args.no_pack:
        print(build_pack(cfg, out_dir, cfg_path))

    if args.splash is not None:
        fit_cfg, bg_cfg, size_cfg = splash_params(cfg)
        fit = args.fit or fit_cfg
        bg = parse_rgb(args.bg) if args.bg else bg_cfg
        if args.splash:
            src = Path(args.splash)
            if not src.is_file():
                print(f"error: splash image not found: {src}", file=sys.stderr)
                raise SystemExit(1)
            src = install_splash_image(src, assets)
            size = args.size if args.size is not None else size_cfg
            set_splash_cfg(cfg, fit, bg, size if size is not None else SPLASH_SIZE)
            save_cfg(cfg, cfg_path)
        else:
            src = splash_src_from_assets(base)
            size = args.size if args.size is not None else size_cfg
        body = parse_rgb(args.body_color) if args.body_color else idle_color_from_cfg(cfg)
        print(build_splash(out_dir, src, fit, body, cfg, bg, size))

    if args.zip is not None:
        zpath = Path(args.zip) if args.zip else (out_dir.parent / "pet.zip")
        z = export_skin_zip(out_dir, zpath)
        print(f"wrote {z}")


if __name__ == "__main__":
    main()
