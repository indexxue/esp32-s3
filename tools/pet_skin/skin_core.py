#!/usr/bin/env python3
"""skin_core — unique write backend for pet_skin (CLI + Qt GUI).

Splash is static single-frame boot/splash.bin only. boot/anim is deferred.
Canvas is always 240×240; content (image / body) size and colors are adjustable.
"""

from __future__ import annotations

import json
import math
import shutil
import struct
import zipfile
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

FACE_PARTS = ("eye_l", "eye_r", "mouth", "brow_l", "brow_r")
FACE_REF_SIZE = 160
PACK_VERSION_FACE = 2
FACE_BLOCK_LEN = 32

BG = (0x20, 0x20, 0x20)
NAME_LEN = 32
SPLASH_SIZE = 240
# Content box inside fixed 240 canvas (body / image). Matches firmware fallback ø96.
SPLASH_CONTENT_DEFAULT = 96
SPLASH_CONTENT_MIN = 32
SPLASH_CONTENT_MAX = SPLASH_SIZE
# Body frame size (matches firmware PET_RES_FRAME_MAX_*).
BODY_SIZE_DEFAULT = 160
BODY_SIZE_MIN = 48
BODY_SIZE_MAX = 180
# Matches firmware PET_RES_MAX_FRAMES.
CLIP_FRAMES_MAX = 8
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_CFG = HERE / "pack.json"
DEFAULT_OUT = REPO / "tools" / "pet_sim" / "sdcard" / "pet"
CARD_CONFIG_NAME = "config"
CARD_CONFIG_TEXT = """# desktop_pet SD card map
# Lives at the SD mount root (/sdcard/config). Not inside /pet — skin replace must not overwrite this.
# Device reads KEY=VALUE after mount. Unknown keys are kept in RAM for later features.
# Lines starting with # are comments.

version=1
content_root=pet
record_root=record

# reserved (ignored until implemented):
# volume=80
# brightness=100
# locale=zh
"""

# Declared / reserved skin paths (tool extension map). Firmware may lag.
# boot/anim is product-reserved but deferred — splash stays static.
DECLARED_PATHS = (
    "pack.bin",
    "body/*.bin",
    "boot/splash.bin",
    "theme/ui/*.bin",
)
RESERVED_PATHS = (
    "sfx/",
    "font/",
    "theme/",  # theme/ui icons declared; other theme/* still reserved
    "boot/anim/",  # deferred; do not implement in tool v1
)

