#!/usr/bin/env python3
"""skin_core — unique write backend for pet_skin (CLI + Qt GUI).

Splash is static single-frame boot/splash.bin only. boot/anim is deferred.
Canvas is always 240×240; content (image / body) size and colors are adjustable.
"""

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
SPLASH_SIZE = 240
# Content box inside fixed 240 canvas (body / image). Matches firmware fallback ø96.
SPLASH_CONTENT_DEFAULT = 96
SPLASH_CONTENT_MIN = 32
SPLASH_CONTENT_MAX = SPLASH_SIZE
# Body frame size (matches firmware PET_RES_FRAME_MAX_*).
BODY_SIZE_DEFAULT = 96
BODY_SIZE_MIN = 48
BODY_SIZE_MAX = 180
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_CFG = HERE / "pack.json"
DEFAULT_OUT = REPO / "tools" / "pet_sim" / "sdcard" / "pet"

# Declared / reserved skin paths (tool extension map). Firmware may lag.
# boot/anim is product-reserved but deferred — splash stays static.
DECLARED_PATHS = (
    "pack.bin",
    "body/*.bin",
    "boot/splash.bin",
)
RESERVED_PATHS = (
    "sfx/",
    "font/",
    "theme/",
    "boot/anim/",  # deferred; do not implement in tool v1
)


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb(pix: int) -> tuple[int, int, int]:
    r = ((pix >> 11) & 0x1F) * 255 // 31
    g = ((pix >> 5) & 0x3F) * 255 // 63
    b = (pix & 0x1F) * 255 // 31
    return (r, g, b)


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)


