#!/usr/bin/env python3
"""Build /sdcard/pet pack.bin + RGBH body frames from pack.json."""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path

CLIP_IDS = {
    "idle": 0,
    "sleepy": 1,
    "eat": 2,
    "play": 3,
    "sad": 4,
    "sleep_loop": 5,
    "poke": 6,
}

BG = (0x20, 0x20, 0x20)
NAME_LEN = 32
REPO = Path(__file__).resolve().parents[2]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)


def write_rgbh(path: Path, w: int, h: int, pixels: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    hdr = b"RGBH" + struct.pack("<HHI", w, h, 0)
    path.write_bytes(hdr + pixels)


def draw_body(w: int, h: int, color: tuple[int, int, int], frame: int) -> bytes:
    cx = (w - 1) * 0.5
    cy = (h - 1) * 0.5 + (1 if frame else 0)
    r = min(w, h) * 0.42
    highlight = (min(255, color[0] + 50), min(255, color[1] + 50), min(255, color[2] + 40))
    out = bytearray()
    bg = rgb565(*BG)
    for y in range(h):
        for x in range(w):
            dx = x - cx
            dy = y - cy
            d = math.sqrt(dx * dx + dy * dy)
            if d > r:
                out += struct.pack("<H", bg)
                continue
            t = max(0.0, min(1.0, 1.0 - (d / r)))
            hx = dx / r - 0.25
            hy = dy / r + 0.25
            shine = max(0.0, 1.0 - math.sqrt(hx * hx + hy * hy) * 1.6) * 0.45
            rr = lerp(color[0], highlight[0], t * 0.4 + shine)
            gg = lerp(color[1], highlight[1], t * 0.4 + shine)
            bb = lerp(color[2], highlight[2], t * 0.4 + shine)
            out += struct.pack("<H", rgb565(rr, gg, bb))
    return bytes(out)


def pad_name(s: str) -> bytes:
    b = s.encode("ascii")[: NAME_LEN - 1]
    return b + b"\x00" * (NAME_LEN - len(b))


def main() -> None:
    cfg_path = Path(__file__).with_name("pack.json")
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    out_dir = REPO / "tools" / "pet_sim" / "sdcard" / "pet"
    body_dir = out_dir / "body"
    body_dir.mkdir(parents=True, exist_ok=True)

    w = int(cfg["width"])
    h = int(cfg["height"])
    needs = cfg["needs"]
    clips = cfg["clips"]

    blob = bytearray()
    blob += b"PETP"
    blob += struct.pack(
        "<HHHHHH BBBB",
        int(cfg["version"]),
        0,
        int(needs["hunger_decay_s"]),
        int(needs["mood_decay_s"]),
        int(needs["energy_decay_s"]),
        int(needs["energy_recover_s"]),
        int(needs["feed_hunger"]),
        int(needs["play_mood"]),
        int(needs["tap_mood"]),
        len(clips),
    )

    for clip in clips:
        cid = CLIP_IDS[clip["id"]]
        n = int(clip["frames"])
        fps = int(clip["fps"])
        color = tuple(int(c) for c in clip["color"])
        blob += struct.pack("BBBB", cid, n, fps, 0)
        for i in range(n):
            rel = f"body/{clip['id']}_{i}.bin"
            blob += pad_name(rel)
            pixels = draw_body(w, h, color, i)
            write_rgbh(out_dir / rel, w, h, pixels)

    (out_dir / "pack.bin").write_bytes(blob)
    print(f"wrote {out_dir / 'pack.bin'} ({len(blob)} bytes), {len(clips)} clips {w}x{h}")


if __name__ == "__main__":
    main()
