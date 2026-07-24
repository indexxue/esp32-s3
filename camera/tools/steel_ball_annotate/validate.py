#!/usr/bin/env python3
"""
Steel-ball YOLO detection validator (PC host GUI).

Pick a .pt weight and images, run Ultralytics predict, draw boxes + centers.
Coordinate: origin top-left, x right, y down — same as board / annotate_app.
"""

from __future__ import annotations

import argparse
import json
import threading
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import List, Optional, Tuple

try:
    from PIL import Image, ImageDraw, ImageFont, ImageTk
except ImportError:
    print("Missing dependency: Pillow. Run: py -3 -m pip install -r requirements.txt")
    raise SystemExit(1) from None

APP_DIR = Path(__file__).resolve().parent
DATASET_IMAGES_DIR = APP_DIR / "dataset" / "images"
CONFIG_PATH = APP_DIR / "validate_config.json"
RESULT_DIR = APP_DIR / "validate_results"

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
MODEL_EXTS = {".pt", ".onnx"}

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

RESIZE_DEBOUNCE_MS = 80
DEFAULT_CONF = 0.25
DEFAULT_IOU = 0.45


@dataclass
class Detection:
    cls: int
    name: str
    conf: float
    x1: float
    y1: float
    x2: float
    y2: float
    cx: float
    cy: float
    cx_norm: float
    cy_norm: float


def _hex_to_rgb(color: str) -> Tuple[int, int, int]:
    c = color.lstrip("#")
    return int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16)


def list_images_in(folder: Path) -> List[Path]:
    if not folder.is_dir():
        return []
    files = [
        p
        for p in folder.iterdir()
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS
    ]
    return sorted(files, key=lambda p: p.name.lower())


def find_default_model() -> Optional[Path]:
    candidates = [
        APP_DIR / "best.pt",
        APP_DIR / "last.pt",
        APP_DIR / "yolov8n.pt",
    ]
    for p in candidates:
        if p.is_file():
            return p
    pts = sorted(APP_DIR.glob("*.pt"))
    return pts[0] if pts else None


def load_config() -> dict:
    defaults = {
        "model_path": "",
        "image_dir": str(DATASET_IMAGES_DIR),
        "conf": DEFAULT_CONF,
        "iou": DEFAULT_IOU,
    }
    if not CONFIG_PATH.is_file():
        return defaults
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            return defaults
        for k, v in defaults.items():
            data.setdefault(k, v)
        return data
    except (OSError, json.JSONDecodeError):
        return defaults


def save_config(cfg: dict) -> None:
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")