def write_rgbh(path: Path, w: int, h: int, pixels: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    hdr = b"RGBH" + struct.pack("<HHI", w, h, 0)
    path.write_bytes(hdr + pixels)


def draw_body_rows(
    w: int, h: int, color: tuple[int, int, int], frame: int
) -> list[list[tuple[int, int, int] | None]]:
    """Body disk; None outside the disk (caller fills canvas bg)."""
    cx = (w - 1) * 0.5
    cy = (h - 1) * 0.5 + (1 if frame else 0)
    r = min(w, h) * 0.42
    highlight = (min(255, color[0] + 50), min(255, color[1] + 50), min(255, color[2] + 40))
    rows: list[list[tuple[int, int, int] | None]] = [[None] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            dx = x - cx
            dy = y - cy
            d = math.sqrt(dx * dx + dy * dy)
            if d > r:
                continue
            t = max(0.0, min(1.0, 1.0 - (d / r)))
            hx = dx / r - 0.25
            hy = dy / r + 0.25
            shine = max(0.0, 1.0 - math.sqrt(hx * hx + hy * hy) * 1.6) * 0.45
            rr = lerp(color[0], highlight[0], t * 0.4 + shine)
            gg = lerp(color[1], highlight[1], t * 0.4 + shine)
            bb = lerp(color[2], highlight[2], t * 0.4 + shine)
            rows[y][x] = (rr, gg, bb)
    return rows


def draw_body(w: int, h: int, color: tuple[int, int, int], frame: int) -> bytes:
    rows = draw_body_rows(w, h, color, frame)
    filled = [[(c if c is not None else BG) for c in row] for row in rows]
    return pixels_from_rgb_rows(filled, w, h)


def pad_name(s: str) -> bytes:
    b = s.encode("ascii")[: NAME_LEN - 1]
    return b + b"\x00" * (NAME_LEN - len(b))


def pixels_from_rgb_rows(rows: list[list[tuple[int, int, int]]], w: int, h: int) -> bytes:
    out = bytearray()
    for y in range(h):
        row = rows[y]
        for x in range(w):
            r, g, b = row[x]
            out += struct.pack("<H", rgb565(r, g, b))
    return bytes(out)


def clamp_content_size(size: int) -> int:
    return max(SPLASH_CONTENT_MIN, min(SPLASH_CONTENT_MAX, int(size)))


def parse_rgb(value: str | tuple[int, int, int] | list[int]) -> tuple[int, int, int]:
    """Accept '#RRGGBB', 'R,G,B', or (r,g,b)."""
    if isinstance(value, (tuple, list)) and len(value) == 3:
        return (int(value[0]) & 255, int(value[1]) & 255, int(value[2]) & 255)
    s = str(value).strip()
    if s.startswith("#") and len(s) == 7:
        return (int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16))
    parts = [p.strip() for p in s.replace(";", ",").split(",")]
    if len(parts) != 3:
        raise ValueError(f"bad rgb: {value!r}")
    return (int(parts[0]) & 255, int(parts[1]) & 255, int(parts[2]) & 255)


def fit_rgba_to_box(
    src_w: int,
    src_h: int,
    get_rgba,
    box: int,
    mode: str,
) -> list[list[tuple[int, int, int] | None]]:
    """Fit source into box×box; None = transparent (keep canvas bg)."""
    if mode == "cover":
        scale = max(box / src_w, box / src_h)
    else:
        scale = min(box / src_w, box / src_h)
    dw = max(1, int(round(src_w * scale)))
    dh = max(1, int(round(src_h * scale)))
    ox = (box - dw) // 2
    oy = (box - dh) // 2
    rows: list[list[tuple[int, int, int] | None]] = [[None] * box for _ in range(box)]
    for y in range(box):
        for x in range(box):
            sx = x - ox
            sy = y - oy
            if sx < 0 or sy < 0 or sx >= dw or sy >= dh:
                continue
            u = (sx + 0.5) / dw * src_w - 0.5
            v = (sy + 0.5) / dh * src_h - 0.5
            ix = int(max(0, min(src_w - 1, round(u))))
            iy = int(max(0, min(src_h - 1, round(v))))
            r, g, b, a = get_rgba(ix, iy)
            if a < 128:
                continue
            rows[y][x] = (r, g, b)
    return rows


def compose_splash_canvas(
    content_rows: list[list[tuple[int, int, int] | None]],
    content_w: int,
    content_h: int,
    bg: tuple[int, int, int],
    offset_y: int = -8,
) -> bytes:
    """Place content centered on fixed SPLASH_SIZE canvas with solid bg."""
    canvas = SPLASH_SIZE
    ox = (canvas - content_w) // 2
    oy = (canvas - content_h) // 2 + offset_y
    rows: list[list[tuple[int, int, int]]] = [[bg] * canvas for _ in range(canvas)]
    for y in range(canvas):
        for x in range(canvas):
            bx = x - ox
            by = y - oy
            if bx < 0 or by < 0 or bx >= content_w or by >= content_h:
                continue
            pix = content_rows[by][bx]
            if pix is not None:
                rows[y][x] = pix
    return pixels_from_rgb_rows(rows, canvas, canvas)


def fit_rgba_to_canvas(
    src_w: int,
    src_h: int,
    get_rgba,
    canvas: int,
    mode: str,
    bg: tuple[int, int, int],
    content_size: int | None = None,
) -> bytes:
    box = clamp_content_size(content_size if content_size is not None else canvas)
    content = fit_rgba_to_box(src_w, src_h, get_rgba, box, mode)
    if box >= canvas:
        filled = [[(c if c is not None else bg) for c in row] for row in content]
        return pixels_from_rgb_rows(filled, canvas, canvas)
    return compose_splash_canvas(content, box, box, bg)


def load_image_splash(
    src: Path,
    fit: str,
    bg: tuple[int, int, int] = BG,
    content_size: int = SPLASH_SIZE,
) -> bytes:
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow required for splash image. Install: py -3 -m pip install Pillow"
        ) from exc

    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size

    def get_rgba(x: int, y: int) -> tuple[int, int, int, int]:
        return px[x, y]

    return fit_rgba_to_canvas(w, h, get_rgba, SPLASH_SIZE, fit, bg, content_size)


def synthetic_splash(
    color: tuple[int, int, int],
    bg: tuple[int, int, int] = BG,
    content_size: int = SPLASH_CONTENT_DEFAULT,
) -> bytes:
    size = clamp_content_size(content_size)
    return compose_splash_canvas(draw_body_rows(size, size, color, 0), size, size, bg)


def load_cfg(cfg_path: Path | None = None) -> dict:
    path = cfg_path or DEFAULT_CFG
    return json.loads(path.read_text(encoding="utf-8"))


def save_cfg(cfg: dict, cfg_path: Path | None = None) -> Path:
    path = Path(cfg_path or DEFAULT_CFG)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


def clamp_body_size(size: int) -> int:
    return max(BODY_SIZE_MIN, min(BODY_SIZE_MAX, int(size)))


def idle_color_from_cfg(cfg: dict) -> tuple[int, int, int]:
    for clip in cfg.get("clips", []):
        if clip.get("id") == "idle":
            return tuple(int(c) for c in clip["color"])
    return (74, 163, 200)


