# -*- coding: utf-8 -*-
"""Pet Design — author body color / doodle / PNG per clip; write via skin_core."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QPushButton,
    QRadioButton,
    QSlider,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

import struct

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext
from app.widgets.body_paint import BodyPaintCanvas
from app.widgets.color_button import ColorButton, hex_color, rgb_tuple
from app.widgets.round_preview import RoundPreview


class DesignFeature(FeatureModule):
    id = "design"
    title = "Design"
    subtitle = "draw / import body | pack.json → body/*.bin"
    status = "ready"
    order = 15

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return DesignPanel(self.ctx, parent)


class DesignPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        self._cfg: dict = {}
        self._clip_id = "idle"

        self.clip_list = QListWidget()
        self.clip_list.currentTextChanged.connect(self._on_clip)

        self.size_slider = QSlider(Qt.Orientation.Horizontal)
        self.size_slider.setRange(skin_core.BODY_SIZE_MIN, skin_core.BODY_SIZE_MAX)
        self.size_slider.setValue(skin_core.BODY_SIZE_DEFAULT)
        self.size_lbl = QLabel(f"{skin_core.BODY_SIZE_DEFAULT}px")
        self.size_slider.valueChanged.connect(self._on_size)

        self.btn_color = ColorButton(QColor(74, 163, 200))
        self.btn_color.colorChanged.connect(self._on_color)
        self.btn_brush = ColorButton(QColor(74, 163, 200))
        self.btn_brush.colorChanged.connect(
            lambda c: self.canvas.set_brush_color(c)
        )

        self.brush_slider = QSlider(Qt.Orientation.Horizontal)
        self.brush_slider.setRange(1, 24)
        self.brush_slider.setValue(6)
        self.brush_lbl = QLabel("6px")
        self.brush_slider.valueChanged.connect(self._on_brush)

        self.chk_erase = QCheckBox("Eraser")

        self.fit_contain = QRadioButton("contain")
        self.fit_cover = QRadioButton("cover")
        self.fit_contain.setChecked(True)
        fit_g = QButtonGroup(self)
        fit_g.addButton(self.fit_contain)
        fit_g.addButton(self.fit_cover)

        self.chk_face = QCheckBox("Preview LVGL face overlay")
        self.chk_face.setChecked(True)
        self.chk_face.toggled.connect(lambda _: self.refresh_preview())

        self.src_lbl = QLabel("source: (synthetic color)")
        self.src_lbl.setStyleSheet("color:#8b93a7;")
        self.src_lbl.setWordWrap(True)

        self.canvas = BodyPaintCanvas()
        self.canvas.changed.connect(self.refresh_preview)
        self.chk_erase.toggled.connect(self.canvas.set_erase)

        self.preview = RoundPreview()
        self.preview.set_show_arc(False)

        # --- actions ---
        btn_disk = QPushButton("Fill disk")
        btn_disk.clicked.connect(self._fill_disk)
        btn_clear = QPushButton("Clear canvas")
        btn_clear.clicked.connect(self.canvas.clear)
        btn_import = QPushButton("Import PNG…")
        btn_import.clicked.connect(self._import_png)
        btn_apply = QPushButton("Apply doodle → clip")
        btn_apply.clicked.connect(self._apply_doodle)
        btn_clear_src = QPushButton("Use color only")
        btn_clear_src.clicked.connect(self._clear_source)
        btn_apply_all = QPushButton("Color → all clips")
        btn_apply_all.clicked.connect(self._color_all)
        btn_save = QPushButton("Save pack.json")
        btn_save.clicked.connect(self._save_json)
        btn_build = QPushButton("Build pack.bin")
        btn_build.clicked.connect(self._build)
        btn_both = QPushButton("Build pack + splash")
        btn_both.clicked.connect(self._build_both)

        form = QVBoxLayout()
        tip = QLabel(
            "固件只读 SD 上的 RGBH 身体；在此设计后 Build，拷到 /sdcard/pet/。\n"
            "五官由固件 LVGL 叠加，身体帧不要画眼睛（预览可勾选模拟）。"
        )
        tip.setWordWrap(True)
        tip.setStyleSheet("color:#8b93a7;")
        form.addWidget(tip)

        form.addWidget(QLabel("Clip"))
        form.addWidget(self.clip_list)

        size_row = QHBoxLayout()
        size_row.addWidget(QLabel("Body size"))
        size_row.addWidget(self.size_slider, 1)
        size_row.addWidget(self.size_lbl)
        form.addLayout(size_row)

        color_row = QHBoxLayout()
        color_row.addWidget(QLabel("Clip color"))
        color_row.addWidget(self.btn_color)
        color_row.addWidget(btn_apply_all)
        color_row.addStretch(1)
        form.addLayout(color_row)

        form.addWidget(QLabel("Doodle"))
        form.addWidget(self.canvas, 0, Qt.AlignmentFlag.AlignHCenter)
        brush_row = QHBoxLayout()
        brush_row.addWidget(QLabel("Brush"))
        brush_row.addWidget(self.btn_brush)
        brush_row.addWidget(self.brush_slider, 1)
        brush_row.addWidget(self.brush_lbl)
        brush_row.addWidget(self.chk_erase)
        form.addLayout(brush_row)

        doodle_row = QHBoxLayout()
        doodle_row.addWidget(btn_disk)
        doodle_row.addWidget(btn_clear)
        doodle_row.addWidget(btn_import)
        form.addLayout(doodle_row)
        form.addWidget(btn_apply)
        form.addWidget(btn_clear_src)
        form.addWidget(self.src_lbl)

        fit_row = QHBoxLayout()
        fit_row.addWidget(QLabel("Import fit"))
        fit_row.addWidget(self.fit_contain)
        fit_row.addWidget(self.fit_cover)
        fit_row.addStretch(1)
        form.addLayout(fit_row)
        form.addWidget(self.chk_face)

        act = QHBoxLayout()
        act.addWidget(btn_save)
        act.addWidget(btn_build)
        act.addWidget(btn_both)
        form.addLayout(act)
        form.addStretch(1)

        left = QWidget()
        left.setLayout(form)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(left)
        split.addWidget(self.preview)
        split.setStretchFactor(0, 1)
        split.setStretchFactor(1, 2)

        root = QVBoxLayout(self)
        root.addWidget(split)

        self.ctx.cfg_changed.connect(lambda _: self.reload())
        self.reload()

    def _fit(self) -> str:
        return "cover" if self.fit_cover.isChecked() else "contain"

    def _clip(self) -> dict | None:
        for c in self._cfg.get("clips", []):
            if c.get("id") == self._clip_id:
                return c
        return None

    def reload(self) -> None:
        try:
            self._cfg = self.ctx.load_cfg()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design reload error: {exc}")
            return
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        self._cfg["width"] = w
        self._cfg["height"] = w
        self.size_slider.blockSignals(True)
        self.size_slider.setValue(w)
        self.size_slider.blockSignals(False)
        self.size_lbl.setText(f"{w}px")
        self.canvas.set_body_size(w)

        self.clip_list.blockSignals(True)
        self.clip_list.clear()
        for c in self._cfg.get("clips", []):
            self.clip_list.addItem(str(c.get("id")))
        self.clip_list.blockSignals(False)
        ids = [c.get("id") for c in self._cfg.get("clips", [])]
        if self._clip_id in ids:
            self.clip_list.setCurrentRow(ids.index(self._clip_id))
        elif ids:
            self.clip_list.setCurrentRow(0)
        self._load_clip_ui()
        self.refresh_preview()

    def _on_clip(self, name: str) -> None:
        if not name:
            return
        self._clip_id = name
        self._load_clip_ui()
        self.refresh_preview()

    def _load_clip_ui(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        r, g, b = (int(x) for x in clip.get("color", [74, 163, 200]))
        self.btn_color.blockSignals(True)
        self.btn_color.set_color(QColor(r, g, b))
        self.btn_color.blockSignals(False)
        self.btn_brush.set_color(QColor(r, g, b))
        self.canvas.set_brush_color(QColor(r, g, b))
        fit = str(clip.get("fit", "contain"))
        self.fit_cover.setChecked(fit == "cover")
        self.fit_contain.setChecked(fit != "cover")
        src = clip.get("source") or ""
        if src:
            self.src_lbl.setText(f"source: {src}")
            try:
                path = skin_core.resolve_asset(str(src), self.ctx.cfg_path.parent)
                img = QImage(str(path))
                if not img.isNull():
                    self.canvas.load_qimage(img)
                    return
            except Exception:  # noqa: BLE001
                pass
        else:
            self.src_lbl.setText("source: (synthetic color)")
        self.canvas.fill_disk(QColor(r, g, b))

    def _on_size(self, v: int) -> None:
        self.size_lbl.setText(f"{v}px")
        self._cfg["width"] = int(v)
        self._cfg["height"] = int(v)
        self.canvas.set_body_size(int(v))
        self.refresh_preview()

    def _on_color(self, c: QColor) -> None:
        clip = self._clip()
        if clip is None:
            return
        clip["color"] = list(rgb_tuple(c))
        self.btn_brush.set_color(c)
        self.canvas.set_brush_color(c)
        if not clip.get("source") and not clip.get("sources"):
            self.canvas.fill_disk(c)
        self.refresh_preview()

    def _on_brush(self, v: int) -> None:
        self.brush_lbl.setText(f"{v}px")
        self.canvas.set_brush_radius(v)

    def _fill_disk(self) -> None:
        self.canvas.fill_disk(self.btn_color.color())

    def _import_png(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Import body image",
            "",
            "Images (*.png *.jpg *.jpeg *.bmp *.webp);;All (*.*)",
        )
        if not path:
            return
        img = QImage(path)
        if img.isNull():
            self.ctx.info(f"design: cannot open {path}")
            return
        self.canvas.load_qimage(img)
        # Persist as asset under pack.json/assets/
        clip = self._clip()
        if clip is None:
            return
        assets = self.ctx.cfg_path.parent / "assets"
        assets.mkdir(parents=True, exist_ok=True)
        dest = assets / f"{self._clip_id}.png"
        img.save(str(dest), "PNG")
        rel = f"assets/{self._clip_id}.png"
        clip["source"] = rel
        clip["fit"] = self._fit()
        clip.pop("sources", None)
        self.src_lbl.setText(f"source: {rel}")
        self.ctx.info(f"design: imported → {dest}")
        self.refresh_preview()

    def _apply_doodle(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        assets = self.ctx.cfg_path.parent / "assets"
        assets.mkdir(parents=True, exist_ok=True)
        dest = assets / f"{self._clip_id}.png"
        self.canvas.to_qimage().save(str(dest), "PNG")
        rel = f"assets/{self._clip_id}.png"
        clip["source"] = rel
        clip["fit"] = "contain"
        clip.pop("sources", None)
        clip["color"] = list(rgb_tuple(self.btn_color.color()))
        self.src_lbl.setText(f"source: {rel}")
        self.ctx.info(f"design: doodle saved → {dest}")
        self.refresh_preview()

    def _clear_source(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        clip.pop("source", None)
        clip.pop("sources", None)
        self.src_lbl.setText("source: (synthetic color)")
        self.canvas.fill_disk(self.btn_color.color())
        self.ctx.info(f"design: {self._clip_id} → synthetic color")
        self.refresh_preview()

    def _color_all(self) -> None:
        rgb = list(rgb_tuple(self.btn_color.color()))
        for c in self._cfg.get("clips", []):
            c["color"] = list(rgb)
        self.ctx.info(f"design: color {hex_color(self.btn_color.color())} → all clips")

    def _body_pixels(self) -> tuple[bytes, int, int]:
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        h = w
        clip = self._clip() or {"id": "idle", "color": [74, 163, 200], "frames": 1}
        # Prefer live canvas for preview when editing.
        img = self.canvas.to_qimage().convertToFormat(QImage.Format.Format_RGB888)
        if img.width() != w or img.height() != h:
            img = img.scaled(w, h, Qt.AspectRatioMode.IgnoreAspectRatio,
                             Qt.TransformationMode.SmoothTransformation)
        out = bytearray()
        for y in range(h):
            for x in range(w):
                c = img.pixelColor(x, y)
                out += struct.pack("<H", skin_core.rgb565(c.red(), c.green(), c.blue()))
        return bytes(out), w, h

    def refresh_preview(self) -> None:
        try:
            body, w, h = self._body_pixels()
            pixels = skin_core.compose_home_preview(
                body, w, h, skin_core.BG, self.chk_face.isChecked()
            )
            self.preview.set_rgb565(pixels)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design preview error: {exc}")

    def _save_json(self) -> None:
        try:
            clip = self._clip()
            if clip is not None:
                clip["fit"] = self._fit()
                clip["color"] = list(rgb_tuple(self.btn_color.color()))
            self._cfg["width"] = int(self.size_slider.value())
            self._cfg["height"] = int(self.size_slider.value())
            path = skin_core.save_cfg(self._cfg, self.ctx.cfg_path)
            self.ctx.info(f"saved {path}")
            self.ctx.cfg_changed.emit(str(path))
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design save error: {exc}")

    def _build(self) -> None:
        try:
            self._save_json()
            msg = skin_core.build_pack(self._cfg, self.ctx.out_dir, self.ctx.cfg_path)
            self.ctx.info(msg)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design build error: {exc}")

    def _build_both(self) -> None:
        try:
            self._build()
            idle = skin_core.idle_color_from_cfg(self._cfg)
            self.ctx.info(
                skin_core.build_splash(
                    self.ctx.out_dir,
                    None,
                    "contain",
                    idle,
                    self._cfg,
                    skin_core.BG,
                    skin_core.SPLASH_CONTENT_DEFAULT,
                )
            )
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design pack+splash error: {exc}")
