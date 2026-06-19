#!/usr/bin/env python3
"""Generate 240x135 MJPEG AVI demo for ESP32-S3 lcd_video (no FFmpeg required)."""

from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W = 240
H = 135
FPS = 12
SECONDS = 5
QUALITY = 85
OUT_DEFAULT = Path(__file__).resolve().parent / "demo.avi"


def pack_strf(width: int, height: int) -> bytes:
    comp = struct.unpack("<I", b"MJPG")[0]
    return struct.pack(
        "<IiiHHIIiiii",
        40,
        width,
        height,
        1,
        24,
        comp,
        0,
        0,
        0,
        0,
        0,
    )


def pack_strh(num_frames: int, fps: int, width: int, height: int) -> bytes:
    return struct.pack(
        "<4s4sIHHIIIIIIII4h",
        b"vids",
        b"MJPG",
        0,
        0,
        0,
        0,
        1,
        fps,
        0,
        num_frames,
        65536,
        0xFFFFFFFF,
        0,
        0,
        0,
        width,
        height,
    )


def pack_avih(num_frames: int, fps: int, width: int, height: int) -> bytes:
    us_per_frame = 1_000_000 // fps
    max_bps = 400_000
    return struct.pack(
        "<14I",
        us_per_frame,
        max_bps,
        0,
        0x10,
        num_frames,
        0,
        1,
        65536,
        width,
        height,
        0,
        0,
        0,
        0,
    )


def write_chunk(buf: bytearray, fourcc: bytes, data: bytes) -> None:
    buf.extend(fourcc)
    buf.extend(struct.pack("<I", len(data)))
    buf.extend(data)
    if len(data) & 1:
        buf.append(0)


def write_list(buf: bytearray, list_type: bytes, body: bytes) -> None:
    buf.extend(b"LIST")
    buf.extend(struct.pack("<I", len(body) + 4))
    buf.extend(list_type)
    buf.extend(body)


def make_jpeg_frame(index: int, total: int) -> bytes:
    img = Image.new("RGB", (W, H))
    draw = ImageDraw.Draw(img)
    phase = index / max(total - 1, 1)
    base = int(phase * 200)
    for y in range(H):
        r = (base + y * 2 + index * 5) % 256
        g = (base + index * 7 + y) % 256
        b = (255 - base + y * 3) % 256
        draw.line([(0, y), (W - 1, y)], fill=(r, g, b))
    draw.rectangle([8, 8, W - 9, H - 9], outline=(255, 255, 255), width=2)
    draw.text((16, 36), "ESP32-S3 demo.avi", fill=(255, 255, 0))
    draw.text((16, 58), f"{W}x{H} MJPEG {FPS}fps", fill=(200, 255, 200))
    draw.text((16, 80), f"frame {index + 1}/{total}", fill=(255, 255, 255))
    bar_w = int((W - 32) * (index + 1) / total)
    draw.rectangle([16, 104, 16 + bar_w, 118], fill=(0, 180, 255))
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=QUALITY)
    return out.getvalue()


def build_avi(jpeg_frames: list[bytes], fps: int) -> bytes:
    n = len(jpeg_frames)
    hdrl_body = bytearray()
    write_chunk(hdrl_body, b"avih", pack_avih(n, fps, W, H))

    strl_body = bytearray()
    write_chunk(strl_body, b"strh", pack_strh(n, fps, W, H))
    write_chunk(strl_body, b"strf", pack_strf(W, H))
    write_list(hdrl_body, b"strl", bytes(strl_body))

    movi_body = bytearray()
    for jpeg in jpeg_frames:
        write_chunk(movi_body, b"00dc", jpeg)

    riff_body = bytearray()
    write_list(riff_body, b"hdrl", bytes(hdrl_body))
    write_list(riff_body, b"movi", bytes(movi_body))

    out = bytearray()
    out.extend(b"RIFF")
    out.extend(struct.pack("<I", len(riff_body) + 4))
    out.extend(b"AVI ")
    out.extend(riff_body)
    return bytes(out)


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT_DEFAULT
    total = FPS * SECONDS
    print(f"Generating {total} frames ({W}x{H}, {FPS} fps, {SECONDS}s) -> {out_path}")
    frames = [make_jpeg_frame(i, total) for i in range(total)]
    data = build_avi(frames, FPS)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    print(f"Done: {out_path} ({len(data) / 1024:.1f} KiB)")
    print("Copy to SD card: /sdcard/videos/demo.avi")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
