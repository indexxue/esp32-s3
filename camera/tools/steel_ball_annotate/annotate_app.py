#!/usr/bin/env python3
"""
Steel-ball SoftAP capture + YOLO annotate tool (PC host).

Connects to ESP32-S3 camera SoftAP, takes exclusive MJPEG
(/api/camera/stream.mjpg, LCD off on device), freezes frames for labeling,
writes YOLO-normalized labels (cls cx cy w h).

Coordinate: origin top-left, x right, y down — same as board camera_detection.
"""

from __future__ import annotations

import io
import json
import random
import shutil
import tempfile
import threading
import time
import tkinter as tk
import zipfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import List, Optional, Tuple

try:
    import requests
except ImportError:
    print("Missing dependency: requests. Run: py -3 -m pip install -r requirements.txt")
    raise SystemExit(1) from None

try:
    from PIL import Image, ImageTk
except ImportError:
    print("Missing dependency: Pillow. Run: py -3 -m pip install -r requirements.txt")
    raise SystemExit(1) from None

APP_DIR = Path(__file__).resolve().parent
DATASET_DIR = APP_DIR / "dataset"
IMAGES_DIR = DATASET_DIR / "images"
LABELS_DIR = DATASET_DIR / "labels"
CLASSES_PATH = DATASET_DIR / "classes.txt"
DATA_YAML_PATH = DATASET_DIR / "data.yaml"
CONFIG_PATH = APP_DIR / "config.json"

DEFAULT_BASE_URL = "http://192.168.4.1"
STREAM_PATH = "/api/camera/stream.mjpg"
HTTP_TIMEOUT_S = 2.0
HTTP_CTRL_TIMEOUT_S = 3.0
STREAM_CONNECT_TIMEOUT_S = 3.0
STREAM_READ_TIMEOUT_S = 60.0
# UI 刷新上限；抓帧始终用最新完整 JPEG。
UI_MAX_FPS = 15.0
# 板端原生 240×240；实时预览最多 2× 整数放大。
PREVIEW_NATIVE = 240
PREVIEW_MAX_SCALE = 2
CLASS_COUNT = 10
MIN_BOX_PX = 3
HANDLE_HIT_PX = 8
RESIZE_DEBOUNCE_MS = 80
PACK_1CLS_NAME = "steel_ball_1cls"
PACK_1CLS_CLASS_NAME = "ball"
PACK_1CLS_VAL_RATIO = 0.2
PACK_1CLS_SPLIT_SEED = 42

DEFAULT_CLASSES = [f"cls{i}" for i in range(CLASS_COUNT)]

DEFAULT_DATA_YAML = """\
# Ultralytics-style dataset config (edit path after copy/move if needed).
# Coordinate: origin top-left; labels are YOLO normalized cls cx cy w h.
path: .
train: images
val: images

nc: 10
names:
  0: cls0
  1: cls1
  2: cls2
  3: cls3
  4: cls4
  5: cls5
  6: cls6
  7: cls7
  8: cls8
  9: cls9
"""

PACK_1CLS_DATA_YAML = """\
# Single-class steel-ball pack for cloud Ultralytics train.
# On GPU host: set path to the absolute unzip directory.
path: .
train: images/train
val: images/val

nc: 1
names:
  0: ball
"""

CLASS_COLORS = (
    "#3d8bfd",
    "#34c759",
    "#ff9f0a",
    "#ff375f",
    "#bf5af2",
    "#64d2ff",
    "#ffd60a",
    "#ac8e68",
    "#30d158",
    "#ff6482",
)


@dataclass
class Box:
    """Pixel xywh on original image (origin top-left)."""

    cls: int
    x: float
    y: float
    w: float
    h: float

    def clamp(self, img_w: int, img_h: int) -> "Box":
        x1 = max(0.0, min(float(img_w), self.x))
        y1 = max(0.0, min(float(img_h), self.y))
        x2 = max(0.0, min(float(img_w), self.x + self.w))
        y2 = max(0.0, min(float(img_h), self.y + self.h))
        return Box(self.cls, x1, y1, max(0.0, x2 - x1), max(0.0, y2 - y1))

    def contains(self, ix: float, iy: float) -> bool:
        return self.x <= ix <= self.x + self.w and self.y <= iy <= self.y + self.h

    def to_yolo(self, img_w: int, img_h: int) -> Tuple[int, float, float, float, float]:
        b = self.clamp(img_w, img_h)
        if img_w <= 0 or img_h <= 0 or b.w <= 0 or b.h <= 0:
            raise ValueError("empty box")
        cx = (b.x + b.w / 2.0) / float(img_w)
        cy = (b.y + b.h / 2.0) / float(img_h)
        nw = b.w / float(img_w)
        nh = b.h / float(img_h)
        return b.cls, cx, cy, nw, nh

    @staticmethod
    def from_yolo(
        cls: int, cx: float, cy: float, nw: float, nh: float, img_w: int, img_h: int
    ) -> "Box":
        w = nw * float(img_w)
        h = nh * float(img_h)
        x = cx * float(img_w) - w / 2.0
        y = cy * float(img_h) - h / 2.0
        return Box(cls, x, y, w, h).clamp(img_w, img_h)


def ensure_dataset_skeleton() -> None:
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    LABELS_DIR.mkdir(parents=True, exist_ok=True)
    if not CLASSES_PATH.is_file():
        CLASSES_PATH.write_text("\n".join(DEFAULT_CLASSES) + "\n", encoding="utf-8")
    if not DATA_YAML_PATH.is_file():
        DATA_YAML_PATH.write_text(DEFAULT_DATA_YAML, encoding="utf-8")


def load_class_names() -> List[str]:
    ensure_dataset_skeleton()
    lines = [
        ln.strip()
        for ln in CLASSES_PATH.read_text(encoding="utf-8").splitlines()
        if ln.strip()
    ]
    if len(lines) < CLASS_COUNT:
        lines.extend(DEFAULT_CLASSES[len(lines) :])
    return lines[:CLASS_COUNT]


def load_config() -> dict:
    if not CONFIG_PATH.is_file():
        return {"base_url": DEFAULT_BASE_URL}
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            return {"base_url": DEFAULT_BASE_URL}
        data.setdefault("base_url", DEFAULT_BASE_URL)
        return data
    except (OSError, json.JSONDecodeError):
        return {"base_url": DEFAULT_BASE_URL}


def save_config(base_url: str) -> None:
    CONFIG_PATH.write_text(
        json.dumps({"base_url": base_url.rstrip("/")}, indent=2) + "\n",
        encoding="utf-8",
    )


def list_image_stems() -> List[str]:
    ensure_dataset_skeleton()
    stems = sorted(p.stem for p in IMAGES_DIR.glob("*.jpg"))
    stems += sorted(p.stem for p in IMAGES_DIR.glob("*.jpeg") if p.stem not in stems)
    return sorted(set(stems))


def label_path_for(stem: str) -> Path:
    return LABELS_DIR / f"{stem}.txt"


def image_path_for(stem: str) -> Optional[Path]:
    for ext in (".jpg", ".jpeg", ".JPG", ".JPEG"):
        p = IMAGES_DIR / f"{stem}{ext}"
        if p.is_file():
            return p
    return None


