#!/usr/bin/env python3
"""
LCD 图片可视化转换工具（Tk GUI）

将 PNG / JPEG / BMP 等常见格式转为：
  - RGB565 裸 .bin 或带 RGBH 头的 .bin（与 `project/tools/lcd_rgb565_convert.py` 一致）
  - Windows BI_RGB 24 位 .bmp（自下而上扫描行，供固件 `lcd_gallery` 解码）
  - 可先旋转 0/90/180/270°（逆时针，Pillow）再按 Fit 缩放到目标横屏分辨率。

输出目录：与本脚本同级目录（仓库根下 `tools/`）。
输出文件名：短格式 `l{MMDDHHMMSS}_{slug6}.bmp` / `.bin`；非 0° 旋转时中间名带 `_r{角度}`；RGBH 头为 `…h.bin`。

依赖：Pillow（见同目录 `requirements-lcd-image.txt`）；Tk 为 Python 自带（Windows 安装器通常已包含）。
"""

from __future__ import annotations

import re
import struct
import sys
from datetime import datetime
from pathlib import Path

# 复用 project/tools 中的编码与缩放逻辑
_REPO_ROOT = Path(__file__).resolve().parent.parent
_PROJECT_TOOLS = _REPO_ROOT / "project" / "tools"
if _PROJECT_TOOLS.is_dir():
    sys.path.insert(0, str(_PROJECT_TOOLS))

SCRIPT_DIR = Path(__file__).resolve().parent

# 预览区逻辑尺寸（约 240:135）；实际像素随窗口 Configure 更新
_PREVIEW_BASE_W = 360
_PREVIEW_BASE_H = int(round(_PREVIEW_BASE_W * 135 / 240))


def apply_rotation_deg(im, deg: int):
    """在 Fit 缩放之前旋转整图。deg 为 Pillow 约定：正数 = 逆时针；expand 便于竖图转横构图。"""
    from PIL import Image

    d = int(deg) % 360
    if d == 0:
        return im
    return im.rotate(d, expand=True, resample=Image.Resampling.BICUBIC, fillcolor=(0, 0, 0))


def _slug_from_stem(stem: str, max_len: int = 6) -> str:
    s = re.sub(r"[^0-9A-Za-z]+", "", stem).lower()
    if not s:
        s = "x"
    return s[:max_len]


def _build_output_basename(*, stem: str, kind: str, rotate_deg: int = 0) -> str:
    """短文件名；rotate_deg≠0 时插入 `_r{角度}` 便于区分横竖处理。"""
    ts = datetime.now().strftime("%m%d%H%M%S")
    slug = _slug_from_stem(stem, 6)
    r = int(rotate_deg) % 360
    mid = f"{ts}_{slug}" + (f"_r{r}" if r != 0 else "")
    if kind == "bmp24":
        return f"l{mid}.bmp"
    if kind == "rgb565_hdr":
        return f"l{mid}h.bin"
    return f"l{mid}.bin"


def write_bmp24_bottom_up(path: Path, rgb_img) -> None:
    """RGB 图写入标准 BMP，24 位 BI_RGB，自下而上（与常见 BMP 一致，固件按正高度解析）。"""
    if rgb_img.mode != "RGB":
        rgb_img = rgb_img.convert("RGB")
    w, h = rgb_img.size
    px = rgb_img.load()
    row_stride = ((w * 3 + 3) // 4) * 4
    pad = row_stride - w * 3
    pad_bytes = bytes([0]) * pad

    pixel_bytes = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b = px[x, y][:3]
            row.extend((b, g, r))
        row.extend(pad_bytes)
        pixel_bytes.extend(row)

    header_size = 14 + 40
    off_bits = header_size
    file_size = off_bits + len(pixel_bytes)

    file_hdr = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, off_bits)
    dib = struct.pack(
        "<IiiHHIIiiII",
        40,
        w,
        h,
        1,
        24,
        0,
        len(pixel_bytes),
        0,
        0,
        0,
        0,
    )
    path.write_bytes(file_hdr + dib + bytes(pixel_bytes))