def resolve_asset(path_str: str, base_dir: Path) -> Path:
    p = Path(path_str)
    if p.is_file():
        return p
    cand = (base_dir / path_str).resolve()
    if cand.is_file():
        return cand
    raise FileNotFoundError(f"asset not found: {path_str} (base={base_dir})")


def load_image_body(
    src: Path,
    w: int,
    h: int,
    fit: str = "contain",
    bg: tuple[int, int, int] = BG,
) -> bytes:
    """Fit image into w×h body frame (RGBH payload, no header)."""
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow required for body image. Install: py -3 -m pip install Pillow"
        ) from exc

    im = Image.open(src).convert("RGBA")
    px = im.load()
    sw, sh = im.size

    def get_rgba(x: int, y: int) -> tuple[int, int, int, int]:
        return px[x, y]

    # Reuse box fitter; body frames are square in practice.
    box = max(w, h)
    content = fit_rgba_to_box(sw, sh, get_rgba, box, fit)
    # Crop/pad to exact w×h centered.
    rows: list[list[tuple[int, int, int]]] = [[bg] * w for _ in range(h)]
    ox = (box - w) // 2
    oy = (box - h) // 2
    for y in range(h):
        for x in range(w):
            c = content[y + oy][x + ox]
            rows[y][x] = c if c is not None else bg
    return pixels_from_rgb_rows(rows, w, h)


def rgb_rows_from_rgb565(pixels: bytes, w: int, h: int) -> list[list[tuple[int, int, int]]]:
    need = w * h * 2
    if len(pixels) < need:
        raise ValueError("RGB565 payload too short")
    rows: list[list[tuple[int, int, int]]] = []
    i = 0
    for _y in range(h):
        row: list[tuple[int, int, int]] = []
        for _x in range(w):
            pix = pixels[i] | (pixels[i + 1] << 8)
            row.append(rgb565_to_rgb(pix))
            i += 2
        rows.append(row)
    return rows


def make_clip_frame_pixels(
    clip: dict,
    frame_index: int,
    w: int,
    h: int,
    base_dir: Path,
    bg: tuple[int, int, int] = BG,
) -> bytes:
    """Build one body frame: per-frame sources[] > source > synthetic color disk."""
    fit = str(clip.get("fit", "contain"))
    sources = clip.get("sources")
    if isinstance(sources, list) and frame_index < len(sources) and sources[frame_index]:
        return load_image_body(resolve_asset(str(sources[frame_index]), base_dir), w, h, fit, bg)
    source = clip.get("source")
    if source:
        return load_image_body(resolve_asset(str(source), base_dir), w, h, fit, bg)
    color = tuple(int(c) for c in clip.get("color", (74, 163, 200)))
    return draw_body(w, h, color, frame_index)


def compose_home_preview(
    body_pixels: bytes,
    body_w: int,
    body_h: int,
    bg: tuple[int, int, int] = BG,
    with_face: bool = True,
) -> bytes:
    """Place body on 240×240 home canvas; optional LVGL-like face for authoring."""
    canvas = SPLASH_SIZE
    rows: list[list[tuple[int, int, int]]] = [[bg] * canvas for _ in range(canvas)]
    ox = (canvas - body_w) // 2
    oy = (canvas - body_h) // 2 - 6
    body_rows = rgb_rows_from_rgb565(body_pixels, body_w, body_h)
    for y in range(body_h):
        dy = oy + y
        if dy < 0 or dy >= canvas:
            continue
        for x in range(body_w):
            dx = ox + x
            if dx < 0 or dx >= canvas:
                continue
            rows[dy][dx] = body_rows[y][x]

    if with_face:
        cx = canvas // 2
        cy = canvas // 2 - 6
        _stamp_disk(rows, canvas, cx - 18, cy - 8, 9, 11, (255, 255, 255))
        _stamp_disk(rows, canvas, cx + 18, cy - 8, 9, 11, (255, 255, 255))
        _stamp_disk(rows, canvas, cx - 18, cy - 8, 4, 5, (32, 32, 40))
        _stamp_disk(rows, canvas, cx + 18, cy - 8, 4, 5, (32, 32, 40))
        _stamp_disk(rows, canvas, cx, cy + 28, 14, 4, (224, 112, 128))
    return pixels_from_rgb_rows(rows, canvas, canvas)