def read_labels(stem: str, img_w: int, img_h: int) -> List[Box]:
    path = label_path_for(stem)
    if not path.is_file():
        return []
    boxes: List[Box] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 5:
            continue
        try:
            cls = int(parts[0])
            cx, cy, nw, nh = map(float, parts[1:])
        except ValueError:
            continue
        if cls < 0 or cls >= CLASS_COUNT:
            continue
        boxes.append(Box.from_yolo(cls, cx, cy, nw, nh, img_w, img_h))
    return boxes


def write_labels(stem: str, boxes: List[Box], img_w: int, img_h: int) -> Path:
    ensure_dataset_skeleton()
    lines: List[str] = []
    for b in boxes:
        try:
            cls, cx, cy, nw, nh = b.to_yolo(img_w, img_h)
        except ValueError:
            continue
        lines.append(f"{cls} {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
    path = label_path_for(stem)
    path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    return path


@dataclass
class Pack1ClsResult:
    zip_path: Path
    train_n: int
    val_n: int
    skipped_no_label: int
    empty_label_n: int
    remapped_non_zero: int


def _split_train_val(
    items: List[str], val_ratio: float = PACK_1CLS_VAL_RATIO, seed: int = PACK_1CLS_SPLIT_SEED
) -> Tuple[List[str], List[str]]:
    """Shuffle with fixed seed; keep at least 1 train when n>=2."""
    ordered = list(items)
    rng = random.Random(seed)
    rng.shuffle(ordered)
    n = len(ordered)
    if n == 0:
        return [], []
    if n == 1:
        return ordered, []
    n_val = max(1, int(round(n * val_ratio)))
    n_val = min(n_val, n - 1)
    return ordered[n_val:], ordered[:n_val]


def _rewrite_label_as_1cls(src: Path, dst: Path) -> Tuple[int, int]:
    """
    Copy YOLO txt, force class_id to 0.
    Returns (box_count, remapped_non_zero_count).
    """
    box_n = 0
    remapped = 0
    out_lines: List[str] = []
    text = src.read_text(encoding="utf-8") if src.is_file() else ""
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 5:
            continue
        try:
            cls = int(parts[0])
            cx, cy, nw, nh = map(float, parts[1:])
        except ValueError:
            continue
        if cls != 0:
            remapped += 1
        out_lines.append(f"0 {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
        box_n += 1
    dst.write_text("\n".join(out_lines) + ("\n" if out_lines else ""), encoding="utf-8")
    return box_n, remapped


def collect_packable_stems() -> Tuple[List[Tuple[str, Path, Path]], int]:
    """
    Return ((stem, image_path, label_path), ...), skipped_no_label.
    Requires a label .txt next to each image (empty txt allowed).
    """
    ensure_dataset_skeleton()
    pairs: List[Tuple[str, Path, Path]] = []
    skipped = 0
    for stem in list_image_stems():
        img = image_path_for(stem)
        if img is None:
            continue
        lbl = label_path_for(stem)
        if not lbl.is_file():
            skipped += 1
            continue
        pairs.append((stem, img, lbl))
    return pairs, skipped


def pack_steel_ball_1cls_zip(
    out_zip: Path,
    val_ratio: float = PACK_1CLS_VAL_RATIO,
    seed: int = PACK_1CLS_SPLIT_SEED,
) -> Pack1ClsResult:
    """
    Build steel_ball_1cls/ layout (train/val split, nc=1) and write a zip.
    Does not modify the working dataset/ tree.
    """
    pairs, skipped = collect_packable_stems()
    if not pairs:
        raise ValueError(
            f"没有可打包样本（有图无标已跳过 {skipped} 张）。请先标注并保存标签。"
        )

    by_stem = {stem: (img, lbl) for stem, img, lbl in pairs}
    train_stems, val_stems = _split_train_val(
        [stem for stem, _, _ in pairs], val_ratio=val_ratio, seed=seed
    )

    remapped_total = 0
    empty_label_n = 0

    with tempfile.TemporaryDirectory(prefix="steel_ball_1cls_") as tmp:
        root = Path(tmp) / PACK_1CLS_NAME
        for split, stems in (("train", train_stems), ("val", val_stems)):
            img_dir = root / "images" / split
            lbl_dir = root / "labels" / split
            img_dir.mkdir(parents=True, exist_ok=True)
            lbl_dir.mkdir(parents=True, exist_ok=True)
            for stem in stems:
                img_src, lbl_src = by_stem[stem]
                shutil.copy2(img_src, img_dir / img_src.name)
                box_n, remapped = _rewrite_label_as_1cls(lbl_src, lbl_dir / f"{stem}.txt")
                remapped_total += remapped
                if box_n == 0:
                    empty_label_n += 1

        (root / "classes.txt").write_text(f"{PACK_1CLS_CLASS_NAME}\n", encoding="utf-8")
        (root / "data.yaml").write_text(PACK_1CLS_DATA_YAML, encoding="utf-8")

        out_zip = out_zip.resolve()
        out_zip.parent.mkdir(parents=True, exist_ok=True)
        if out_zip.is_file():
            out_zip.unlink()

        with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    zf.write(path, arcname=str(path.relative_to(root.parent)).replace("\\", "/"))

    return Pack1ClsResult(
        zip_path=out_zip,
        train_n=len(train_stems),
        val_n=len(val_stems),
        skipped_no_label=skipped,
        empty_label_n=empty_label_n,
        remapped_non_zero=remapped_total,
    )


def http_error_hint(status: Optional[int], exc: Optional[BaseException]) -> str:
    if isinstance(exc, requests.exceptions.ConnectionError):
        return "无法连接设备：请确认 PC 已连 SoftAP，且 Base URL 正确"
    if isinstance(exc, requests.exceptions.Timeout):
        return "请求超时：请关闭浏览器独占预览(/preview.html)，并确认固件为采数模式"
    if status == 403:
        return "403 Forbidden：须在 SoftAP/同子网访问摄像头接口"
    if status == 503:
        return "503：相机忙/已暂停/无帧 — 请关独占预览后 Connect；采数请烧 CAMERA_APP_COLLECT_MODE=1"
    if status is not None:
        return f"HTTP {status}"
    if exc is not None:
        return str(exc)
    return "未知错误"


def decode_jpeg(jpeg: bytes) -> Image.Image:
    img = Image.open(io.BytesIO(jpeg))
    if img.mode != "RGB":
        img = img.convert("RGB")
    return img


def live_disp_size(iw: int, ih: int, cw: int, ch: int) -> Tuple[int, int]:
    """Live preview size: prefer native 240, integer upscale ≤ PREVIEW_MAX_SCALE."""
    if iw <= 0 or ih <= 0:
        return (1, 1)
    fit = min(max(1, cw) / float(iw), max(1, ch) / float(ih))
    if fit < 1.0:
        dw = max(1, int(round(iw * fit)))
        dh = max(1, int(round(ih * fit)))
        return (dw, dh)
    scale = min(PREVIEW_MAX_SCALE, max(1, int(fit)))
    return (iw * scale, ih * scale)


def extract_complete_jpegs(buf: bytearray) -> List[bytes]:
    """Pull SOI..EOI JPEGs from an MJPEG byte buffer; leave a partial frame in buf."""
    out: List[bytes] = []
    while True:
        start = buf.find(b"\xff\xd8")
        if start < 0:
            buf.clear()
            break
        if start > 0:
            del buf[:start]
        end = buf.find(b"\xff\xd9", 2)
        if end < 0:
            if len(buf) > 256 * 1024:
                buf.clear()
            break
        out.append(bytes(buf[: end + 2]))
        del buf[: end + 2]
    return out


class AnnotateApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Steel Ball SoftAP Annotate")
        self.root.minsize(900, 640)

        ensure_dataset_skeleton()
        self.class_names = load_class_names()
        cfg = load_config()

        self.base_url = tk.StringVar(value=cfg.get("base_url", DEFAULT_BASE_URL))
        self.status_text = tk.StringVar(value="未连接")
        self.hint_text = tk.StringVar(
            value="独占 MJPEG 预览 | Space 抓帧 | Esc 回预览 | 请关闭浏览器 /preview.html"
        )
        self.class_var = tk.IntVar(value=0)

        self.connected = False
        self.device_w = PREVIEW_NATIVE
        self.device_h = PREVIEW_NATIVE
        self.preview_on = False
        self._resize_after_id: Optional[str] = None
        self._session = requests.Session()
        self._stream_lock = threading.Lock()
        self._stream_resp: Optional[requests.Response] = None
        self._stream_thread: Optional[threading.Thread] = None
        self._stream_generation = 0
        self._pending_apply = False
        self._latest_frame: Optional[Tuple[int, bytes, Image.Image, Image.Image, Tuple[int, int]]] = None
        self._fps_times: List[float] = []
        self._last_frame_ms = 0.0

        self.mode = "preview"  # preview | annotate
        self.raw_jpeg: Optional[bytes] = None
        self.pil_image: Optional[Image.Image] = None
        self._preview_disp: Optional[Image.Image] = None  # worker-scaled RGB for live view
        self.tk_image: Optional[ImageTk.PhotoImage] = None
        self._cached_disp_size: Tuple[int, int] = (0, 0)
        self._cached_src_key: Optional[Tuple[int, int, int]] = None  # (pil_id, dw, dh)
        self.display_scale = 1.0
        self.display_offset = (0, 0)
        self.display_size = (0, 0)
        self._canvas_img_id: Optional[int] = None
        self._overlay_ids: List[int] = []

        self.current_stem: Optional[str] = None
        self.boxes: List[Box] = []
        self.selected_idx: Optional[int] = None
        self.dirty = False
        self._undo_stack: List[List[Box]] = []
        self._stem_cache: List[str] = []
        self._stem_cache_dirty = True

        # Interaction: none | draw | move | resize
        self._drag_mode: Optional[str] = None
        self._drag_start: Optional[Tuple[float, float]] = None
        self._drag_origin_box: Optional[Box] = None
        self._resize_handle: Optional[str] = None  # nw|ne|sw|se
        self._drag_rect_id: Optional[int] = None
        self._move_undo_pushed = False

        self._build_ui()
        self._refresh_status()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.bind("<Key>", self._on_key)
        self.root.bind("<Control-s>", self._on_ctrl_s)
        self.root.bind("<Control-S>", self._on_ctrl_s)
        self.root.bind("<Control-z>", self._on_ctrl_z)
        self.root.bind("<Control-Z>", self._on_ctrl_z)
        self.root.bind("<Control-Delete>", self._on_ctrl_delete)
        self.root.bind("<Control-BackSpace>", self._on_ctrl_delete)

    def _build_ui(self) -> None:
        top = ttk.Frame(self.root, padding=(10, 8))
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="Base URL").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.base_url, width=26).pack(side=tk.LEFT, padx=6)
        ttk.Button(top, text="Connect", command=self.connect).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Disconnect", command=self.disconnect).pack(side=tk.LEFT, padx=2)

        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(top, text="Capture (Space)", command=self.capture).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Save (Ctrl+S)", command=self.save_labels).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Live (Esc)", command=self.back_to_preview).pack(side=tk.LEFT, padx=2)

        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(top, text="◀ Prev", command=lambda: self.nav(-1)).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Next ▶", command=lambda: self.nav(1)).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Delete image", command=self.delete_current_image).pack(
            side=tk.LEFT, padx=8
        )
        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(top, text="Pack 1cls zip", command=self.pack_1cls_zip).pack(
            side=tk.LEFT, padx=2
        )

        mid = ttk.Frame(self.root, padding=(10, 0))
        mid.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(mid, text="Class").pack(side=tk.LEFT)
        class_combo = ttk.Combobox(
            mid,
            width=18,
            state="readonly",
            values=[f"{i}: {self.class_names[i]}" for i in range(CLASS_COUNT)],
        )
        class_combo.current(0)
        class_combo.pack(side=tk.LEFT, padx=6)
        class_combo.bind("<<ComboboxSelected>>", self._on_class_combo)
        self._class_combo = class_combo
        ttk.Label(mid, textvariable=self.hint_text).pack(side=tk.LEFT, padx=10)

        body = ttk.Frame(self.root)
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=10, pady=6)

        self.canvas = tk.Canvas(body, bg="#121418", highlightthickness=0, cursor="crosshair")
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.canvas.bind("<ButtonPress-1>", self._on_canvas_press)
        self.canvas.bind("<B1-Motion>", self._on_canvas_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_canvas_release)
        self.canvas.bind("<Configure>", self._on_canvas_configure)
        self.canvas.bind("<Motion>", self._on_canvas_motion)

        side = ttk.Frame(body, width=220)
        side.pack(side=tk.RIGHT, fill=tk.Y, padx=(8, 0))
        side.pack_propagate(False)
        ttk.Label(side, text="Boxes").pack(anchor=tk.W)
        self.box_list = tk.Listbox(side, exportselection=False, height=18)
        self.box_list.pack(fill=tk.BOTH, expand=True, pady=(4, 6))
        self.box_list.bind("<<ListboxSelect>>", self._on_box_list_select)
        ttk.Button(side, text="Delete selected box", command=self._delete_selected).pack(fill=tk.X)
        ttk.Button(side, text="Clear all boxes", command=self._clear_boxes).pack(
            fill=tk.X, pady=(4, 0)
        )
        ttk.Button(side, text="Delete this image", command=self.delete_current_image).pack(
            fill=tk.X, pady=(12, 0)
        )

        bottom = ttk.Frame(self.root, padding=(10, 6))
        bottom.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(bottom, textvariable=self.status_text).pack(side=tk.LEFT)

    def _on_class_combo(self, _event: Optional[tk.Event] = None) -> None:
        self.class_var.set(self._class_combo.current())
        if self.selected_idx is not None and 0 <= self.selected_idx < len(self.boxes):
            self._push_undo()
            self.boxes[self.selected_idx].cls = self.class_var.get()
            self.dirty = True
            self.save_labels(quiet=True)
            self._redraw_overlays()
            self._refresh_box_list()
            self._refresh_status()

    def _set_class_ui(self, cls: int) -> None:
        cls = max(0, min(CLASS_COUNT - 1, cls))
        self.class_var.set(cls)
        self._class_combo.current(cls)

    def _get_stems(self) -> List[str]:
        if self._stem_cache_dirty:
            self._stem_cache = list_image_stems()
            self._stem_cache_dirty = False
        return self._stem_cache

    def _invalidate_stems(self) -> None:
        self._stem_cache_dirty = True

    def _refresh_status(self) -> None:
        stems = self._get_stems()
        n = len(stems)
        cur = "?"
        if self.current_stem and self.current_stem in stems:
            cur = str(stems.index(self.current_stem) + 1)
        cls = self.class_var.get()
        res = f"{self.device_w}x{self.device_h}" if self.device_w else "—"
        dirty = "*" if self.dirty else ""
        fps = 0.0
        if len(self._fps_times) >= 2:
            dt = self._fps_times[-1] - self._fps_times[0]
            if dt > 0:
                fps = (len(self._fps_times) - 1) / dt
        live = ""
        if self.mode == "preview" and self.preview_on:
            live = f"  exclusive-MJPEG  fps~{fps:.1f}  frame={self._last_frame_ms:.0f}ms"
        stem = self.current_stem or "—"
        self.status_text.set(
            f"[{self.mode}{dirty}] {stem}  images={n} ({cur}/{n})  "
            f"class={cls}:{self.class_names[cls]}  device={res}  "
            f"boxes={len(self.boxes)}{live}"
        )

    def _refresh_box_list(self) -> None:
        self.box_list.delete(0, tk.END)
        for i, b in enumerate(self.boxes):
            self.box_list.insert(
                tk.END,
                f"#{i} {b.cls}:{self.class_names[b.cls]}  "
                f"{int(b.w)}x{int(b.h)} @({int(b.x)},{int(b.y)})",
            )
        if self.selected_idx is not None and 0 <= self.selected_idx < len(self.boxes):
            self.box_list.selection_clear(0, tk.END)
            self.box_list.selection_set(self.selected_idx)
            self.box_list.see(self.selected_idx)

    def _on_box_list_select(self, _event: tk.Event) -> None:
        sel = self.box_list.curselection()
        if not sel:
            return
        idx = int(sel[0])
        if 0 <= idx < len(self.boxes):
            self.selected_idx = idx
            self._set_class_ui(self.boxes[idx].cls)
            self._redraw_overlays()
            self._refresh_status()

    def _post_camera_ctrl(self, url: str, path: str) -> None:
        try:
            self._session.post(f"{url}{path}", timeout=HTTP_CTRL_TIMEOUT_S)
        except requests.RequestException:
            pass

    def _kick_other_stream(self, url: str, data: dict) -> None:
        """踢掉浏览器独占预览，再 resume，供本工具独占 MJPEG。"""
        exclusive = bool(data.get("exclusive")) or bool(data.get("mjpeg_busy"))
        paused = bool(data.get("paused"))
        if exclusive:
            self._post_camera_ctrl(url, "/api/camera/camera_pause")
            time.sleep(0.4)
            paused = True
        if paused or exclusive:
            self._post_camera_ctrl(url, "/api/camera/camera_resume")
            time.sleep(0.15)

    def _force_native_240(self, url: str) -> None:
        """Ensure board outputs 240×240 JPEG (persist to NVS)."""
        try:
            self._session.post(
                f"{url}/api/camera/config",
                data='{"size":"240x240","quality":55,"persist":1}',
                headers={"Content-Type": "application/json"},
                timeout=HTTP_CTRL_TIMEOUT_S,
            )
        except requests.RequestException:
            pass

    def _stop_exclusive_stream(self) -> None:
        """关闭 MJPEG 连接 → 板端 web_stream_leave，恢复 LCD。"""
        self.preview_on = False
        self._stream_generation += 1
        with self._stream_lock:
            resp = self._stream_resp
            self._stream_resp = None
        if resp is not None:
            try:
                resp.close()
            except Exception:  # noqa: BLE001
                pass

    def _start_exclusive_stream(self) -> None:
        if not self.connected:
            return
        url = self.base_url.get().strip().rstrip("/")
        self._stop_exclusive_stream()
        self._post_camera_ctrl(url, "/api/camera/camera_resume")
        self.preview_on = True
        gen = self._stream_generation
        t = threading.Thread(
            target=self._mjpeg_worker,
            args=(url, gen),
            daemon=True,
            name="mjpeg-exclusive",
        )
        self._stream_thread = t
        t.start()

    def connect(self) -> None:
        url = self.base_url.get().strip().rstrip("/")
        if not url:
            messagebox.showerror("Connect", "Base URL 为空")
            return
        try:
            r = self._session.get(f"{url}/api/camera/status", timeout=HTTP_TIMEOUT_S)
        except requests.RequestException as exc:
            messagebox.showerror("Connect", http_error_hint(None, exc))
            return
        if r.status_code != 200:
            messagebox.showerror("Connect", http_error_hint(r.status_code, None))
            return
        try:
            data = r.json()
        except ValueError:
            messagebox.showerror("Connect", "status 非 JSON")
            return
        if data.get("camera") is not True:
            messagebox.showwarning("Connect", "设备回报 camera=false（未就绪）")

        self._kick_other_stream(url, data)
        self._force_native_240(url)

        if data.get("collect_mode") is False:
            messagebox.showwarning(
                "Connect",
                "当前固件为识别模式（CAMERA_APP_COLLECT_MODE=0）。\n"
                "采数请在 camera/main/CMakeLists.txt 设 CAMERA_APP_COLLECT_MODE=1 后重编烧录。\n"
                "并关闭浏览器 /preview.html。",
            )

        self.base_url.set(url)
        save_config(url)
        self.connected = True
        self.device_w = int(data.get("width") or 0) or PREVIEW_NATIVE
        self.device_h = int(data.get("height") or 0) or PREVIEW_NATIVE
        if not self._maybe_leave_annotate(save_prompt=True):
            return
        self.mode = "preview"
        self.current_stem = None
        self.boxes = []
        self.selected_idx = None
        self.dirty = False
        self._undo_stack.clear()
        self._refresh_box_list()
        self._start_exclusive_stream()
        self._refresh_status()

    def disconnect(self) -> None:
        self._stop_exclusive_stream()
        self.connected = False
        self.status_text.set("已断开（独占流已释放）")

    def back_to_preview(self) -> None:
        if not self.connected:
            messagebox.showinfo("Live", "请先 Connect")
            return
        if not self._maybe_leave_annotate(save_prompt=True):
            return
        self.mode = "preview"
        self.current_stem = None
        self.boxes = []
        self.selected_idx = None
        self.dirty = False
        self._undo_stack.clear()
        self._refresh_box_list()
        self._start_exclusive_stream()
        self._refresh_status()

    def _mjpeg_worker(self, url: str, gen: int) -> None:
        err_msg: Optional[str] = None
        resp: Optional[requests.Response] = None
        try:
            resp = self._session.get(
                f"{url}{STREAM_PATH}",
                params={"t": int(time.time() * 1000)},
                stream=True,
                timeout=(STREAM_CONNECT_TIMEOUT_S, STREAM_READ_TIMEOUT_S),
            )
            if gen != self._stream_generation:
                return
            with self._stream_lock:
                self._stream_resp = resp
            if resp.status_code != 200:
                err_msg = http_error_hint(resp.status_code, None)
                if resp.status_code == 503:
                    err_msg = "503 stream_busy：请关闭浏览器独占预览后再 Connect"
                raise RuntimeError(err_msg)

            buf = bytearray()
            last_ui = 0.0
            t_frame0 = time.perf_counter()
            for chunk in resp.iter_content(chunk_size=8192):
                if gen != self._stream_generation or not self.preview_on:
                    break
                if not chunk:
                    continue
                buf.extend(chunk)
                frames = extract_complete_jpegs(buf)
                if not frames:
                    continue
                jpeg = frames[-1]
                now = time.perf_counter()
                frame_ms = (now - t_frame0) * 1000.0
                t_frame0 = now
                # 始终保留最新完整帧供 Capture；UI 限帧以免 Tk 卡顿。
                if (now - last_ui) < (1.0 / UI_MAX_FPS) and self.raw_jpeg is not None:
                    self.raw_jpeg = jpeg
                    self._last_frame_ms = frame_ms
                    continue
                last_ui = now
                try:
                    pil = decode_jpeg(jpeg)
                except Exception as exc:  # noqa: BLE001
                    err_msg = f"JPEG 解码失败: {exc}"
                    continue
                cw = 480
                ch = 480
                try:
                    cw = max(1, self.canvas.winfo_width())
                    ch = max(1, self.canvas.winfo_height())
                except tk.TclError:
                    pass
                iw, ih = pil.width, pil.height
                dw, dh = live_disp_size(iw, ih, cw, ch)
                disp = pil if (dw == iw and dh == ih) else pil.resize((dw, dh), Image.Resampling.NEAREST)
                self._last_frame_ms = frame_ms
                self._latest_frame = (gen, jpeg, pil, disp, (dw, dh))
                if self._pending_apply:
                    continue
                self._pending_apply = True
                self.root.after(0, self._drain_stream_frame)
        except requests.RequestException as exc:
            err_msg = http_error_hint(None, exc)
        except RuntimeError as exc:
            err_msg = str(exc)
        except Exception as exc:  # noqa: BLE001
            err_msg = f"独占流异常: {exc}"
        finally:
            with self._stream_lock:
                if self._stream_resp is resp:
                    self._stream_resp = None
            if resp is not None:
                try:
                    resp.close()
                except Exception:  # noqa: BLE001
                    pass

        if err_msg and gen == self._stream_generation:

            def show_err(msg: str = err_msg) -> None:
                if gen != self._stream_generation:
                    return
                self.status_text.set(msg)
                self.preview_on = False

            try:
                self.root.after(0, show_err)
            except tk.TclError:
                pass

    def _drain_stream_frame(self) -> None:
        self._pending_apply = False
        frame = self._latest_frame
        self._latest_frame = None
        if frame is None:
            return
        fgen, fjpeg, fpil, fdisp, fsize = frame
        if fgen != self._stream_generation or not self.preview_on or self.mode != "preview":
            return
        self.raw_jpeg = fjpeg
        self.pil_image = fpil
        self._preview_disp = fdisp
        self.device_w = fpil.width
        self.device_h = fpil.height
        now = time.perf_counter()
        self._fps_times.append(now)
        self._fps_times = [t for t in self._fps_times if now - t < 2.0]
        self._apply_preview_frame(fdisp, fsize)
        self._refresh_status()

    def _apply_preview_frame(self, disp: Image.Image, disp_size: Tuple[int, int]) -> None:
        """UI-thread: install worker-scaled frame without re-resize."""
        if self.pil_image is None:
            return
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        iw, ih = self.pil_image.width, self.pil_image.height
        dw, dh = live_disp_size(iw, ih, cw, ch)
        if abs(dw - disp_size[0]) > 2 or abs(dh - disp_size[1]) > 2:
            # Window changed; reuse worker frame only if size still matches intent.
            if dw == iw and dh == ih:
                disp = self.pil_image
            else:
                disp = self.pil_image.resize((dw, dh), Image.Resampling.NEAREST)
            disp_size = (dw, dh)
        ox = (cw - dw) // 2
        oy = (ch - dh) // 2
        self.display_scale = dw / float(iw)
        self.display_offset = (ox, oy)
        self.display_size = (dw, dh)
        self.tk_image = ImageTk.PhotoImage(disp)
        self._cached_disp_size = (dw, dh)
        self._cached_src_key = (id(self.pil_image), dw, dh)
        if self._canvas_img_id is None:
            self.canvas.delete("all")
            self._overlay_ids.clear()
            self._canvas_img_id = self.canvas.create_image(
                ox, oy, anchor=tk.NW, image=self.tk_image, tags=("frame",)
            )
            self._draw_crosshair()
        else:
            self.canvas.itemconfigure(self._canvas_img_id, image=self.tk_image)
            self.canvas.coords(self._canvas_img_id, ox, oy)

    def _compute_display_geom(self) -> None:
        if self.pil_image is None:
            self.display_scale = 1.0
            self.display_offset = (0, 0)
            self.display_size = (0, 0)
            return
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        iw, ih = self.pil_image.width, self.pil_image.height
        scale = min(cw / iw, ch / ih)
        dw = max(1, int(round(iw * scale)))
        dh = max(1, int(round(ih * scale)))
        ox = (cw - dw) // 2
        oy = (ch - dh) // 2
        self.display_scale = dw / float(iw)
        self.display_offset = (ox, oy)
        self.display_size = (dw, dh)

    def _ensure_tk_image(self, force: bool = False) -> bool:
        """Build/cache PhotoImage for current pil_image + display size. Return True if ready."""
        if self.pil_image is None:
            return False
        self._compute_display_geom()
        dw, dh = self.display_size
        if dw < 2 or dh < 2:
            return False
        key = (id(self.pil_image), dw, dh)
        if not force and self.tk_image is not None and self._cached_src_key == key:
            return True
        resample = (
            Image.Resampling.NEAREST
            if max(dw / self.pil_image.width, 1.0) >= 1.5
            else Image.Resampling.BILINEAR
        )
        disp = self.pil_image.resize((dw, dh), resample)
        self.tk_image = ImageTk.PhotoImage(disp)
        self._cached_disp_size = (dw, dh)
        self._cached_src_key = key
        return True

    def _update_preview_image(self, force_geom: bool = False) -> None:
        if not self._ensure_tk_image(force=force_geom):
            return
        ox, oy = self.display_offset
        if self._canvas_img_id is None:
            self.canvas.delete("all")
            self._overlay_ids.clear()
            self._canvas_img_id = self.canvas.create_image(
                ox, oy, anchor=tk.NW, image=self.tk_image, tags=("frame",)
            )
            self._draw_crosshair()
        else:
            self.canvas.itemconfigure(self._canvas_img_id, image=self.tk_image)
            self.canvas.coords(self._canvas_img_id, ox, oy)
            # Refresh crosshair positions if geometry changed.
            if force_geom:
                self.canvas.delete("crosshair")
                self._draw_crosshair()

    def _draw_crosshair(self) -> None:
        dw, dh = self.display_size
        ox, oy = self.display_offset
        cx = ox + dw / 2.0
        cy = oy + dh / 2.0
        self.canvas.create_line(cx - 10, cy, cx + 10, cy, fill="#555555", tags=("crosshair",))
        self.canvas.create_line(cx, cy - 10, cx, cy + 10, fill="#555555", tags=("crosshair",))

    def _clear_canvas_frame(self) -> None:
        self.canvas.delete("all")
        self._canvas_img_id = None
        self._overlay_ids.clear()
        self._drag_rect_id = None

    def _redraw_full(self) -> None:
        self._clear_canvas_frame()
        if self.pil_image is None:
            return
        if not self._ensure_tk_image(force=True):
            return
        ox, oy = self.display_offset
        self._canvas_img_id = self.canvas.create_image(
            ox, oy, anchor=tk.NW, image=self.tk_image, tags=("frame",)
        )
        self._draw_crosshair()
        self._redraw_overlays()

    def _redraw_overlays(self) -> None:
        self.canvas.delete("box")
        self.canvas.delete("handle")
        self._overlay_ids.clear()
        if self.mode != "annotate" or self.pil_image is None:
            return
        ox, oy = self.display_offset
        s = self.display_scale
        for i, b in enumerate(self.boxes):
            x1 = ox + b.x * s
            y1 = oy + b.y * s
            x2 = ox + (b.x + b.w) * s
            y2 = oy + (b.y + b.h) * s
            selected = i == self.selected_idx
            color = "#ffcc00" if selected else CLASS_COLORS[b.cls % len(CLASS_COLORS)]
            width = 3 if selected else 2
            rid = self.canvas.create_rectangle(
                x1, y1, x2, y2, outline=color, width=width, tags=("box", f"box-{i}")
            )
            tid = self.canvas.create_text(
                x1 + 3,
                y1 + 3,
                anchor=tk.NW,
                text=f"{b.cls}:{self.class_names[b.cls]}",
                fill=color,
                font=("Segoe UI", 9, "bold"),
                tags=("box", f"box-{i}"),
            )
            self._overlay_ids.extend([rid, tid])
            if selected:
                self._draw_handles(x1, y1, x2, y2)

    def _draw_handles(self, x1: float, y1: float, x2: float, y2: float) -> None:
        r = 4
        for name, hx, hy in (
            ("nw", x1, y1),
            ("ne", x2, y1),
            ("sw", x1, y2),
            ("se", x2, y2),
        ):
            self.canvas.create_rectangle(
                hx - r,
                hy - r,
                hx + r,
                hy + r,
                outline="#ffcc00",
                fill="#1a1a1a",
                width=1,
                tags=("handle", f"handle-{name}"),
            )

    def _on_canvas_configure(self, _event: tk.Event) -> None:
        if self._resize_after_id is not None:
            try:
                self.root.after_cancel(self._resize_after_id)
            except tk.TclError:
                pass
        self._resize_after_id = self.root.after(RESIZE_DEBOUNCE_MS, self._on_resize_debounced)

    def _on_resize_debounced(self) -> None:
        self._resize_after_id = None
        if self.pil_image is None:
            return
        if self.mode == "preview":
            self._update_preview_image(force_geom=True)
        else:
            self._redraw_full()

    def capture(self) -> None:
        if self.raw_jpeg is None or self.pil_image is None:
            messagebox.showwarning("Capture", "尚无预览帧，请先 Connect")
            return
        if self.mode == "annotate" and not self._maybe_leave_annotate(save_prompt=True):
            return
        ensure_dataset_skeleton()
        stem = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        path = IMAGES_DIR / f"{stem}.jpg"
        path.write_bytes(self.raw_jpeg)
        self._invalidate_stems()
        self._stop_exclusive_stream()
        self.mode = "annotate"
        self.current_stem = stem
        self.boxes = []
        self.selected_idx = None
        self.dirty = False
        self._undo_stack.clear()
        self._redraw_full()
        self._refresh_box_list()
        self._refresh_status()

    def save_labels(self, quiet: bool = False) -> bool:
        if self.mode != "annotate" or not self.current_stem or self.pil_image is None:
            if not quiet:
                messagebox.showwarning("Save", "请先 Capture 或浏览到一张图再保存")
            return False
        path = write_labels(
            self.current_stem, self.boxes, self.pil_image.width, self.pil_image.height
        )
        self.dirty = False
        self._refresh_status()
        if not quiet:
            self.status_text.set(f"已保存 {path.name}  boxes={len(self.boxes)}")
        return True

    def _maybe_leave_annotate(self, save_prompt: bool) -> bool:
        if self.mode != "annotate" or not self.dirty:
            return True
        if not save_prompt:
            return True
        ans = messagebox.askyesnocancel(
            "未保存",
            f"{self.current_stem} 有未保存标注，是否保存？",
        )
        if ans is None:
            return False
        if ans:
            return self.save_labels(quiet=True)
        self.dirty = False
        return True

    def nav(self, delta: int) -> None:
        stems = self._get_stems()
        if not stems:
            self._invalidate_stems()
            stems = self._get_stems()
        if not stems:
            messagebox.showinfo("Nav", "尚无已采图片")
            return
        if not self._maybe_leave_annotate(save_prompt=True):
            return
        if self.current_stem in stems:
            idx = stems.index(self.current_stem)
        else:
            idx = 0 if delta > 0 else len(stems) - 1
            delta = 0
        idx = (idx + delta) % len(stems)
        self._open_stem(stems[idx])

    def _open_stem(self, stem: str) -> None:
        path = image_path_for(stem)
        if path is None:
            return
        self._stop_exclusive_stream()
        self.mode = "annotate"
        self.current_stem = stem
        jpeg = path.read_bytes()
        self.raw_jpeg = jpeg
        self.pil_image = decode_jpeg(jpeg)
        self.device_w = self.pil_image.width
        self.device_h = self.pil_image.height
        self.boxes = read_labels(stem, self.pil_image.width, self.pil_image.height)
        self.selected_idx = None
        self.dirty = False
        self._undo_stack.clear()
        self._redraw_full()
        self._refresh_box_list()
        self._refresh_status()

    def delete_current_image(self) -> None:
        """Delete current image + label files from dataset (annotate mode only)."""
        if self.mode != "annotate" or not self.current_stem:
            messagebox.showinfo("Delete image", "请先 Capture 或浏览到一张已采图片")
            return
        stem = self.current_stem
        img_path = image_path_for(stem)
        lab_path = label_path_for(stem)
        if img_path is None and not lab_path.is_file():
            messagebox.showwarning("Delete image", f"未找到文件: {stem}")
            return
        n_boxes = len(self.boxes)
        if not messagebox.askyesno(
            "Delete image",
            f"永久删除当前图片及其标签？\n\n"
            f"  {img_path.name if img_path else stem}\n"
            f"  labels: {lab_path.name}（框数 {n_boxes}）\n\n"
            f"此操作不可撤销。",
        ):
            return

        stems = self._get_stems()
        try:
            idx = stems.index(stem)
        except ValueError:
            idx = -1
        next_stem: Optional[str] = None
        if idx >= 0 and len(stems) > 1:
            # Prefer next; if deleting last, go previous.
            if idx + 1 < len(stems):
                next_stem = stems[idx + 1]
            else:
                next_stem = stems[idx - 1]

        removed: List[str] = []
        try:
            if img_path is not None and img_path.is_file():
                img_path.unlink()
                removed.append(img_path.name)
            if lab_path.is_file():
                lab_path.unlink()
                removed.append(lab_path.name)
        except OSError as exc:
            messagebox.showerror("Delete image", f"删除失败: {exc}")
            self._invalidate_stems()
            return

        self._invalidate_stems()
        self.dirty = False
        self.boxes = []
        self.selected_idx = None
        self._undo_stack.clear()
        self.current_stem = None

        if next_stem is not None and image_path_for(next_stem) is not None:
            self._open_stem(next_stem)
            self.status_text.set(f"已删除 {', '.join(removed)} → {next_stem}")
            return

        # No remaining images: clear canvas; resume live if connected.
        self.raw_jpeg = None
        self.pil_image = None
        self.tk_image = None
        self._clear_canvas_frame()
        self._refresh_box_list()
        if self.connected:
            self.mode = "preview"
            self._start_exclusive_stream()
            self.status_text.set(f"已删除 {', '.join(removed)}；已回实时预览")
        else:
            self.mode = "preview"
            self.preview_on = False
            self.status_text.set(f"已删除 {', '.join(removed)}；数据集已空")
        self._refresh_status()

    def pack_1cls_zip(self) -> None:
        """Export single-class train/val zip for cloud Ultralytics training."""
        if self.mode == "annotate" and self.dirty:
            if not self.save_labels(quiet=True):
                messagebox.showerror("Pack 1cls zip", "当前标签保存失败，已取消打包")
                return

        try:
            pairs, skipped = collect_packable_stems()
        except OSError as exc:
            messagebox.showerror("Pack 1cls zip", f"扫描数据集失败: {exc}")
            return

        if not pairs:
            messagebox.showwarning(
                "Pack 1cls zip",
                f"没有可打包样本。\n有图无标已跳过 {skipped} 张。\n请先标注并保存标签。",
            )
            return

        train_stems, val_stems = _split_train_val([stem for stem, _, _ in pairs])
        preview = (
            f"将整理为单类 ball（class_id=0）并打包：\n\n"
            f"  可用样本: {len(pairs)}\n"
            f"  train / val: {len(train_stems)} / {len(val_stems)}"
            f"  （约 {int(PACK_1CLS_VAL_RATIO * 100)}% val）\n"
            f"  跳过有图无标: {skipped}\n\n"
            f"不会改动 dataset/ 原文件；非 0 类别会在 zip 内改写为 0。"
        )
        if not messagebox.askyesno("Pack 1cls zip", preview):
            return

        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        default_name = f"{PACK_1CLS_NAME}_{stamp}.zip"
        out = filedialog.asksaveasfilename(
            title="保存单类训练包",
            initialdir=str(APP_DIR),
            initialfile=default_name,
            defaultextension=".zip",
            filetypes=[("ZIP archive", "*.zip"), ("All files", "*.*")],
        )
        if not out:
            return

        try:
            result = pack_steel_ball_1cls_zip(Path(out))
        except ValueError as exc:
            messagebox.showwarning("Pack 1cls zip", str(exc))
            return
        except OSError as exc:
            messagebox.showerror("Pack 1cls zip", f"打包失败: {exc}")
            return

        extra = ""
        if result.remapped_non_zero:
            extra += f"\n非 0 类别已改写为 0: {result.remapped_non_zero} 框"
        if result.empty_label_n:
            extra += f"\n空标签（负样本）: {result.empty_label_n} 张"
        if result.skipped_no_label:
            extra += f"\n跳过有图无标: {result.skipped_no_label} 张"

        self.status_text.set(
            f"已打包 {result.train_n}+{result.val_n} → {result.zip_path.name}"
        )
        messagebox.showinfo(
            "Pack 1cls zip",
            f"打包完成。\n\n"
            f"{result.zip_path}\n\n"
            f"train={result.train_n}  val={result.val_n}"
            f"{extra}\n\n"
            f"上传云端后请把 data.yaml 的 path 改成解压绝对路径。",
        )

    def _on_close(self) -> None:
        if not self._maybe_leave_annotate(save_prompt=True):
            return
        self.disconnect()
        try:
            self._session.close()
        except Exception:  # noqa: BLE001
            pass
        self.root.destroy()

    def _on_ctrl_s(self, _event: tk.Event) -> str:
        self.save_labels()
        return "break"

    def _on_ctrl_z(self, _event: tk.Event) -> str:
        self._undo()
        return "break"

    def _on_ctrl_delete(self, _event: tk.Event) -> str:
        self.delete_current_image()
        return "break"

    def _push_undo(self) -> None:
        snap = [Box(b.cls, b.x, b.y, b.w, b.h) for b in self.boxes]
        self._undo_stack.append(snap)
        if len(self._undo_stack) > 40:
            self._undo_stack = self._undo_stack[-40:]

    def _undo(self) -> None:
        if self.mode != "annotate" or not self._undo_stack:
            return
        self.boxes = self._undo_stack.pop()
        self.selected_idx = None
        self.dirty = True
        self._redraw_overlays()
        self._refresh_box_list()
        self._refresh_status()

    def _delete_selected(self) -> None:
        if self.mode != "annotate":
            return
        if self.selected_idx is None or not (0 <= self.selected_idx < len(self.boxes)):
            return
        self._push_undo()
        del self.boxes[self.selected_idx]
        self.selected_idx = None
        self.dirty = True
        self.save_labels(quiet=True)
        self._redraw_overlays()
        self._refresh_box_list()
        self._refresh_status()

    def _clear_boxes(self) -> None:
        if self.mode != "annotate" or not self.boxes:
            return
        if not messagebox.askyesno("Clear", "清除当前图全部框？"):
            return
        self._push_undo()
        self.boxes = []
        self.selected_idx = None
        self.dirty = True
        self._redraw_overlays()
        self._refresh_box_list()
        self._refresh_status()

    def _on_key(self, event: tk.Event) -> None:
        # Ignore typing into Entry
        w = event.widget
        if isinstance(w, (ttk.Entry, tk.Entry)):
            return
        if event.keysym in ("space", "Space"):
            if self.mode == "preview":
                self.capture()
            return
        if event.keysym == "Escape":
            if self.mode == "annotate":
                self.back_to_preview()
            return
        if event.keysym in ("Left", "a"):
            self.nav(-1)
            return
        if event.keysym in ("Right", "d"):
            self.nav(1)
            return
        ch = event.char
        if ch and ch.isdigit():
            v = int(ch)
            if 0 <= v < CLASS_COUNT:
                self._set_class_ui(v)
                if self.selected_idx is not None and 0 <= self.selected_idx < len(self.boxes):
                    self._push_undo()
                    self.boxes[self.selected_idx].cls = v
                    self.dirty = True
                    self.save_labels(quiet=True)
                    self._redraw_overlays()
                    self._refresh_box_list()
                self._refresh_status()
            return
        # Plain Del/BackSpace: delete selected box (Ctrl+Del deletes whole image).
        if event.keysym in ("Delete", "BackSpace") and not (
            event.state & 0x4
        ):  # 0x4 = Control on Windows/X11
            self._delete_selected()

    def _canvas_to_image(self, cx: float, cy: float, clamp: bool = False) -> Optional[Tuple[float, float]]:
        if self.pil_image is None or self.display_scale <= 0:
            return None
        ox, oy = self.display_offset
        ix = (cx - ox) / self.display_scale
        iy = (cy - oy) / self.display_scale
        if clamp:
            ix = max(0.0, min(float(self.pil_image.width), ix))
            iy = max(0.0, min(float(self.pil_image.height), iy))
            return ix, iy
        if ix < 0 or iy < 0 or ix > self.pil_image.width or iy > self.pil_image.height:
            return None
        return ix, iy

    def _hit_handle(self, cx: float, cy: float) -> Optional[str]:
        if self.selected_idx is None or not (0 <= self.selected_idx < len(self.boxes)):
            return None
        b = self.boxes[self.selected_idx]
        ox, oy = self.display_offset
        s = self.display_scale
        corners = {
            "nw": (ox + b.x * s, oy + b.y * s),
            "ne": (ox + (b.x + b.w) * s, oy + b.y * s),
            "sw": (ox + b.x * s, oy + (b.y + b.h) * s),
            "se": (ox + (b.x + b.w) * s, oy + (b.y + b.h) * s),
        }
        for name, (hx, hy) in corners.items():
            if abs(cx - hx) <= HANDLE_HIT_PX and abs(cy - hy) <= HANDLE_HIT_PX:
                return name
        return None

    def _on_canvas_motion(self, event: tk.Event) -> None:
        if self.mode != "annotate":
            self.canvas.configure(cursor="arrow")
            return
        handle = self._hit_handle(event.x, event.y)
        if handle in ("nw", "se"):
            self.canvas.configure(cursor="size_nw_se")
        elif handle in ("ne", "sw"):
            self.canvas.configure(cursor="size_ne_sw")
        else:
            pt = self._canvas_to_image(event.x, event.y)
            if pt is not None and any(b.contains(pt[0], pt[1]) for b in self.boxes):
                self.canvas.configure(cursor="fleur")
            else:
                self.canvas.configure(cursor="crosshair")

    def _on_canvas_press(self, event: tk.Event) -> None:
        if self.mode != "annotate" or self.pil_image is None:
            return
        handle = self._hit_handle(event.x, event.y)
        if handle is not None and self.selected_idx is not None:
            self._push_undo()
            self._drag_mode = "resize"
            self._resize_handle = handle
            self._drag_start = self._canvas_to_image(event.x, event.y, clamp=True)
            self._drag_origin_box = Box(
                self.boxes[self.selected_idx].cls,
                self.boxes[self.selected_idx].x,
                self.boxes[self.selected_idx].y,
                self.boxes[self.selected_idx].w,
                self.boxes[self.selected_idx].h,
            )
            return

        pt = self._canvas_to_image(event.x, event.y)
        if pt is None:
            self.selected_idx = None
            self._redraw_overlays()
            self._refresh_box_list()
            return

        hit: Optional[int] = None
        for i in range(len(self.boxes) - 1, -1, -1):
            if self.boxes[i].contains(pt[0], pt[1]):
                hit = i
                break
        if hit is not None:
            self.selected_idx = hit
            self._set_class_ui(self.boxes[hit].cls)
            self._drag_mode = "move"
            self._drag_start = pt
            self._drag_origin_box = Box(
                self.boxes[hit].cls,
                self.boxes[hit].x,
                self.boxes[hit].y,
                self.boxes[hit].w,
                self.boxes[hit].h,
            )
            self._move_undo_pushed = False
            self._redraw_overlays()
            self._refresh_box_list()
            self._refresh_status()
            return

        self.selected_idx = None
        self._drag_mode = "draw"
        self._drag_start = pt
        self._drag_origin_box = None
        self._resize_handle = None
        if self._drag_rect_id is not None:
            self.canvas.delete(self._drag_rect_id)
            self._drag_rect_id = None
        self._redraw_overlays()
        self._refresh_box_list()

    def _on_canvas_drag(self, event: tk.Event) -> None:
        if self.mode != "annotate" or self.pil_image is None or self._drag_mode is None:
            return
        if self._drag_mode == "draw":
            if self._drag_start is None:
                return
            pt = self._canvas_to_image(event.x, event.y, clamp=True)
            if pt is None:
                return
            x0, y0 = self._drag_start
            x1, y1 = pt
            ox, oy = self.display_offset
            s = self.display_scale
            c_coords = (
                ox + min(x0, x1) * s,
                oy + min(y0, y1) * s,
                ox + max(x0, x1) * s,
                oy + max(y0, y1) * s,
            )
            color = CLASS_COLORS[self.class_var.get() % len(CLASS_COLORS)]
            if self._drag_rect_id is None:
                self._drag_rect_id = self.canvas.create_rectangle(
                    *c_coords, outline=color, width=2, dash=(4, 2), tags=("drag",)
                )
            else:
                self.canvas.coords(self._drag_rect_id, *c_coords)
            return

        if self._drag_start is None or self._drag_origin_box is None or self.selected_idx is None:
            return
        pt = self._canvas_to_image(event.x, event.y, clamp=True)
        if pt is None:
            return
        ob = self._drag_origin_box
        iw, ih = self.pil_image.width, self.pil_image.height

        if self._drag_mode == "move":
            dx = pt[0] - self._drag_start[0]
            dy = pt[1] - self._drag_start[1]
            if abs(dx) < 0.5 and abs(dy) < 0.5:
                return
            if not self._move_undo_pushed:
                self._push_undo()
                self._move_undo_pushed = True
            nx = max(0.0, min(float(iw) - ob.w, ob.x + dx))
            ny = max(0.0, min(float(ih) - ob.h, ob.y + dy))
            self.boxes[self.selected_idx] = Box(ob.cls, nx, ny, ob.w, ob.h)
            self.dirty = True
            self._redraw_overlays()
            return

        if self._drag_mode == "resize" and self._resize_handle:
            x1, y1 = ob.x, ob.y
            x2, y2 = ob.x + ob.w, ob.y + ob.h
            ix, iy = pt
            h = self._resize_handle
            if "n" in h:
                y1 = min(iy, y2 - MIN_BOX_PX)
            if "s" in h:
                y2 = max(iy, y1 + MIN_BOX_PX)
            if "w" in h:
                x1 = min(ix, x2 - MIN_BOX_PX)
            if "e" in h:
                x2 = max(ix, x1 + MIN_BOX_PX)
            box = Box(ob.cls, x1, y1, x2 - x1, y2 - y1).clamp(iw, ih)
            self.boxes[self.selected_idx] = box
            self.dirty = True
            self._redraw_overlays()

    def _on_canvas_release(self, event: tk.Event) -> None:
        if self._drag_mode == "draw":
            if self._drag_start is None or self.pil_image is None:
                self._drag_mode = None
                return
            pt = self._canvas_to_image(event.x, event.y, clamp=True)
            x0, y0 = self._drag_start
            self._drag_start = None
            self._drag_mode = None
            if self._drag_rect_id is not None:
                self.canvas.delete(self._drag_rect_id)
                self._drag_rect_id = None
            if pt is None:
                return
            x1, y1 = pt
            x = min(x0, x1)
            y = min(y0, y1)
            w = abs(x1 - x0)
            h = abs(y1 - y0)
            if w < MIN_BOX_PX or h < MIN_BOX_PX:
                return
            self._push_undo()
            box = Box(self.class_var.get(), x, y, w, h).clamp(
                self.pil_image.width, self.pil_image.height
            )
            if box.w < MIN_BOX_PX or box.h < MIN_BOX_PX:
                return
            self.boxes.append(box)
            self.selected_idx = len(self.boxes) - 1
            self.dirty = True
            # New capture often wants save soon; auto-save keeps dataset consistent.
            self.save_labels(quiet=True)
            self._redraw_overlays()
            self._refresh_box_list()
            self._refresh_status()
            return

        if self._drag_mode in ("move", "resize"):
            if self.dirty:
                self.save_labels(quiet=True)
            self._drag_mode = None
            self._drag_start = None
            self._drag_origin_box = None
            self._resize_handle = None
            self._refresh_box_list()
            self._refresh_status()
            return

        self._drag_mode = None


def main() -> None:
    ensure_dataset_skeleton()
    root = tk.Tk()
    try:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass
    AnnotateApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
