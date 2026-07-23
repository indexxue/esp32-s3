#!/usr/bin/env python3
"""
Steel-ball SoftAP capture + YOLO annotate tool (PC host).

Connects to ESP32-S3 camera SoftAP, polls /api/camera/camera.jpg,
saves frames, and writes YOLO-normalized labels (cls cx cy w h).

Coordinate: origin top-left, x right, y down — same as board camera_detection.
"""

from __future__ import annotations

import io
import json
import threading
import time
import tkinter as tk
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk
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
POLL_MS = 250
HTTP_TIMEOUT_S = 2.0
CLASS_COUNT = 10

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


def write_labels(stem: str, boxes: List[Box], img_w: int, img_h: int) -> None:
    ensure_dataset_skeleton()
    lines: List[str] = []
    for b in boxes:
        try:
            cls, cx, cy, nw, nh = b.to_yolo(img_w, img_h)
        except ValueError:
            continue
        lines.append(f"{cls} {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
    label_path_for(stem).write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def http_error_hint(status: Optional[int], exc: Optional[BaseException]) -> str:
    if isinstance(exc, requests.exceptions.ConnectionError):
        return "无法连接设备：请确认 PC 已连 SoftAP，且 Base URL 正确"
    if isinstance(exc, requests.exceptions.Timeout):
        return "请求超时：设备忙或网络不通"
    if status == 403:
        return "403 Forbidden：须在 SoftAP/同子网访问摄像头接口"
    if status == 503:
        return "503：相机未就绪 / 已暂停 / 无帧"
    if status is not None:
        return f"HTTP {status}"
    if exc is not None:
        return str(exc)
    return "未知错误"


class AnnotateApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Steel Ball SoftAP Annotate")
        self.root.minsize(720, 560)

        ensure_dataset_skeleton()
        self.class_names = load_class_names()
        cfg = load_config()

        self.base_url = tk.StringVar(value=cfg.get("base_url", DEFAULT_BASE_URL))
        self.status_text = tk.StringVar(value="未连接")
        self.class_var = tk.IntVar(value=0)

        self.connected = False
        self.device_w = 0
        self.device_h = 0
        self.poll_busy = False
        self.preview_on = False
        self._poll_after_id: Optional[str] = None

        # Preview / annotate image state
        self.mode = "preview"  # preview | annotate
        self.raw_jpeg: Optional[bytes] = None
        self.pil_image: Optional[Image.Image] = None  # original size RGB
        self.tk_image: Optional[ImageTk.PhotoImage] = None
        self.display_scale = 1.0
        self.display_offset = (0, 0)  # canvas offset of image top-left
        self.display_size = (0, 0)

        self.current_stem: Optional[str] = None
        self.boxes: List[Box] = []
        self.selected_idx: Optional[int] = None
        self.drag_start: Optional[Tuple[float, float]] = None  # image coords
        self.drag_rect_id: Optional[int] = None

        self._build_ui()
        self._refresh_nav_label()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.bind("<Key>", self._on_key)

    def _build_ui(self) -> None:
        top = ttk.Frame(self.root, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(top, text="Base URL").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.base_url, width=28).pack(side=tk.LEFT, padx=6)
        ttk.Button(top, text="Connect", command=self.connect).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Disconnect", command=self.disconnect).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Capture", command=self.capture).pack(side=tk.LEFT, padx=8)
        ttk.Button(top, text="Save", command=self.save_labels).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Prev", command=lambda: self.nav(-1)).pack(side=tk.LEFT, padx=8)
        ttk.Button(top, text="Next", command=lambda: self.nav(1)).pack(side=tk.LEFT, padx=2)

        mid = ttk.Frame(self.root, padding=(8, 0))
        mid.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(mid, text="Class").pack(side=tk.LEFT)
        class_combo = ttk.Combobox(
            mid,
            width=16,
            state="readonly",
            values=[f"{i}: {self.class_names[i]}" for i in range(CLASS_COUNT)],
        )
        class_combo.current(0)
        class_combo.pack(side=tk.LEFT, padx=6)
        class_combo.bind(
            "<<ComboboxSelected>>",
            lambda _e: self.class_var.set(class_combo.current()),
        )
        self._class_combo = class_combo
        ttk.Label(mid, text="0-9 选类 | Del 删框 | 拖拽画框").pack(side=tk.LEFT, padx=8)

        self.canvas = tk.Canvas(self.root, bg="#1a1a1a", highlightthickness=0)
        self.canvas.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=8, pady=8)
        self.canvas.bind("<ButtonPress-1>", self._on_canvas_press)
        self.canvas.bind("<B1-Motion>", self._on_canvas_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_canvas_release)
        self.canvas.bind("<Configure>", lambda _e: self._redraw())

        bottom = ttk.Frame(self.root, padding=8)
        bottom.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(bottom, textvariable=self.status_text).pack(side=tk.LEFT)

    def _set_status(self, msg: str) -> None:
        self.status_text.set(msg)

    def _refresh_nav_label(self) -> None:
        stems = list_image_stems()
        n = len(stems)
        cur = "?"
        if self.current_stem and self.current_stem in stems:
            cur = str(stems.index(self.current_stem) + 1)
        cls = self.class_var.get()
        res = f"{self.device_w}x{self.device_h}" if self.device_w else "—"
        mode = self.mode
        stem = self.current_stem or "—"
        self._set_status(
            f"[{mode}] {stem}  images={n} ({cur}/{n})  class={cls}:{self.class_names[cls]}  "
            f"device={res}  boxes={len(self.boxes)}"
        )

    def connect(self) -> None:
        url = self.base_url.get().strip().rstrip("/")
        if not url:
            messagebox.showerror("Connect", "Base URL 为空")
            return
        try:
            r = requests.get(f"{url}/api/camera/status", timeout=HTTP_TIMEOUT_S)
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
        self.base_url.set(url)
        save_config(url)
        self.connected = True
        self.device_w = int(data.get("width") or 0)
        self.device_h = int(data.get("height") or 0)
        self.mode = "preview"
        self.current_stem = None
        self.boxes = []
        self.selected_idx = None
        self.preview_on = True
        self._schedule_poll(0)
        self._refresh_nav_label()

    def disconnect(self) -> None:
        self.preview_on = False
        self.connected = False
        if self._poll_after_id is not None:
            try:
                self.root.after_cancel(self._poll_after_id)
            except tk.TclError:
                pass
            self._poll_after_id = None
        self._set_status("已断开")

    def _schedule_poll(self, delay_ms: int) -> None:
        if self._poll_after_id is not None:
            try:
                self.root.after_cancel(self._poll_after_id)
            except tk.TclError:
                pass
            self._poll_after_id = None
        if not self.preview_on or self.mode != "preview":
            return
        self._poll_after_id = self.root.after(delay_ms, self._poll_tick)

    def _poll_tick(self) -> None:
        self._poll_after_id = None
        if not self.preview_on or self.mode != "preview" or self.poll_busy:
            return
        self.poll_busy = True
        url = self.base_url.get().strip().rstrip("/")
        threading.Thread(target=self._fetch_jpeg_worker, args=(url,), daemon=True).start()

    def _fetch_jpeg_worker(self, url: str) -> None:
        err_msg: Optional[str] = None
        jpeg: Optional[bytes] = None
        status: Optional[int] = None
        try:
            r = requests.get(
                f"{url}/api/camera/camera.jpg",
                params={"t": int(time.time() * 1000)},
                timeout=HTTP_TIMEOUT_S,
            )
            status = r.status_code
            if r.status_code == 200 and r.content:
                jpeg = r.content
            else:
                err_msg = http_error_hint(r.status_code, None)
        except requests.RequestException as exc:
            err_msg = http_error_hint(None, exc)

        def apply() -> None:
            self.poll_busy = False
            if not self.preview_on or self.mode != "preview":
                return
            if jpeg is not None:
                self.raw_jpeg = jpeg
                try:
                    self._load_pil_from_jpeg(jpeg)
                    self._redraw()
                except Exception as exc:  # noqa: BLE001
                    self._set_status(f"JPEG 解码失败: {exc}")
            elif err_msg:
                self._set_status(err_msg)
            self._schedule_poll(POLL_MS)

        self.root.after(0, apply)

    def _load_pil_from_jpeg(self, jpeg: bytes) -> None:
        img = Image.open(io.BytesIO(jpeg))
        if img.mode != "RGB":
            img = img.convert("RGB")
        self.pil_image = img
        if self.device_w <= 0:
            self.device_w = img.width
        if self.device_h <= 0:
            self.device_h = img.height

    def capture(self) -> None:
        if self.raw_jpeg is None or self.pil_image is None:
            messagebox.showwarning("Capture", "尚无预览帧，请先 Connect")
            return
        ensure_dataset_skeleton()
        stem = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        path = IMAGES_DIR / f"{stem}.jpg"
        path.write_bytes(self.raw_jpeg)
        self.preview_on = False
        self.mode = "annotate"
        self.current_stem = stem
        self.boxes = []
        self.selected_idx = None
        # Keep frozen frame already in pil_image / raw_jpeg
        self._redraw()
        self._refresh_nav_label()

    def save_labels(self) -> None:
        if self.mode != "annotate" or not self.current_stem or self.pil_image is None:
            messagebox.showwarning("Save", "请先 Capture 或浏览到一张图再保存")
            return
        write_labels(self.current_stem, self.boxes, self.pil_image.width, self.pil_image.height)
        self._refresh_nav_label()
        messagebox.showinfo("Save", f"已写入 {label_path_for(self.current_stem).name}")

    def nav(self, delta: int) -> None:
        stems = list_image_stems()
        if not stems:
            messagebox.showinfo("Nav", "尚无已采图片")
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
        self.preview_on = False
        self.mode = "annotate"
        self.current_stem = stem
        jpeg = path.read_bytes()
        self.raw_jpeg = jpeg
        self._load_pil_from_jpeg(jpeg)
        assert self.pil_image is not None
        self.boxes = read_labels(stem, self.pil_image.width, self.pil_image.height)
        self.selected_idx = None
        self._redraw()
        self._refresh_nav_label()

    def _on_close(self) -> None:
        self.disconnect()
        self.root.destroy()

    def _on_key(self, event: tk.Event) -> None:
        ch = event.char
        if ch and ch.isdigit():
            v = int(ch)
            if 0 <= v < CLASS_COUNT:
                self.class_var.set(v)
                self._class_combo.current(v)
                if self.selected_idx is not None and 0 <= self.selected_idx < len(self.boxes):
                    self.boxes[self.selected_idx].cls = v
                    self._redraw()
                self._refresh_nav_label()
            return
        if event.keysym in ("Delete", "BackSpace"):
            if self.selected_idx is not None and 0 <= self.selected_idx < len(self.boxes):
                del self.boxes[self.selected_idx]
                self.selected_idx = None
                self._redraw()
                self._refresh_nav_label()

    def _canvas_to_image(self, cx: float, cy: float) -> Optional[Tuple[float, float]]:
        if self.pil_image is None or self.display_scale <= 0:
            return None
        ox, oy = self.display_offset
        ix = (cx - ox) / self.display_scale
        iy = (cy - oy) / self.display_scale
        if ix < 0 or iy < 0 or ix > self.pil_image.width or iy > self.pil_image.height:
            return None
        return ix, iy

    def _on_canvas_press(self, event: tk.Event) -> None:
        if self.mode != "annotate" or self.pil_image is None:
            return
        pt = self._canvas_to_image(event.x, event.y)
        if pt is None:
            return
        # Hit-test existing boxes (top-most last)
        hit: Optional[int] = None
        for i in range(len(self.boxes) - 1, -1, -1):
            b = self.boxes[i]
            if b.x <= pt[0] <= b.x + b.w and b.y <= pt[1] <= b.y + b.h:
                hit = i
                break
        if hit is not None:
            self.selected_idx = hit
            self.drag_start = None
            self._redraw()
            self._refresh_nav_label()
            return
        self.selected_idx = None
        self.drag_start = pt
        if self.drag_rect_id is not None:
            self.canvas.delete(self.drag_rect_id)
            self.drag_rect_id = None

    def _on_canvas_drag(self, event: tk.Event) -> None:
        if self.drag_start is None or self.pil_image is None:
            return
        pt = self._canvas_to_image(event.x, event.y)
        if pt is None:
            return
        x0, y0 = self.drag_start
        x1, y1 = pt
        ox, oy = self.display_offset
        s = self.display_scale
        c_coords = (
            ox + min(x0, x1) * s,
            oy + min(y0, y1) * s,
            ox + max(x0, x1) * s,
            oy + max(y0, y1) * s,
        )
        if self.drag_rect_id is None:
            self.drag_rect_id = self.canvas.create_rectangle(
                *c_coords, outline="#3d8bfd", width=2, dash=(4, 2)
            )
        else:
            self.canvas.coords(self.drag_rect_id, *c_coords)

    def _on_canvas_release(self, event: tk.Event) -> None:
        if self.drag_start is None or self.pil_image is None:
            return
        pt = self._canvas_to_image(event.x, event.y)
        x0, y0 = self.drag_start
        self.drag_start = None
        if self.drag_rect_id is not None:
            self.canvas.delete(self.drag_rect_id)
            self.drag_rect_id = None
        if pt is None:
            return
        x1, y1 = pt
        x = min(x0, x1)
        y = min(y0, y1)
        w = abs(x1 - x0)
        h = abs(y1 - y0)
        if w < 2 or h < 2:
            return
        box = Box(self.class_var.get(), x, y, w, h).clamp(
            self.pil_image.width, self.pil_image.height
        )
        if box.w < 2 or box.h < 2:
            return
        self.boxes.append(box)
        self.selected_idx = len(self.boxes) - 1
        self._redraw()
        self._refresh_nav_label()

    def _compute_display_geom(self) -> None:
        if self.pil_image is None:
            self.display_scale = 1.0
            self.display_offset = (0, 0)
            self.display_size = (0, 0)
            return
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        iw, ih = self.pil_image.width, self.pil_image.height
        # Fill canvas while keeping aspect (labels always use original pixels).
        scale = min(cw / iw, ch / ih)
        dw = max(1, int(round(iw * scale)))
        dh = max(1, int(round(ih * scale)))
        ox = (cw - dw) // 2
        oy = (ch - dh) // 2
        self.display_scale = dw / float(iw)
        self.display_offset = (ox, oy)
        self.display_size = (dw, dh)

    def _redraw(self) -> None:
        self.canvas.delete("all")
        if self.pil_image is None:
            return
        self._compute_display_geom()
        dw, dh = self.display_size
        ox, oy = self.display_offset
        disp = self.pil_image.resize((dw, dh), Image.Resampling.NEAREST)
        self.tk_image = ImageTk.PhotoImage(disp)
        self.canvas.create_image(ox, oy, anchor=tk.NW, image=self.tk_image)

        # Crosshair at image center (board tracking reference)
        cx = ox + dw / 2.0
        cy = oy + dh / 2.0
        self.canvas.create_line(cx - 8, cy, cx + 8, cy, fill="#666666")
        self.canvas.create_line(cx, cy - 8, cx, cy + 8, fill="#666666")

        for i, b in enumerate(self.boxes):
            x1 = ox + b.x * self.display_scale
            y1 = oy + b.y * self.display_scale
            x2 = ox + (b.x + b.w) * self.display_scale
            y2 = oy + (b.y + b.h) * self.display_scale
            selected = i == self.selected_idx
            color = "#ffcc00" if selected else "#3d8bfd"
            width = 3 if selected else 2
            self.canvas.create_rectangle(x1, y1, x2, y2, outline=color, width=width)
            label = f"{b.cls}:{self.class_names[b.cls]}"
            self.canvas.create_text(
                x1 + 2,
                y1 + 2,
                anchor=tk.NW,
                text=label,
                fill=color,
                font=("Segoe UI", 9, "bold"),
            )


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