def _stamp_disk(
    rows: list[list[tuple[int, int, int]]],
    canvas: int,
    cx: int,
    cy: int,
    rx: int,
    ry: int,
    color: tuple[int, int, int],
) -> None:
    for y in range(max(0, cy - ry), min(canvas, cy + ry + 1)):
        for x in range(max(0, cx - rx), min(canvas, cx + rx + 1)):
            if ((x - cx) / max(1, rx)) ** 2 + ((y - cy) / max(1, ry)) ** 2 <= 1.0:
                rows[y][x] = color


def make_splash_pixels(
    src: Path | None,
    fit: str = "contain",
    idle_color: tuple[int, int, int] | None = None,
    cfg: dict | None = None,
    bg: tuple[int, int, int] = BG,
    content_size: int | None = None,
) -> bytes:
    color = idle_color
    if color is None:
        color = idle_color_from_cfg(cfg or load_cfg())
    if src is not None:
        size = content_size if content_size is not None else SPLASH_SIZE
        return load_image_splash(src, fit, bg, size)
    size = content_size if content_size is not None else SPLASH_CONTENT_DEFAULT
    return synthetic_splash(color, bg, size)


def rgb565_to_qimage_bytes(pixels: bytes, w: int, h: int) -> bytes:
    """RGB888 packed bytes for QImage.Format_RGB888."""
    need = w * h * 2
    if len(pixels) < need:
        raise ValueError("RGB565 payload too short")
    out = bytearray(w * h * 3)
    o = 0
    for i in range(0, need, 2):
        pix = pixels[i] | (pixels[i + 1] << 8)
        r, g, b = rgb565_to_rgb(pix)
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b
        o += 3
    return bytes(out)


def build_pack(cfg: dict, out_dir: Path, cfg_path: Path | None = None) -> str:
    """Write pack.bin + body/*.bin. Optional image sources resolved vs cfg_path parent."""
    body_dir = out_dir / "body"
    body_dir.mkdir(parents=True, exist_ok=True)

    w = clamp_body_size(int(cfg["width"]))
    h = clamp_body_size(int(cfg["height"]))
    cfg["width"] = w
    cfg["height"] = h
    needs = cfg["needs"]
    clips = cfg["clips"]
    base_dir = Path(cfg_path).resolve().parent if cfg_path is not None else HERE

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

    img_n = 0
    for clip in clips:
        cid = CLIP_IDS[clip["id"]]
        n = int(clip["frames"])
        fps = int(clip["fps"])
        blob += struct.pack("BBBB", cid, n, fps, 0)
        for i in range(n):
            rel = f"body/{clip['id']}_{i}.bin"
            blob += pad_name(rel)
            pixels = make_clip_frame_pixels(clip, i, w, h, base_dir)
            write_rgbh(out_dir / rel, w, h, pixels)
            if clip.get("source") or clip.get("sources"):
                img_n += 1

    path = out_dir / "pack.bin"
    path.write_bytes(blob)
    note = f", {img_n} image frames" if img_n else ", synthetic colors"
    return f"wrote {path} ({len(blob)} bytes), {len(clips)} clips {w}x{h}{note}"


def build_splash(
    out_dir: Path,
    src: Path | None,
    fit: str = "contain",
    idle_color: tuple[int, int, int] | None = None,
    cfg: dict | None = None,
    bg: tuple[int, int, int] = BG,
    content_size: int | None = None,
) -> str:
    pixels = make_splash_pixels(src, fit, idle_color, cfg, bg, content_size)
    path = out_dir / "boot" / "splash.bin"
    write_rgbh(path, SPLASH_SIZE, SPLASH_SIZE, pixels)
    size = (
        content_size
        if content_size is not None
        else (SPLASH_SIZE if src is not None else SPLASH_CONTENT_DEFAULT)
    )
    bg_hex = f"#{bg[0]:02x}{bg[1]:02x}{bg[2]:02x}"
    if src is not None:
        note = f"from {src} fit={fit} content={size} bg={bg_hex}"
    else:
        if idle_color is not None:
            c_hex = f"#{idle_color[0]:02x}{idle_color[1]:02x}{idle_color[2]:02x}"
            note = f"synthetic body={size} color={c_hex} bg={bg_hex}"
        else:
            note = f"synthetic body={size} bg={bg_hex}"
    return f"wrote {path} ({12 + len(pixels)} bytes) {SPLASH_SIZE}x{SPLASH_SIZE} ({note})"