class LcdImageToolApp:
    def __init__(self) -> None:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk

        self._tk = tk
        self._filedialog = filedialog
        self._messagebox = messagebox
        self._ttk = ttk

        self.root = tk.Tk()
        self.root.title("LCD 图片转换 — 输出 tools/")
        self.root.minsize(480, 520)

        self.src_path: Path | None = None
        self._preview_ref = None
        self._preview_job: str | None = None
        self._preview_cw = _PREVIEW_BASE_W
        self._preview_ch = _PREVIEW_BASE_H

        frm = ttk.Frame(self.root, padding=10)
        frm.pack(fill=tk.BOTH, expand=True)

        ttk.Button(frm, text="选择图片…", command=self._pick_file).pack(anchor=tk.W)
        self.lbl_path = ttk.Label(frm, text="未选择文件", wraplength=440, justify=tk.LEFT)
        self.lbl_path.pack(fill=tk.X, pady=(4, 8))

        prev_fr = ttk.LabelFrame(frm, text="预览（随宽高 / Fit / 旋转自动更新）")
        prev_fr.pack(fill=tk.BOTH, expand=True, pady=(0, 8))
        prev_fr.rowconfigure(0, weight=1)
        prev_fr.columnconfigure(0, weight=1)
        self.canvas = tk.Canvas(
            prev_fr,
            width=_PREVIEW_BASE_W,
            height=_PREVIEW_BASE_H,
            bg="#1e1e1e",
            highlightthickness=1,
            highlightbackground="#444",
        )
        self.canvas.grid(row=0, column=0, sticky="nsew", padx=4, pady=4)
        self.canvas.bind("<Configure>", self._on_preview_configure)

        opts = ttk.LabelFrame(frm, text="目标与算法")
        opts.pack(fill=tk.X, pady=4)

        g1 = ttk.Frame(opts)
        g1.pack(fill=tk.X, padx=6, pady=4)
        ttk.Label(g1, text="宽度").grid(row=0, column=0, sticky=tk.W)
        self.var_w = tk.StringVar(value="240")
        e_w = ttk.Entry(g1, textvariable=self.var_w, width=8)
        e_w.grid(row=0, column=1, padx=4)
        ttk.Label(g1, text="高度").grid(row=0, column=2, sticky=tk.W, padx=(12, 0))
        self.var_h = tk.StringVar(value="135")
        e_h = ttk.Entry(g1, textvariable=self.var_h, width=8)
        e_h.grid(row=0, column=3, padx=4)

        ttk.Label(g1, text="Fit").grid(row=1, column=0, sticky=tk.W, pady=(8, 0))
        self.var_fit = tk.StringVar(value="cover")
        fit_cb = ttk.Combobox(
            g1,
            textvariable=self.var_fit,
            values=("cover", "contain", "stretch"),
            state="readonly",
            width=10,
        )
        fit_cb.grid(row=1, column=1, sticky=tk.W, pady=(8, 0))
        fit_cb.bind("<<ComboboxSelected>>", lambda _e: self._schedule_preview())

        ttk.Label(g1, text="旋转").grid(row=2, column=0, sticky=tk.W, pady=(8, 0))
        self.var_rotate = tk.StringVar(value="0")
        rot_cb = ttk.Combobox(
            g1,
            textvariable=self.var_rotate,
            values=("0", "90", "180", "270"),
            state="readonly",
            width=5,
        )
        rot_cb.grid(row=2, column=1, sticky=tk.W, pady=(8, 0))
        rot_cb.bind("<<ComboboxSelected>>", lambda _e: self._schedule_preview())
        ttk.Label(g1, text="° 逆时针(PIL)；竖长图试 90° 或 270°", font=("", 9)).grid(
            row=2, column=2, columnspan=2, sticky=tk.W, padx=(8, 0), pady=(8, 0)
        )

        ttk.Label(g1, text="像素序").grid(row=3, column=0, sticky=tk.W, pady=(8, 0))
        self.var_order = tk.StringVar(value="row")
        ttk.Radiobutton(g1, text="行 row", variable=self.var_order, value="row").grid(
            row=3, column=1, sticky=tk.W, pady=(8, 0)
        )
        ttk.Radiobutton(g1, text="列 column", variable=self.var_order, value="column").grid(
            row=3, column=2, sticky=tk.W, padx=(8, 0), pady=(8, 0)
        )

        self.var_swap_rb = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            opts,
            text="交换 R/B（MADCTL_BGR；影响 bin 输出，预览同步）",
            variable=self.var_swap_rb,
            command=self._schedule_preview,
        ).pack(anchor=tk.W, padx=6, pady=4)

        for v in (self.var_w, self.var_h, self.var_fit, self.var_order, self.var_rotate):
            v.trace_add("write", lambda *_a, **_k: self._schedule_preview())

        out_fr = ttk.LabelFrame(frm, text="输出格式（保存到脚本目录，短文件名）")
        out_fr.pack(fill=tk.X, pady=6)
        self.var_out = tk.StringVar(value="rgb565")
        ttk.Radiobutton(out_fr, text="RGB565 裸 .bin", variable=self.var_out, value="rgb565").pack(anchor=tk.W, padx=8)
        ttk.Radiobutton(out_fr, text="RGB565 + RGBH 头 .bin（…h.bin）", variable=self.var_out, value="rgb565_hdr").pack(
            anchor=tk.W, padx=8
        )
        ttk.Radiobutton(out_fr, text="BMP 24 位 .bmp", variable=self.var_out, value="bmp24").pack(anchor=tk.W, padx=8)

        ttk.Button(frm, text="生成文件", command=self._convert).pack(pady=10)

        log_fr = ttk.LabelFrame(frm, text="日志")
        log_fr.pack(fill=tk.BOTH, expand=True, pady=(4, 0))
        self.txt_log = tk.Text(log_fr, height=5, wrap="word", state=tk.DISABLED)
        self.txt_log.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self._draw_preview_placeholder()

    def _on_preview_configure(self, event) -> None:
        if event.widget is not self.canvas:
            return
        w = max(80, event.width - 2)
        h = max(60, event.height - 2)
        if w != self._preview_cw or h != self._preview_ch:
            self._preview_cw = w
            self._preview_ch = h
            self._schedule_preview()

    def _schedule_preview(self) -> None:
        if self._preview_job is not None:
            self.root.after_cancel(self._preview_job)
        self._preview_job = self.root.after(180, self._run_scheduled_preview)

    def _run_scheduled_preview(self) -> None:
        self._preview_job = None
        self._refresh_preview()

    def _draw_preview_placeholder(self) -> None:
        self.canvas.delete("all")
        self._preview_ref = None
        cw = max(80, self.canvas.winfo_width() or self._preview_cw)
        ch = max(60, self.canvas.winfo_height() or self._preview_ch)
        self.canvas.create_text(
            cw // 2,
            ch // 2,
            text="选择图片后显示预览",
            fill="#888",
            font=("", 11),
        )

    def _log(self, msg: str) -> None:
        self.txt_log.configure(state=tk.NORMAL)
        self.txt_log.insert(self._tk.END, msg + "\n")
        self.txt_log.see(self._tk.END)
        self.txt_log.configure(state=tk.DISABLED)

    def _pick_file(self) -> None:
        p = self._filedialog.askopenfilename(
            title="选择图片",
            filetypes=[
                ("图片", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff"),
                ("全部", "*.*"),
            ],
        )
        if not p:
            return
        self.src_path = Path(p)
        self.lbl_path.configure(text=str(self.src_path))
        self.root.update_idletasks()
        self.lbl_path.configure(wraplength=max(280, self.root.winfo_width() - 40))
        self._schedule_preview()

    def _refresh_preview(self) -> None:
        self.canvas.delete("all")
        self._preview_ref = None

        if self.src_path is None or not self.src_path.is_file():
            self._draw_preview_placeholder()
            return

        try:
            from PIL import Image, ImageTk

            from lcd_rgb565_convert import fit_resize_image, image_to_rgb565_bytes
        except ImportError as e:
            tk = self._tk
            cw = max(80, int(self.canvas.winfo_width() or self._preview_cw))
            ch = max(60, int(self.canvas.winfo_height() or self._preview_ch))
            self.canvas.create_text(
                cw // 2, ch // 2, text=f"缺少依赖:\n{e}", fill="#c44", font=("", 10), justify=tk.CENTER
            )
            return

        try:
            tw = int(self.var_w.get().strip())
            th = int(self.var_h.get().strip())
        except ValueError:
            tw, th = 240, 135

        if tw < 1 or th < 1:
            tw, th = 240, 135

        try:
            im = Image.open(self.src_path).convert("RGB")
        except OSError as e:
            tk = self._tk
            cw = max(80, int(self.canvas.winfo_width() or self._preview_cw))
            ch = max(60, int(self.canvas.winfo_height() or self._preview_ch))
            self.canvas.create_text(cw // 2, ch // 2, text=f"无法打开图片:\n{e}", fill="#c44", justify=tk.CENTER)
            return

        try:
            rot_deg = int(self.var_rotate.get().strip())
        except ValueError:
            rot_deg = 0
        rot_deg = rot_deg % 360
        im = apply_rotation_deg(im, rot_deg)

        fit = self.var_fit.get()
        resized = fit_resize_image(im, tw, th, fit)

        # 预览用 RGB565 再解回近似 RGB，与 bin 输出色彩一致（含 swap_rb）
        order = self.var_order.get()
        swap_rb = self.var_swap_rb.get()
        raw565 = image_to_rgb565_bytes(resized, order=order, swap_rb=swap_rb)
        w0, h0 = resized.size
        preview_rgb = Image.new("RGB", (w0, h0))
        pxv = preview_rgb.load()
        idx = 0
        if order == "row":
            for yy in range(h0):
                for xx in range(w0):
                    hi = raw565[idx]
                    lo = raw565[idx + 1]
                    idx += 2
                    v = (hi << 8) | lo
                    r5 = (v >> 11) & 0x1F
                    g6 = (v >> 5) & 0x3F
                    b5 = v & 0x1F
                    r8 = (r5 << 3) | (r5 >> 2)
                    g8 = (g6 << 2) | (g6 >> 4)
                    b8 = (b5 << 3) | (b5 >> 2)
                    pxv[xx, yy] = (r8, g8, b8)
        else:
            for xx in range(w0):
                for yy in range(h0):
                    hi = raw565[idx]
                    lo = raw565[idx + 1]
                    idx += 2
                    v = (hi << 8) | lo
                    r5 = (v >> 11) & 0x1F
                    g6 = (v >> 5) & 0x3F
                    b5 = v & 0x1F
                    r8 = (r5 << 3) | (r5 >> 2)
                    g8 = (g6 << 2) | (g6 >> 4)
                    b8 = (b5 << 3) | (b5 >> 2)
                    pxv[xx, yy] = (r8, g8, b8)

        self.root.update_idletasks()
        cw = max(80, int(self.canvas.winfo_width() or self._preview_cw))
        ch = max(60, int(self.canvas.winfo_height() or self._preview_ch))

        rw, rh = preview_rgb.size
        if rw <= 0 or rh <= 0:
            return
        scale = min(cw / rw, ch / rh)
        nw = max(1, int(round(rw * scale)))
        nh = max(1, int(round(rh * scale)))
        resample = Image.Resampling.NEAREST if scale >= 1.0 else Image.Resampling.LANCZOS
        thumb = preview_rgb.resize((nw, nh), resample)

        self._preview_ref = ImageTk.PhotoImage(thumb)
        self.canvas.create_image(cw // 2, ch // 2, image=self._preview_ref)

    def _convert(self) -> None:
        if self.src_path is None or not self.src_path.is_file():
            self._messagebox.showwarning("提示", "请先选择图片文件。")
            return
        try:
            from PIL import Image

            from lcd_rgb565_convert import fit_resize_image, image_to_rgb565_bytes
        except ImportError:
            self._messagebox.showerror(
                "缺少依赖",
                "请安装 Pillow，例如：\n  python -m pip install -r tools/requirements-lcd-image.txt",
            )
            return

        try:
            tw = int(self.var_w.get().strip())
            th = int(self.var_h.get().strip())
        except ValueError:
            self._messagebox.showerror("参数错误", "宽、高须为整数。")
            return
        if tw < 1 or th < 1 or tw > 4096 or th > 4096:
            self._messagebox.showerror("参数错误", "宽高范围建议 1~4096。")
            return

        fit = self.var_fit.get()
        order = self.var_order.get()
        swap_rb = self.var_swap_rb.get()
        out_kind = self.var_out.get()

        try:
            rot_deg = int(self.var_rotate.get().strip())
        except ValueError:
            rot_deg = 0
        rot_deg = rot_deg % 360

        im = Image.open(self.src_path).convert("RGB")
        im = apply_rotation_deg(im, rot_deg)
        resized = fit_resize_image(im, tw, th, fit)

        base = _build_output_basename(stem=self.src_path.stem, kind=out_kind, rotate_deg=rot_deg)
        out_path = SCRIPT_DIR / base

        try:
            if out_kind == "bmp24":
                write_bmp24_bottom_up(out_path, resized)
                self._log(f"[OK] BMP  -> {out_path.name} ({tw}x{th}, rot={rot_deg}°)")
            else:
                raw = image_to_rgb565_bytes(resized, order=order, swap_rb=swap_rb)
                if out_kind == "rgb565_hdr":
                    flags = 0
                    if order == "column":
                        flags |= 1
                    blob = struct.pack("<4sHHII", b"RGBH", tw, th, flags, 0) + raw
                else:
                    blob = raw
                out_path.write_bytes(blob)
                self._log(
                    f"[OK] BIN  -> {out_path.name} ({len(blob)} B, rot={rot_deg}°, order={order}, swap_rb={swap_rb}, hdr={out_kind == 'rgb565_hdr'})"
                )
        except OSError as e:
            self._messagebox.showerror("写入失败", str(e))
            self._log(f"[ERR] {e}")
            return

        self._messagebox.showinfo("完成", f"已保存:\n{out_path.name}\n目录: {SCRIPT_DIR}")
        self._log(f"完整路径: {out_path}")

    def run(self) -> None:
        self.root.mainloop()


def main() -> int:
    try:
        import tkinter as tk  # noqa: F401
    except ImportError:
        print("当前 Python 未包含 tkinter，无法启动 GUI。", file=sys.stderr)
        print("Windows：请勾选安装器的 tcl/tk；或换用带 Tk 的 Python。", file=sys.stderr)
        return 1

    if not _PROJECT_TOOLS.is_dir():
        print(f"未找到 {_PROJECT_TOOLS}，请在仓库根目录运行本脚本。", file=sys.stderr)
        return 2

    LcdImageToolApp().run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