# UI button icons (28×28 RGBH) → theme/ui/{id}.bin; missing → firmware letter fallback
UI_ICON_SIZE = 28
UI_ICON_CARE_BG = (0x3A, 0x3A, 0x44)
UI_ICON_CHAT_BG = (0x3A, 0x55, 0x70)
# (bin_stem, assets_stem, button_bg)
UI_ICONS = (
    ("feed", "ui_feed", UI_ICON_CARE_BG),
    ("play", "ui_play", UI_ICON_CARE_BG),
    ("sleep", "ui_sleep", UI_ICON_CARE_BG),
    ("chat", "ui_chat", UI_ICON_CHAT_BG),
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
    if path.is_file():
        try:
            old = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            old = {}
        if isinstance(old, dict):
            for key, val in old.items():
                if key not in cfg:
                    cfg[key] = val
    path.write_text(json.dumps(cfg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


def clamp_body_size(size: int) -> int:
    return max(BODY_SIZE_MIN, min(BODY_SIZE_MAX, int(size)))


def default_face(size: int = FACE_REF_SIZE) -> dict:
    """Default face anchors relative to body top-left (ref 160×160)."""
    s = float(clamp_body_size(size)) / float(FACE_REF_SIZE)
    return {
        "eye_l": {"x": int(round(62 * s)), "y": int(round(72 * s)), "angle": 0},
        "eye_r": {"x": int(round(98 * s)), "y": int(round(72 * s)), "angle": 0},
        "mouth": {"x": int(round(80 * s)), "y": int(round(108 * s)), "angle": 0},
        "brow_l": {"x": int(round(62 * s)), "y": int(round(56 * s)), "angle": 0},
        "brow_r": {"x": int(round(98 * s)), "y": int(round(56 * s)), "angle": 0},
    }


def normalize_face(face: dict | None, size: int = FACE_REF_SIZE) -> dict:
    base = default_face(size)
    if not isinstance(face, dict):
        return base
    out: dict = {}
    for part in FACE_PARTS:
        src = face.get(part) if isinstance(face.get(part), dict) else {}
        out[part] = {
            "x": int(src.get("x", base[part]["x"])),
            "y": int(src.get("y", base[part]["y"])),
            "angle": int(src.get("angle", base[part]["angle"])),
        }
    return out


def face_for_frame(clip: dict, frame_index: int, size: int = FACE_REF_SIZE) -> dict:
    faces = clip.get("faces")
    if isinstance(faces, list) and frame_index < len(faces) and faces[frame_index]:
        return normalize_face(faces[frame_index], size)
    return normalize_face(clip.get("face"), size)


def pack_face_bytes(face: dict) -> bytes:
    blob = bytearray()
    for part in FACE_PARTS:
        p = face[part]
        blob += struct.pack("<hhh", int(p["x"]), int(p["y"]), int(p["angle"]))
    blob += struct.pack("<H", 0)
    assert len(blob) == FACE_BLOCK_LEN
    return bytes(blob)


def idle_color_from_cfg(cfg: dict) -> tuple[int, int, int]:
    for clip in cfg.get("clips", []):
        if clip.get("id") == "idle":
            return tuple(int(c) for c in clip["color"])
    return (74, 163, 200)


IMAGE_EXTS = (".png", ".webp", ".jpg", ".jpeg", ".bmp")
# Filenames from skin_ai_asset_brief.md (stems, any IMAGE_EXTS).
KNOWN_ASSET_STEMS = (
    "idle_0",
    "idle_1",
    "idle",
    "sleepy",
    "sad",
    "eat_0",
    "eat_1",
    "eat",
    "play_0",
    "play_1",
    "play",
    "poke",
    "sleep_loop",
    "splash",
    "ui_feed",
    "ui_play",
    "ui_sleep",
    "ui_chat",
)


def resolve_asset(path_str: str, base_dir: Path) -> Path:
    p = Path(path_str)
    if p.is_file():
        return p
    cand = (base_dir / path_str).resolve()
    if cand.is_file():
        return cand
    raise FileNotFoundError(f"asset not found: {path_str} (base={base_dir})")


def find_asset_file(assets_dir: Path, stem: str) -> Path | None:
    """Return first existing assets/<stem>.<ext>."""
    for ext in IMAGE_EXTS:
        cand = assets_dir / f"{stem}{ext}"
        if cand.is_file():
            return cand
    return None


def splash_src_from_assets(base_dir: Path) -> Path | None:
    return find_asset_file(base_dir / "assets", "splash")


def splash_params(cfg: dict) -> tuple[str, tuple[int, int, int], int | None]:
    """fit, bg, content_size from pack.json splash{} (missing size → caller default)."""
    raw = cfg.get("splash")
    s = raw if isinstance(raw, dict) else {}
    fit = str(s.get("fit", "contain"))
    if fit not in ("contain", "cover"):
        fit = "contain"
    bg = parse_rgb(s["bg"]) if s.get("bg") else BG
    size = None
    if s.get("content_size") is not None:
        size = clamp_content_size(int(s["content_size"]))
    return fit, bg, size


def set_splash_cfg(cfg: dict, fit: str, bg: tuple[int, int, int], content_size: int) -> None:
    if fit not in ("contain", "cover"):
        fit = "contain"
    cfg["splash"] = {
        "fit": fit,
        "bg": f"#{bg[0]:02x}{bg[1]:02x}{bg[2]:02x}",
        "content_size": int(clamp_content_size(content_size)),
    }


def install_splash_image(src: Path, assets_dir: Path) -> Path:
    """Copy/convert src to assets/splash.png; drop other splash.* so autoload cannot stick to the old file."""
    from PIL import Image

    assets_dir.mkdir(parents=True, exist_ok=True)
    dest = assets_dir / "splash.png"
    src = Path(src).resolve()
    im = Image.open(src).convert("RGBA")
    dest_res = dest.resolve() if dest.exists() else dest
    for ext in IMAGE_EXTS:
        stale = assets_dir / f"splash{ext}"
        if stale.exists() and stale.resolve() != dest_res:
            stale.unlink()
    im.save(dest, "PNG")
    return dest


def clear_splash_assets(assets_dir: Path) -> None:
    for ext in IMAGE_EXTS:
        p = assets_dir / f"splash{ext}"
        if p.is_file():
            p.unlink()


def clip_frame_stem(clip_id: str, frame_i: int, frames: int) -> str:
    return f"{clip_id}_{frame_i}" if frames > 1 else clip_id


def install_clip_frames(
    assets_dir: Path,
    clip_id: str,
    frames: int,
    *,
    src: Path | None = None,
    png: bytes | None = None,
    frame_i: int | None = None,
) -> list[Path]:
    """Write assets/<clip>[_i].png. frame_i=None writes every frame (same pixels)."""
    from PIL import Image
    import io

    assets_dir.mkdir(parents=True, exist_ok=True)
    if src is not None:
        im = Image.open(src).convert("RGBA")
    elif png is not None:
        im = Image.open(io.BytesIO(png)).convert("RGBA")
    else:
        raise ValueError("install_clip_frames needs src or png")

    n = max(1, int(frames))
    indices = range(n) if frame_i is None else [int(frame_i)]
    written: list[Path] = []
    for i in indices:
        stem = clip_frame_stem(clip_id, i, n)
        dest = assets_dir / f"{stem}.png"
        dest_res = dest.resolve() if dest.exists() else dest
        for ext in IMAGE_EXTS:
            stale = assets_dir / f"{stem}{ext}"
            if stale.exists() and stale.resolve() != dest_res:
                stale.unlink()
        im.save(dest, "PNG")
        written.append(dest)
    return written


def set_clip_sources(clip: dict, rels: dict[int, str], apply_all: bool) -> None:
    """rels maps frame index → assets-relative path."""
    n = max(1, int(clip.get("frames", 1)))
    if n <= 1:
        clip["source"] = next(iter(rels.values()))
        clip.pop("sources", None)
        return
    sources = list(clip.get("sources") or [None] * n)
    while len(sources) < n:
        sources.append(None)
    if apply_all:
        for i in range(n):
            sources[i] = rels.get(i, sources[i])
    else:
        for i, rel in rels.items():
            if 0 <= i < n:
                sources[i] = rel
    clip["sources"] = sources
    clip.pop("source", None)


def clip_source_list(clip: dict, frames: int | None = None) -> list[str | None]:
    n = max(1, int(frames if frames is not None else clip.get("frames", 1)))
    sources = clip.get("sources")
    if isinstance(sources, list):
        rows: list[str | None] = [str(x) if x else None for x in sources]
    elif clip.get("source"):
        rows = [str(clip["source"])] * n
    else:
        rows = [None] * n
    while len(rows) < n:
        rows.append(rows[-1] if rows else None)
    return rows[:n]


def write_clip_sources(clip: dict, sources: list[str | None]) -> None:
    n = max(1, int(clip.get("frames", 1)))
    rows = list(sources[:n])
    while len(rows) < n:
        rows.append(None)
    if n <= 1:
        if rows[0]:
            clip["source"] = rows[0]
        else:
            clip.pop("source", None)
        clip.pop("sources", None)
        return
    clip["sources"] = rows
    clip.pop("source", None)


def set_clip_frame_count(clip: dict, count: int) -> None:
    """Grow/shrink clip frames (1..CLIP_FRAMES_MAX). New slots copy the last source."""
    n = max(1, min(CLIP_FRAMES_MAX, int(count)))
    old_n = max(1, int(clip.get("frames", 1)))
    sources = clip_source_list(clip, old_n)
    if n > old_n:
        fill = sources[-1] if sources else None
        sources.extend([fill] * (n - old_n))
    else:
        sources = sources[:n]
    clip["frames"] = n
    write_clip_sources(clip, sources)
    faces = clip.get("faces")
    if isinstance(faces, list):
        while len(faces) < n:
            last = faces[-1] if faces else None
            faces.append(dict(last) if isinstance(last, dict) else None)
        clip["faces"] = faces[:n]
        if n <= 1 and faces:
            clip["face"] = faces[0]
            clip.pop("faces", None)
    elif n > 1 and clip.get("face"):
        clip["faces"] = [dict(clip["face"]) for _ in range(n)]
        clip.pop("face", None)


def delete_clip_frame(clip: dict, frame_i: int) -> int:
    """Remove one frame; returns new current index. No-op if only one frame."""
    n = max(1, int(clip.get("frames", 1)))
    if n <= 1:
        return 0
    i = max(0, min(n - 1, int(frame_i)))
    sources = clip_source_list(clip, n)
    sources.pop(i)
    faces = clip.get("faces")
    if isinstance(faces, list) and i < len(faces):
        faces.pop(i)
        clip["faces"] = faces
    clip["frames"] = n - 1
    write_clip_sources(clip, sources)
    return min(i, n - 2)


def duplicate_clip_frame(clip: dict, frame_i: int) -> int:
    """Insert a copy after frame_i; returns the new frame index."""
    n = max(1, int(clip.get("frames", 1)))
    if n >= CLIP_FRAMES_MAX:
        return min(frame_i, n - 1)
    i = max(0, min(n - 1, int(frame_i)))
    sources = clip_source_list(clip, n)
    sources.insert(i + 1, sources[i])
    faces = clip.get("faces")
    if isinstance(faces, list):
        src_face = faces[i] if i < len(faces) else clip.get("face")
        faces.insert(i + 1, dict(src_face) if isinstance(src_face, dict) else None)
        clip["faces"] = faces
    clip["frames"] = n + 1
    write_clip_sources(clip, sources)
    return i + 1


def clear_clip_assets(assets_dir: Path, clip_id: str, frames: int) -> None:
    n = max(1, int(frames))
    stems = [clip_id] + [f"{clip_id}_{i}" for i in range(n)]
    for stem in stems:
        for ext in IMAGE_EXTS:
            p = assets_dir / f"{stem}{ext}"
            if p.is_file():
                p.unlink()


def _rel_asset(path: Path, base_dir: Path) -> str:
    try:
        return path.resolve().relative_to(base_dir.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def import_asset_folder(src_dir: Path, dest_assets: Path) -> list[str]:
    """Copy known stems from src_dir into dest_assets/. Does not rewrite pack.json."""
    notes: list[str] = []
    if not src_dir.is_dir():
        return [f"import: not a directory: {src_dir}"]
    dest_assets.mkdir(parents=True, exist_ok=True)
    n = 0
    for stem in KNOWN_ASSET_STEMS:
        found = find_asset_file(src_dir, stem)
        if found is None:
            continue
        dest = dest_assets / found.name
        shutil.copy2(found, dest)
        notes.append(f"copied {found.name}")
        n += 1
    if n == 0:
        notes.append(f"import: no known filenames in {src_dir}")
    else:
        notes.insert(0, f"import: {n} files → {dest_assets}")
    return notes


def bind_assets(cfg: dict, base_dir: Path) -> list[str]:
    """Scan assets/ and write clip source/sources in pack.json (in-memory)."""
    assets = base_dir / "assets"
    notes: list[str] = []
    if not assets.is_dir():
        return [f"bind: no assets/ under {base_dir}"]

    for clip in cfg.get("clips", []):
        cid = str(clip.get("id", ""))
        if cid not in CLIP_IDS:
            continue
        n = max(1, int(clip.get("frames", 1)))
        found_rows: list[str | None] = []
        for i in range(n):
            if n == 1:
                hit = find_asset_file(assets, cid)
                if hit is None:
                    hit = find_asset_file(assets, f"{cid}_0")
            else:
                hit = find_asset_file(assets, f"{cid}_{i}")
            found_rows.append(_rel_asset(hit, base_dir) if hit is not None else None)

        bound = sum(1 for x in found_rows if x)
        if bound == 0:
            single = find_asset_file(assets, cid)
            if single is not None:
                clip["source"] = _rel_asset(single, base_dir)
                clip.pop("sources", None)
                notes.append(f"{cid}: {clip['source']} (all {n} frames)")
            else:
                notes.append(f"{cid}: no PNG → synthetic color")
            continue

        if n > 1:
            sources = list(clip.get("sources") or [None] * n)
            while len(sources) < n:
                sources.append(None)
            for i, rel in enumerate(found_rows):
                if rel:
                    sources[i] = rel
            clip["sources"] = sources
            clip.pop("source", None)
            notes.append(f"{cid}: {bound}/{n} frames bound")
        else:
            clip["source"] = found_rows[0]
            clip.pop("sources", None)
            notes.append(f"{cid}: {found_rows[0]}")

    splash = splash_src_from_assets(base_dir)
    if splash is not None:
        notes.append(f"splash: {splash.name} (use --splash or Splash panel)")
    else:
        notes.append("splash: assets/splash.png missing")
    return notes


def check_pack(cfg: dict, cfg_path: Path) -> list[str]:
    """Lines prefixed error: or warn:. Empty = all clips have readable sources."""
    base = Path(cfg_path).resolve().parent
    lines: list[str] = []
    ids = [str(c.get("id")) for c in cfg.get("clips", [])]
    for need in CLIP_IDS:
        if need not in ids:
            lines.append(f"error: missing clip '{need}'")

    for clip in cfg.get("clips", []):
        cid = str(clip.get("id", "?"))
        n = max(1, int(clip.get("frames", 1)))
        sources = clip.get("sources")
        source = clip.get("source")
        if isinstance(sources, list):
            for i in range(n):
                if i >= len(sources) or not sources[i]:
                    lines.append(f"warn: {cid}[{i}]: no source → synthetic")
                    continue
                try:
                    resolve_asset(str(sources[i]), base)
                except FileNotFoundError:
                    lines.append(f"error: {cid}[{i}]: missing {sources[i]}")
        elif source:
            try:
                resolve_asset(str(source), base)
            except FileNotFoundError:
                lines.append(f"error: {cid}: missing {source}")
        else:
            lines.append(f"warn: {cid}: no source → synthetic color")

    if splash_src_from_assets(base) is None:
        lines.append("warn: assets/splash.png missing")
    return lines


def check_has_errors(lines: list[str]) -> bool:
    return any(s.startswith("error:") for s in lines)


def clip_source_label(clip: dict) -> str:
    sources = clip.get("sources")
    if isinstance(sources, list) and any(sources):
        return ",".join(str(s) if s else "-" for s in sources)
    if clip.get("source"):
        return str(clip.get("source"))
    return "(synthetic)"


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
    with_face: bool = False,
    face: dict | None = None,
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
        f = normalize_face(face, body_w)
        _stamp_face_part(rows, canvas, ox + f["eye_l"]["x"], oy + f["eye_l"]["y"], 9, 11, (255, 255, 255), f["eye_l"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["eye_r"]["x"], oy + f["eye_r"]["y"], 9, 11, (255, 255, 255), f["eye_r"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["eye_l"]["x"], oy + f["eye_l"]["y"], 4, 5, (32, 32, 40), f["eye_l"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["eye_r"]["x"], oy + f["eye_r"]["y"], 4, 5, (32, 32, 40), f["eye_r"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["mouth"]["x"], oy + f["mouth"]["y"], 14, 4, (224, 112, 128), f["mouth"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["brow_l"]["x"], oy + f["brow_l"]["y"], 8, 2, (48, 48, 48), f["brow_l"]["angle"])
        _stamp_face_part(rows, canvas, ox + f["brow_r"]["x"], oy + f["brow_r"]["y"], 8, 2, (48, 48, 48), f["brow_r"]["angle"])
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


def _stamp_face_part(
    rows: list[list[tuple[int, int, int]]],
    canvas: int,
    cx: int,
    cy: int,
    rx: int,
    ry: int,
    color: tuple[int, int, int],
    angle_deg: int,
) -> None:
    """Stamp ellipse; angle rotates local axes (clockwise degrees)."""
    rad = math.radians(float(angle_deg))
    cos_a = math.cos(rad)
    sin_a = math.sin(rad)
    pad = max(rx, ry) + 2
    for y in range(max(0, cy - pad), min(canvas, cy + pad + 1)):
        for x in range(max(0, cx - pad), min(canvas, cx + pad + 1)):
            dx = float(x - cx)
            dy = float(y - cy)
            lx = dx * cos_a + dy * sin_a
            ly = -dx * sin_a + dy * cos_a
            if (lx / max(1, rx)) ** 2 + (ly / max(1, ry)) ** 2 <= 1.0:
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


def ensure_card_config(out_pet: Path) -> Path | None:
    """If out is .../pet, write sibling card-root config once (never overwrite)."""
    out_pet = Path(out_pet)
    if out_pet.name != "pet":
        return None
    path = out_pet.parent / CARD_CONFIG_NAME
    if not path.exists():
        path.write_text(CARD_CONFIG_TEXT, encoding="utf-8")
    return path


def export_skin_zip(pet_dir: Path, zip_path: Path | None = None) -> Path:
    """Zip pet/ contents (pack.bin, body/, boot/). Does not include card-root config."""
    pet_dir = Path(pet_dir)
    if zip_path is None:
        zip_path = pet_dir.parent / "pet.zip"
    zip_path = Path(zip_path)
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(pet_dir.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(pet_dir).as_posix()
            if rel == CARD_CONFIG_NAME or rel.startswith("record/"):
                continue
            zf.write(p, rel)
    return zip_path


def build_pack(cfg: dict, out_dir: Path, cfg_path: Path | None = None) -> str:
    """Write pack.bin + body/*.bin. Optional image sources resolved vs cfg_path parent."""
    ensure_card_config(out_dir)
    body_dir = out_dir / "body"
    body_dir.mkdir(parents=True, exist_ok=True)

    w = clamp_body_size(int(cfg["width"]))
    h = clamp_body_size(int(cfg["height"]))
    cfg["width"] = w
    cfg["height"] = h
    # Face anchors require pack version >= 2.
    cfg["version"] = max(int(cfg.get("version", PACK_VERSION_FACE)), PACK_VERSION_FACE)
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
            face = face_for_frame(clip, i, w)
            blob += pack_face_bytes(face)
            pixels = make_clip_frame_pixels(clip, i, w, h, base_dir)
            write_rgbh(out_dir / rel, w, h, pixels)
            if clip.get("source") or clip.get("sources"):
                img_n += 1

    path = out_dir / "pack.bin"
    path.write_bytes(blob)
    note = f", {img_n} image frames" if img_n else ", synthetic colors"
    return (
        f"wrote {path} ({len(blob)} bytes), {len(clips)} clips {w}x{h}"
        f"{note}, pack v{cfg['version']} face anchors"
    )


def build_splash(
    out_dir: Path,
    src: Path | None,
    fit: str = "contain",
    idle_color: tuple[int, int, int] | None = None,
    cfg: dict | None = None,
    bg: tuple[int, int, int] = BG,
    content_size: int | None = None,
) -> str:
    ensure_card_config(out_dir)
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


def build_splash_from_cfg(
    out_dir: Path,
    cfg: dict,
    base_dir: Path,
    idle_color: tuple[int, int, int] | None = None,
) -> str:
    """Write splash.bin from assets/splash.* + pack.json splash{}."""
    src = splash_src_from_assets(base_dir)
    fit, bg, size = splash_params(cfg)
    return build_splash(
        out_dir,
        src,
        fit,
        idle_color if idle_color is not None else idle_color_from_cfg(cfg),
        cfg,
        bg,
        size,
    )


def find_ui_icon_src(base_dir: Path, assets_stem: str) -> Path | None:
    return find_asset_file(Path(base_dir) / "assets", assets_stem)


def default_ui_icon_bg(bin_stem: str) -> tuple[int, int, int]:
    return UI_ICON_CHAT_BG if bin_stem == "chat" else UI_ICON_CARE_BG


def theme_ui_icon_cfg(cfg: dict, bin_stem: str) -> dict:
    """Return {mode, bg} for one UI icon; mode is image|solid."""
    theme = cfg.get("theme") if isinstance(cfg.get("theme"), dict) else {}
    ui = theme.get("ui") if isinstance(theme.get("ui"), dict) else {}
    raw = ui.get(bin_stem) if isinstance(ui.get(bin_stem), dict) else {}
    mode = str(raw.get("mode", "image")).lower()
    if mode not in ("image", "solid"):
        mode = "image"
    bg = parse_rgb(raw["bg"]) if raw.get("bg") else default_ui_icon_bg(bin_stem)
    return {"mode": mode, "bg": bg}


def set_theme_ui_icon_cfg(
    cfg: dict,
    bin_stem: str,
    mode: str,
    bg: tuple[int, int, int],
) -> None:
    if mode not in ("image", "solid"):
        mode = "image"
    theme = cfg.setdefault("theme", {})
    if not isinstance(theme, dict):
        theme = {}
        cfg["theme"] = theme
    ui = theme.setdefault("ui", {})
    if not isinstance(ui, dict):
        ui = {}
        theme["ui"] = ui
    ui[bin_stem] = {
        "mode": mode,
        "bg": f"#{bg[0]:02x}{bg[1]:02x}{bg[2]:02x}",
    }


def make_solid_ui_icon_pixels(
    size: int = UI_ICON_SIZE,
    bg: tuple[int, int, int] = UI_ICON_CARE_BG,
) -> bytes:
    rows = [[bg] * size for _ in range(size)]
    return pixels_from_rgb_rows(rows, size, size)


def load_image_ui_icon(
    src: Path,
    size: int = UI_ICON_SIZE,
    bg: tuple[int, int, int] = UI_ICON_CARE_BG,
    fit: str = "contain",
) -> bytes:
    """Fit icon PNG into size×size; transparent → button bg (RGB565 has no alpha)."""
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow required for UI icons. Install: py -3 -m pip install Pillow"
        ) from exc

    im = Image.open(src).convert("RGBA")
    px = im.load()
    sw, sh = im.size

    def get_rgba(x: int, y: int) -> tuple[int, int, int, int]:
        return px[x, y]

    content = fit_rgba_to_box(sw, sh, get_rgba, size, fit)
    rows = [[(c if c is not None else bg) for c in row] for row in content]
    return pixels_from_rgb_rows(rows, size, size)


def preview_ui_icon_pixels(
    base_dir: Path,
    bin_stem: str,
    assets_stem: str,
    mode: str,
    bg: tuple[int, int, int],
) -> bytes:
    """Pixels for Theme panel preview (solid or image+bg)."""
    if mode == "solid":
        return make_solid_ui_icon_pixels(UI_ICON_SIZE, bg)
    src = find_ui_icon_src(base_dir, assets_stem)
    if src is None:
        return make_solid_ui_icon_pixels(UI_ICON_SIZE, bg)
    return load_image_ui_icon(src, UI_ICON_SIZE, bg)


def build_theme_ui_icons(
    out_dir: Path,
    base_dir: Path,
    cfg: dict | None = None,
) -> str:
    """Write theme/ui/{feed,play,sleep,chat}.bin.

    mode=image: needs assets/ui_*.* ; transparent filled with bg.
    mode=solid: flat bg color (no PNG required).
    Missing image-mode assets are skipped so firmware keeps letter fallback.
    """
    ensure_card_config(out_dir)
    out_dir = Path(out_dir)
    base_dir = Path(base_dir)
    cfg = cfg if isinstance(cfg, dict) else {}
    written: list[str] = []
    missing: list[str] = []
    for bin_stem, assets_stem, _default_bg in UI_ICONS:
        opt = theme_ui_icon_cfg(cfg, bin_stem)
        mode = opt["mode"]
        bg = opt["bg"]
        rel = f"theme/ui/{bin_stem}.bin"
        path = out_dir / rel
        if mode == "solid":
            pixels = make_solid_ui_icon_pixels(UI_ICON_SIZE, bg)
            write_rgbh(path, UI_ICON_SIZE, UI_ICON_SIZE, pixels)
            written.append(f"{rel}=solid#{bg[0]:02x}{bg[1]:02x}{bg[2]:02x}")
            continue
        src = find_ui_icon_src(base_dir, assets_stem)
        if src is None:
            missing.append(assets_stem)
            continue
        pixels = load_image_ui_icon(src, UI_ICON_SIZE, bg)
        write_rgbh(path, UI_ICON_SIZE, UI_ICON_SIZE, pixels)
        written.append(
            f"{rel}←{src.name}+bg#{bg[0]:02x}{bg[1]:02x}{bg[2]:02x}"
        )
    parts = []
    if written:
        parts.append(
            f"wrote {len(written)} UI icons ({UI_ICON_SIZE}x{UI_ICON_SIZE}): "
            + ", ".join(written)
        )
    else:
        parts.append(
            "no UI icons written (set solid color or add assets/ui_feed|ui_play|ui_sleep|ui_chat.*)"
        )
    if missing:
        parts.append("skip missing image: " + ", ".join(missing))
    return "; ".join(parts)