def draw_detections(
    image: Image.Image, dets: List[Detection], show_labels: bool = True
) -> Image.Image:
    out = image.convert("RGB").copy()
    draw = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype("arial.ttf", 14)
    except OSError:
        font = ImageFont.load_default()

    for det in dets:
        color = CLASS_COLORS[det.cls % len(CLASS_COLORS)]
        rgb = _hex_to_rgb(color)
        x1, y1, x2, y2 = det.x1, det.y1, det.x2, det.y2
        draw.rectangle([x1, y1, x2, y2], outline=rgb, width=2)
        r = max(3, int(min(out.width, out.height) * 0.015))
        draw.ellipse(
            [det.cx - r, det.cy - r, det.cx + r, det.cy + r],
            fill=(255, 55, 95),
            outline=(255, 255, 255),
        )
        if show_labels:
            label = f"{det.name} {det.conf:.2f}"
            tw = draw.textlength(label, font=font)
            th = 16
            pad = 2
            ty = max(0, y1 - th - pad * 2)
            draw.rectangle([x1, ty, x1 + tw + pad * 2, ty + th + pad], fill=rgb)
            draw.text((x1 + pad, ty + pad // 2), label, fill=(0, 0, 0), font=font)
    return out


class ValidateApp:
    def __init__(
        self,
        root: tk.Tk,
        model_path: Optional[str] = None,
        image_path: Optional[str] = None,
    ) -> None:
        self.root = root
        self.root.title("Steel Ball — YOLO Validate")
        self.root.geometry("1100x720")
        self.root.minsize(900, 560)

        self.cfg = load_config()
        self.model_path = tk.StringVar(
            value=model_path
            or self.cfg.get("model_path")
            or str(find_default_model() or "")
        )
        self.image_dir = tk.StringVar(
            value=self.cfg.get("image_dir") or str(DATASET_IMAGES_DIR)
        )
        self.conf = tk.DoubleVar(value=float(self.cfg.get("conf", DEFAULT_CONF)))
        self.iou = tk.DoubleVar(value=float(self.cfg.get("iou", DEFAULT_IOU)))
        self.status_text = tk.StringVar(value="Load a model, then pick an image.")
        self.hint_text = tk.StringVar(value="")
        self.image_index_text = tk.StringVar(value="0 / 0")

        self._model = None
        self._model_loaded_path: Optional[str] = None
        self._busy = False
        self._images: List[Path] = []
        self._index = -1
        self._pil_orig: Optional[Image.Image] = None
        self._pil_drawn: Optional[Image.Image] = None
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._dets: List[Detection] = []
        self._resize_after: Optional[str] = None
        self._view_scale = 1.0
        self._view_ox = 0
        self._view_oy = 0

        self._build_ui()
        self._bind_keys()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._refresh_image_list()
        if image_path:
            p = Path(image_path)
            if p.is_file():
                self.image_dir.set(str(p.parent))
                self._refresh_image_list()
                self._select_image_path(p)
        elif self._images:
            self._show_index(0)

        if self.model_path.get().strip():
            self.root.after(80, self.load_model)

    def _build_ui(self) -> None:
        top = ttk.Frame(self.root, padding=(10, 8))
        top.pack(fill=tk.X)

        ttk.Label(top, text="Model").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.model_path, width=42).pack(side=tk.LEFT, padx=6)
        ttk.Button(top, text="Browse…", command=self.browse_model).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Load", command=self.load_model).pack(side=tk.LEFT, padx=2)

        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(top, text="Detect", command=self.run_detect).pack(side=tk.LEFT, padx=2)
        ttk.Button(top, text="Save result", command=self.save_result).pack(side=tk.LEFT, padx=2)

        mid = ttk.Frame(self.root, padding=(10, 0))
        mid.pack(fill=tk.X)

        ttk.Label(mid, text="Image dir").pack(side=tk.LEFT)
        ttk.Entry(mid, textvariable=self.image_dir, width=42).pack(side=tk.LEFT, padx=6)
        ttk.Button(mid, text="Browse…", command=self.browse_image_dir).pack(
            side=tk.LEFT, padx=2
        )
        ttk.Button(mid, text="Open file…", command=self.browse_image_file).pack(
            side=tk.LEFT, padx=2
        )
        ttk.Button(mid, text="Refresh", command=self._refresh_image_list).pack(
            side=tk.LEFT, padx=2
        )

        ttk.Separator(mid, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Button(mid, text="◀ Prev", command=lambda: self.nav(-1)).pack(
            side=tk.LEFT, padx=2
        )
        ttk.Label(mid, textvariable=self.image_index_text, width=10).pack(side=tk.LEFT)
        ttk.Button(mid, text="Next ▶", command=lambda: self.nav(1)).pack(
            side=tk.LEFT, padx=2
        )

        opts = ttk.Frame(self.root, padding=(10, 6))
        opts.pack(fill=tk.X)
        ttk.Label(opts, text="conf").pack(side=tk.LEFT)
        ttk.Scale(
            opts,
            from_=0.05,
            to=0.95,
            variable=self.conf,
            orient=tk.HORIZONTAL,
            length=140,
            command=lambda _v: self._on_thresh_change(),
        ).pack(side=tk.LEFT, padx=4)
        self._conf_label = ttk.Label(opts, text=f"{self.conf.get():.2f}", width=5)
        self._conf_label.pack(side=tk.LEFT)

        ttk.Label(opts, text="iou").pack(side=tk.LEFT, padx=(12, 0))
        ttk.Scale(
            opts,
            from_=0.1,
            to=0.95,
            variable=self.iou,
            orient=tk.HORIZONTAL,
            length=140,
            command=lambda _v: self._on_thresh_change(),
        ).pack(side=tk.LEFT, padx=4)
        self._iou_label = ttk.Label(opts, text=f"{self.iou.get():.2f}", width=5)
        self._iou_label.pack(side=tk.LEFT)

        ttk.Label(opts, textvariable=self.hint_text).pack(side=tk.LEFT, padx=12)

        body = ttk.Frame(self.root)
        body.pack(fill=tk.BOTH, expand=True, padx=10, pady=4)

        self.canvas = tk.Canvas(body, bg="#121418", highlightthickness=0)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", self._on_canvas_resize)

        side = ttk.Frame(body, width=260)
        side.pack(side=tk.RIGHT, fill=tk.Y, padx=(8, 0))
        side.pack_propagate(False)

        ttk.Label(side, text="Images").pack(anchor=tk.W)
        self.image_list = tk.Listbox(side, height=12, exportselection=False)
        self.image_list.pack(fill=tk.BOTH, expand=True, pady=(2, 8))
        self.image_list.bind("<<ListboxSelect>>", self._on_image_list_select)

        ttk.Label(side, text="Detections").pack(anchor=tk.W)
        self.det_list = tk.Listbox(side, height=10, exportselection=False)
        self.det_list.pack(fill=tk.BOTH, expand=True, pady=(2, 0))

        bottom = ttk.Frame(self.root, padding=(10, 6))
        bottom.pack(fill=tk.X)
        ttk.Label(bottom, textvariable=self.status_text).pack(side=tk.LEFT)

    def _bind_keys(self) -> None:
        self.root.bind("<Left>", lambda _e: self.nav(-1))
        self.root.bind("<Right>", lambda _e: self.nav(1))
        self.root.bind("<a>", lambda _e: self.nav(-1))
        self.root.bind("<d>", lambda _e: self.nav(1))
        self.root.bind("<A>", lambda _e: self.nav(-1))
        self.root.bind("<D>", lambda _e: self.nav(1))
        self.root.bind("<Return>", lambda _e: self.run_detect())
        self.root.bind("<Control-s>", lambda _e: self.save_result())
        self.root.bind("<Control-S>", lambda _e: self.save_result())
        self.root.bind("<F5>", lambda _e: self.run_detect())

    def _on_close(self) -> None:
        self.cfg["model_path"] = self.model_path.get().strip()
        self.cfg["image_dir"] = self.image_dir.get().strip()
        self.cfg["conf"] = float(self.conf.get())
        self.cfg["iou"] = float(self.iou.get())
        try:
            save_config(self.cfg)
        except OSError:
            pass
        self.root.destroy()

    def _on_thresh_change(self) -> None:
        self._conf_label.configure(text=f"{self.conf.get():.2f}")
        self._iou_label.configure(text=f"{self.iou.get():.2f}")

    def browse_model(self) -> None:
        path = filedialog.askopenfilename(
            title="Select YOLO model",
            initialdir=str(APP_DIR),
            filetypes=[
                ("YOLO weights", "*.pt *.onnx"),
                ("PyTorch", "*.pt"),
                ("ONNX", "*.onnx"),
                ("All", "*.*"),
            ],
        )
        if path:
            self.model_path.set(path)
            self.load_model()

    def browse_image_dir(self) -> None:
        path = filedialog.askdirectory(
            title="Select image folder",
            initialdir=self.image_dir.get() or str(DATASET_IMAGES_DIR),
        )
        if path:
            self.image_dir.set(path)
            self._refresh_image_list()
            if self._images:
                self._show_index(0)

    def browse_image_file(self) -> None:
        path = filedialog.askopenfilename(
            title="Select image",
            initialdir=self.image_dir.get() or str(DATASET_IMAGES_DIR),
            filetypes=[
                ("Images", "*.jpg *.jpeg *.png *.bmp *.webp"),
                ("All", "*.*"),
            ],
        )
        if not path:
            return
        p = Path(path)
        self.image_dir.set(str(p.parent))
        self._refresh_image_list()
        self._select_image_path(p)

    def load_model(self) -> None:
        path = self.model_path.get().strip().strip('"').strip("'")
        if not path:
            messagebox.showwarning("Model", "Please choose a model file (.pt / .onnx).")
            return
        mp = Path(path)
        if not mp.is_file():
            messagebox.showerror("Model", f"File not found:\n{mp}")
            return
        if mp.suffix.lower() not in MODEL_EXTS:
            if not messagebox.askyesno(
                "Model",
                f"Unusual extension {mp.suffix}. Load anyway?",
            ):
                return

        if self._busy:
            return
        self._busy = True
        self.status_text.set(f"Loading model: {mp.name} …")
        self.root.update_idletasks()

        def worker() -> None:
            err: Optional[str] = None
            model = None
            try:
                from ultralytics import YOLO

                model = YOLO(str(mp))
            except ImportError:
                err = (
                    "Missing dependency: ultralytics.\n"
                    "Run: py -3 -m pip install -r requirements.txt"
                )
            except Exception as exc:  # noqa: BLE001 — surface any load failure
                err = str(exc)

            def done() -> None:
                self._busy = False
                if err:
                    self._model = None
                    self._model_loaded_path = None
                    self.status_text.set("Model load failed.")
                    messagebox.showerror("Model", err)
                    return
                self._model = model
                self._model_loaded_path = str(mp.resolve())
                self.model_path.set(self._model_loaded_path)
                names = getattr(model, "names", None) or {}
                n = len(names) if isinstance(names, dict) else "?"
                self.status_text.set(f"Model ready: {mp.name}  (classes={n})")
                self.hint_text.set("Enter / F5 detect · ← → navigate · Ctrl+S save")
                if self._pil_orig is not None:
                    self.run_detect()

            self.root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_image_list(self) -> None:
        folder = Path(self.image_dir.get().strip() or str(DATASET_IMAGES_DIR))
        self._images = list_images_in(folder)
        self.image_list.delete(0, tk.END)
        for p in self._images:
            self.image_list.insert(tk.END, p.name)
        if not self._images:
            self._index = -1
            self._pil_orig = None
            self._pil_drawn = None
            self._dets = []
            self.image_index_text.set("0 / 0")
            self._redraw_canvas()
            self._refresh_det_list()
            self.status_text.set(f"No images in: {folder}")
        else:
            self.image_index_text.set(f"{max(1, self._index + 1)} / {len(self._images)}")

    def _select_image_path(self, path: Path) -> None:
        path = path.resolve()
        for i, p in enumerate(self._images):
            if p.resolve() == path:
                self._show_index(i)
                return
        # File not in current list (e.g. just opened): prepend
        self._images.insert(0, path)
        self.image_list.insert(0, path.name)
        self._show_index(0)

    def _on_image_list_select(self, _event=None) -> None:
        sel = self.image_list.curselection()
        if not sel:
            return
        idx = int(sel[0])
        if idx != self._index:
            self._show_index(idx)

    def nav(self, delta: int) -> None:
        if not self._images:
            return
        if self._index < 0:
            self._show_index(0)
            return
        self._show_index((self._index + delta) % len(self._images))

    def _show_index(self, index: int) -> None:
        if not self._images:
            return
        index = max(0, min(index, len(self._images) - 1))
        self._index = index
        path = self._images[index]
        self.image_index_text.set(f"{index + 1} / {len(self._images)}")
        self.image_list.selection_clear(0, tk.END)
        self.image_list.selection_set(index)
        self.image_list.see(index)
        try:
            self._pil_orig = Image.open(path).convert("RGB")
        except OSError as exc:
            self._pil_orig = None
            self._pil_drawn = None
            self._dets = []
            self.status_text.set(f"Failed to open: {path.name}")
            messagebox.showerror("Image", str(exc))
            self._redraw_canvas()
            self._refresh_det_list()
            return
        self._dets = []
        self._pil_drawn = self._pil_orig.copy()
        self._redraw_canvas()
        self._refresh_det_list()
        self.status_text.set(
            f"{path.name}  {self._pil_orig.width}x{self._pil_orig.height}"
        )
        if self._model is not None:
            self.run_detect()

    def run_detect(self) -> None:
        if self._busy:
            return
        if self._model is None:
            messagebox.showwarning("Detect", "Load a model first.")
            return
        if self._pil_orig is None or self._index < 0:
            messagebox.showwarning("Detect", "Select an image first.")
            return

        path = self._images[self._index]
        conf = float(self.conf.get())
        iou = float(self.iou.get())
        self._busy = True
        self.status_text.set(f"Detecting: {path.name} …")

        def worker() -> None:
            err: Optional[str] = None
            dets: List[Detection] = []
            try:
                results = self._model.predict(
                    source=str(path),
                    conf=conf,
                    iou=iou,
                    verbose=False,
                )
                r0 = results[0]
                names = r0.names or {}
                img_w, img_h = self._pil_orig.width, self._pil_orig.height
                boxes = r0.boxes
                if boxes is not None and len(boxes) > 0:
                    for box in boxes:
                        cls_i = int(box.cls[0].item())
                        conf_v = float(box.conf[0].item())
                        x1, y1, x2, y2 = (float(v) for v in box.xyxy[0].tolist())
                        cx = (x1 + x2) / 2.0
                        cy = (y1 + y2) / 2.0
                        name = str(names.get(cls_i, f"cls{cls_i}"))
                        dets.append(
                            Detection(
                                cls=cls_i,
                                name=name,
                                conf=conf_v,
                                x1=x1,
                                y1=y1,
                                x2=x2,
                                y2=y2,
                                cx=cx,
                                cy=cy,
                                cx_norm=cx / float(img_w) if img_w else 0.0,
                                cy_norm=cy / float(img_h) if img_h else 0.0,
                            )
                        )
            except Exception as exc:  # noqa: BLE001
                err = str(exc)

            def done() -> None:
                self._busy = False
                if err:
                    self.status_text.set("Detect failed.")
                    messagebox.showerror("Detect", err)
                    return
                self._dets = dets
                assert self._pil_orig is not None
                self._pil_drawn = draw_detections(self._pil_orig, dets)
                self._redraw_canvas()
                self._refresh_det_list()
                self.status_text.set(
                    f"{path.name}  ·  {len(dets)} det  ·  conf≥{conf:.2f}"
                )

            self.root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_det_list(self) -> None:
        self.det_list.delete(0, tk.END)
        for i, d in enumerate(self._dets, 1):
            self.det_list.insert(
                tk.END,
                f"{i}. {d.name} {d.conf:.2f}  "
                f"cx={d.cx_norm:.3f} cy={d.cy_norm:.3f}",
            )
        if not self._dets and self._pil_orig is not None:
            self.det_list.insert(tk.END, "(no detections)")

    def save_result(self) -> None:
        if self._pil_drawn is None or self._index < 0:
            messagebox.showwarning("Save", "Nothing to save.")
            return
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        stem = self._images[self._index].stem
        default_name = f"result_{stem}.jpg"
        out = filedialog.asksaveasfilename(
            title="Save detection result",
            initialdir=str(RESULT_DIR),
            initialfile=default_name,
            defaultextension=".jpg",
            filetypes=[("JPEG", "*.jpg"), ("PNG", "*.png"), ("All", "*.*")],
        )
        if not out:
            return
        try:
            self._pil_drawn.save(out, quality=95)
        except OSError as exc:
            messagebox.showerror("Save", str(exc))
            return
        self.status_text.set(f"Saved: {out}")

    def _on_canvas_resize(self, _event=None) -> None:
        if self._resize_after is not None:
            self.root.after_cancel(self._resize_after)
        self._resize_after = self.root.after(RESIZE_DEBOUNCE_MS, self._redraw_canvas)

    def _redraw_canvas(self) -> None:
        self._resize_after = None
        self.canvas.delete("all")
        img = self._pil_drawn or self._pil_orig
        if img is None:
            return
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        scale = min(cw / img.width, ch / img.height, 1.0)
        if scale <= 0:
            return
        nw = max(1, int(img.width * scale))
        nh = max(1, int(img.height * scale))
        shown = img if (nw, nh) == img.size else img.resize((nw, nh), Image.Resampling.LANCZOS)
        self._photo = ImageTk.PhotoImage(shown)
        ox = (cw - nw) // 2
        oy = (ch - nh) // 2
        self._view_scale = scale
        self._view_ox = ox
        self._view_oy = oy
        self.canvas.create_image(ox, oy, anchor=tk.NW, image=self._photo)


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="YOLO steel-ball detection validator (GUI)")
    p.add_argument("-m", "--model", type=str, default=None, help="Initial .pt / .onnx path")
    p.add_argument("-i", "--image", type=str, default=None, help="Initial image path")
    return p.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> None:
    args = parse_args(argv)
    root = tk.Tk()
    try:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass
    ValidateApp(root, model_path=args.model, image_path=args.image)
    root.mainloop()


if __name__ == "__main__":
    main()
