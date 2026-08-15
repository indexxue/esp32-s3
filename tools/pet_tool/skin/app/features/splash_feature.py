# -*- coding: utf-8 -*-
"""Boot splash - static boot/splash.bin only (visual C). No anim in v1.

Canvas fixed 240x240; bg / body / ring colors and content size are adjustable.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QColorDialog,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QRadioButton,
    QScrollArea,
    QSlider,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext
from app.widgets.round_preview import RoundPreview


def _hex(c: QColor) -> str:
    return f"#{c.red():02X}{c.green():02X}{c.blue():02X}"


def _rgb_tuple(c: QColor) -> tuple[int, int, int]:
    return (c.red(), c.green(), c.blue())


class _ColorButton(QPushButton):
    """Swatch + hex label; click opens QColorDialog."""

    colorChanged = Signal(QColor)

    def __init__(self, color: QColor, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._color = QColor(color)
        self.setFixedHeight(28)
        self.setMinimumWidth(110)
        self.clicked.connect(self._pick)
        self._apply()

    def color(self) -> QColor:
        return QColor(self._color)

    def set_color(self, color: QColor) -> None:
        self._color = QColor(color)
        self._apply()
        self.colorChanged.emit(self._color)

    def _pick(self) -> None:
        c = QColorDialog.getColor(self._color, self, "Choose color")
        if c.isValid():
            self.set_color(c)

    def _apply(self) -> None:
        self.setText(_hex(self._color))
        lum = 0.299 * self._color.red() + 0.587 * self._color.green() + 0.114 * self._color.blue()
        fg = "#10141c" if lum > 160 else "#eef2ff"
        self.setStyleSheet(
            f"QPushButton {{ background:{_hex(self._color)}; color:{fg}; "
            f"border:1px solid #3a4258; border-radius:4px; padding:2px 8px; }}"
        )


class SplashFeature(FeatureModule):
    id = "splash"
    title = "Splash"
    subtitle = "boot/splash.bin | static frame | visual C"
    status = "ready"
    order = 10

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return SplashPanel(self.ctx, parent)


class SplashPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        self._src: Path | None = None
        self._loading = True

        self.path_edit = QLineEdit()
        self.path_edit.setPlaceholderText("Optional PNG/JPEG (empty = synthetic idle body)")
        self.path_edit.setReadOnly(True)

        btn_browse = QPushButton("Browse...")
        btn_browse.clicked.connect(self._browse)
        btn_clear = QPushButton("Clear")
        btn_clear.clicked.connect(self._clear)

        self.fit_contain = QRadioButton("contain")
        self.fit_cover = QRadioButton("cover")
        self.fit_contain.setChecked(True)
        fit_group = QButtonGroup(self)
        fit_group.addButton(self.fit_contain)
        fit_group.addButton(self.fit_cover)
        self.fit_contain.toggled.connect(lambda _: self._on_fit())
        self.fit_cover.toggled.connect(lambda _: self._on_fit())

        self.size_slider = QSlider(Qt.Orientation.Horizontal)
        self.size_slider.setRange(skin_core.SPLASH_CONTENT_MIN, skin_core.SPLASH_CONTENT_MAX)
        self.size_slider.setValue(skin_core.SPLASH_CONTENT_DEFAULT)
        self.size_lbl = QLabel(f"{skin_core.SPLASH_CONTENT_DEFAULT}px")
        self.size_slider.valueChanged.connect(self._on_size)
        self.size_slider.sliderReleased.connect(self._persist_splash_cfg)

        self.btn_bg = _ColorButton(QColor(0x20, 0x20, 0x20))
        self.btn_body = _ColorButton(QColor(74, 163, 200))
        self.btn_bg.colorChanged.connect(lambda _: self._on_bg())
        self.btn_body.colorChanged.connect(lambda _: self.refresh())

        self.btn_arc_track = _ColorButton(QColor(0x2A, 0x31, 0x48))
        self.btn_arc_ind = _ColorButton(QColor(0x7E, 0xC8, 0xFF))
        self.btn_arc_track.colorChanged.connect(lambda _: self._apply_arc_colors())
        self.btn_arc_ind.colorChanged.connect(lambda _: self._apply_arc_colors())

        self.arc_size = QSlider(Qt.Orientation.Horizontal)
        self.arc_size.setRange(48, 220)
        self.arc_size.setValue(132)
        self.arc_size_lbl = QLabel("132px")
        self.arc_size.valueChanged.connect(self._on_arc_size)

        self.chk_arc = QCheckBox("Show progress ring (firmware overlay preview)")
        self.chk_arc.setChecked(True)
        self.chk_arc.toggled.connect(self._toggle_arc)

        btn_preview = QPushButton("Refresh preview")
        btn_preview.clicked.connect(self.refresh)
        btn_write = QPushButton("Write splash.bin")
        btn_write.clicked.connect(self._write)

        form = QVBoxLayout()
        form.addWidget(QLabel("Canvas fixed 240x240 (device round screen)"))

        form.addWidget(QLabel("Source (static)"))
        row = QHBoxLayout()
        row.addWidget(self.path_edit, 1)
        row.addWidget(btn_browse)
        row.addWidget(btn_clear)
        form.addLayout(row)

        fit_row = QHBoxLayout()
        fit_row.addWidget(QLabel("Fit"))
        fit_row.addWidget(self.fit_contain)
        fit_row.addWidget(self.fit_cover)
        fit_row.addStretch(1)
        form.addLayout(fit_row)

        size_row = QHBoxLayout()
        size_row.addWidget(QLabel("Content size"))
        size_row.addWidget(self.size_slider, 1)
        size_row.addWidget(self.size_lbl)
        form.addLayout(size_row)

        form.addWidget(QLabel("Colors (written into splash.bin)"))
        color_row = QHBoxLayout()
        color_row.addWidget(QLabel("BG"))
        color_row.addWidget(self.btn_bg)
        color_row.addWidget(QLabel("Body"))
        color_row.addWidget(self.btn_body)
        color_row.addStretch(1)
        form.addLayout(color_row)

        form.addWidget(QLabel("Progress ring (preview only, LVGL overlay)"))
        arc_color_row = QHBoxLayout()
        arc_color_row.addWidget(QLabel("Track"))
        arc_color_row.addWidget(self.btn_arc_track)
        arc_color_row.addWidget(QLabel("Indicator"))
        arc_color_row.addWidget(self.btn_arc_ind)
        arc_color_row.addStretch(1)
        form.addLayout(arc_color_row)

        arc_size_row = QHBoxLayout()
        arc_size_row.addWidget(QLabel("Ring size"))
        arc_size_row.addWidget(self.arc_size, 1)
        arc_size_row.addWidget(self.arc_size_lbl)
        form.addLayout(arc_size_row)
        form.addWidget(self.chk_arc)

        hint = QLabel(
            "Browse copies the image into assets/splash.png and writes boot/splash.bin.\n"
            "Reopen / Pack / Design all use that file. Preview is the full 240x240 round screen.\n"
            "Content size scales the image inside the canvas; BG fills the rest."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8b93a7;")
        form.addWidget(hint)

        btn_row = QHBoxLayout()
        btn_row.addWidget(btn_preview)
        btn_row.addWidget(btn_write)
        btn_row.addStretch(1)
        form.addLayout(btn_row)
        form.addStretch(1)

        left = QWidget()
        left.setLayout(form)
        scroll = QScrollArea()
        scroll.setWidget(left)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setMinimumWidth(320)

        self.preview = RoundPreview()
        self._apply_arc_colors()

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(scroll)
        split.addWidget(self.preview)
        split.setStretchFactor(0, 1)
        split.setStretchFactor(1, 0)
        split.setChildrenCollapsible(False)

        root = QVBoxLayout(self)
        root.addWidget(split)

        self._sync_body_from_cfg()
        self._load_splash_cfg()
        self._autoload_splash_png()
        self._loading = False
        self.refresh()

    def _assets_dir(self) -> Path:
        return self.ctx.cfg_path.parent / "assets"

    def _load_splash_cfg(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
        except Exception:  # noqa: BLE001
            return
        fit, bg, size = skin_core.splash_params(cfg)
        self.fit_cover.setChecked(fit == "cover")
        self.fit_contain.setChecked(fit != "cover")
        self.btn_bg.set_color(QColor(bg[0], bg[1], bg[2]))
        if size is not None:
            self.size_slider.setValue(size)

    def _autoload_splash_png(self) -> None:
        src = skin_core.splash_src_from_assets(self.ctx.cfg_path.parent)
        if src is None:
            return
        self._src = src
        self.path_edit.setText(str(src))
        if skin_core.splash_params(self.ctx.load_cfg())[2] is None:
            self.size_slider.setValue(skin_core.SPLASH_SIZE)

    def _persist_splash_cfg(self) -> None:
        if self._loading:
            return
        cfg = self.ctx.load_cfg()
        skin_core.set_splash_cfg(cfg, self._fit(), self._bg(), self._content_size())
        skin_core.save_cfg(cfg, self.ctx.cfg_path)

    def _sync_body_from_cfg(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
            r, g, b = skin_core.idle_color_from_cfg(cfg)
            self.btn_body.set_color(QColor(r, g, b))
        except Exception:  # noqa: BLE001
            pass

    def _fit(self) -> str:
        return "cover" if self.fit_cover.isChecked() else "contain"

    def _content_size(self) -> int:
        return int(self.size_slider.value())

    def _bg(self) -> tuple[int, int, int]:
        return _rgb_tuple(self.btn_bg.color())

    def _body(self) -> tuple[int, int, int]:
        return _rgb_tuple(self.btn_body.color())

    def _on_fit(self) -> None:
        self.refresh()
        self._persist_splash_cfg()

    def _on_bg(self) -> None:
        self.refresh()
        self._persist_splash_cfg()

    def _on_size(self, v: int) -> None:
        self.size_lbl.setText(f"{v}px")
        self.refresh()
        if not self.size_slider.isSliderDown():
            self._persist_splash_cfg()

    def _on_arc_size(self, v: int) -> None:
        self.arc_size_lbl.setText(f"{v}px")
        self.preview.set_arc_size(float(v))

    def _apply_arc_colors(self) -> None:
        self.preview.set_arc_colors(self.btn_arc_track.color(), self.btn_arc_ind.color())

    def _browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Choose static splash image",
            "",
            "Images (*.png *.jpg *.jpeg *.bmp *.webp);;All (*.*)",
        )
        if not path:
            return
        try:
            self._src = skin_core.install_splash_image(Path(path), self._assets_dir())
            self.path_edit.setText(str(self._src))
            if self.size_slider.value() == skin_core.SPLASH_CONTENT_DEFAULT:
                self.size_slider.setValue(skin_core.SPLASH_SIZE)
            self._persist_splash_cfg()
            self.refresh()
            self._write()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"splash browse error: {exc}")

    def _clear(self) -> None:
        self._src = None
        self.path_edit.clear()
        self.size_slider.setValue(skin_core.SPLASH_CONTENT_DEFAULT)
        try:
            skin_core.clear_splash_assets(self._assets_dir())
            self._persist_splash_cfg()
            self.refresh()
            self._write()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"splash clear error: {exc}")

    def _toggle_arc(self, on: bool) -> None:
        self.preview.set_show_arc(on)

    def refresh(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
            pixels = skin_core.make_splash_pixels(
                self._src,
                self._fit(),
                self._body(),
                cfg,
                self._bg(),
                self._content_size(),
            )
            self.preview.set_rgb565(pixels)
            self.preview.set_view_bg(self.btn_bg.color())
            note = self._src.name if self._src else "synthetic"
            self.ctx.info(
                f"splash preview | {note} | fit={self._fit()} "
                f"size={self._content_size()} bg={_hex(self.btn_bg.color())}"
            )
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"splash preview error: {exc}")

    def _write(self) -> None:
        try:
            self._persist_splash_cfg()
            cfg = self.ctx.load_cfg()
            msg = skin_core.build_splash(
                self.ctx.out_dir,
                self._src,
                self._fit(),
                self._body(),
                cfg,
                self._bg(),
                self._content_size(),
            )
            self.ctx.info(msg)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"splash write error: {exc}")
