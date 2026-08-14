# -*- coding: utf-8 -*-
"""Theme UI icons → theme/ui/{feed,play,sleep,chat}.bin (optional; letter fallback on device)."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QImage, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext
from app.widgets.color_button import ColorButton, hex_color, rgb_tuple


class ThemeFeature(FeatureModule):
    id = "theme"
    title = "Theme"
    subtitle = "theme/ui icons | F/P/S/C"
    status = "ready"
    order = 70

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return ThemePanel(self.ctx, parent)


class ThemePanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        self._loading = True
        self._previews: dict[str, QLabel] = {}
        self._paths: dict[str, QLabel] = {}
        self._bg_btns: dict[str, ColorButton] = {}
        self._mode_boxes: dict[str, QComboBox] = {}

        root = QVBoxLayout(self)
        tip = QLabel(
            "按键皮肤：透明 PNG 叠在<strong>背景色</strong>上，或选<strong>纯色</strong>不配图。"
            "缺图且非纯色时，固件显示字母 F/P/S/C。"
            f"<br/>落盘：<code>theme/ui/*.bin</code>（{skin_core.UI_ICON_SIZE}×{skin_core.UI_ICON_SIZE} RGBH）；"
            "设置写入 <code>pack.json → theme.ui</code>。"
        )
        tip.setWordWrap(True)
        tip.setTextFormat(Qt.TextFormat.RichText)
        root.addWidget(tip)

        presets = QHBoxLayout()
        presets.addWidget(QLabel("快捷："))
        for label, rgb in (
            ("Care灰", skin_core.UI_ICON_CARE_BG),
            ("Chat蓝", skin_core.UI_ICON_CHAT_BG),
            ("屏底", skin_core.BG),
            ("黑", (0, 0, 0)),
            ("白", (255, 255, 255)),
        ):
            b = QPushButton(label)
            b.clicked.connect(lambda _=False, c=rgb: self._apply_bg_preset(c))
            presets.addWidget(b)
        care_all = QPushButton("Care共用此色→F/P/S")
        care_all.clicked.connect(self._apply_care_shared)
        presets.addWidget(care_all)
        presets.addStretch(1)
        root.addLayout(presets)

        grid = QGridLayout()
        root.addLayout(grid)
        grid.addWidget(QLabel("键"), 0, 0)
        grid.addWidget(QLabel("预览"), 0, 1)
        grid.addWidget(QLabel("模式"), 0, 2)
        grid.addWidget(QLabel("背景/纯色"), 0, 3)
        grid.addWidget(QLabel("资源"), 0, 4)
        grid.addWidget(QLabel(""), 0, 5)

        for row, (bin_stem, assets_stem, default_bg) in enumerate(skin_core.UI_ICONS, start=1):
            title = QLabel(f"<b>{bin_stem}</b><br/><code>{assets_stem}</code>")
            title.setTextFormat(Qt.TextFormat.RichText)

            prev = QLabel()
            prev.setFixedSize(56, 56)
            prev.setAlignment(Qt.AlignmentFlag.AlignCenter)
            prev.setStyleSheet(
                "background:#2a2f3a; border:1px solid #3a4258; border-radius:28px;"
            )

            mode = QComboBox()
            mode.addItem("图片+背景", "image")
            mode.addItem("纯色", "solid")
            mode.currentIndexChanged.connect(self._on_edit)

            bg_btn = ColorButton(QColor(default_bg[0], default_bg[1], default_bg[2]))
            bg_btn.colorChanged.connect(lambda _c, s=assets_stem: self._on_edit())

            path_lab = QLabel("(none)")
            path_lab.setWordWrap(True)
            path_lab.setMinimumWidth(140)

            btn = QPushButton("Import…")
            btn.clicked.connect(lambda _=False, s=assets_stem: self._import_one(s))

            grid.addWidget(title, row, 0)
            grid.addWidget(prev, row, 1)
            grid.addWidget(mode, row, 2)
            grid.addWidget(bg_btn, row, 3)
            grid.addWidget(path_lab, row, 4)
            grid.addWidget(btn, row, 5)

            self._previews[assets_stem] = prev
            self._paths[assets_stem] = path_lab
            self._bg_btns[assets_stem] = bg_btn
            self._mode_boxes[assets_stem] = mode

        row = QHBoxLayout()
        refresh = QPushButton("Reload pack.json")
        refresh.clicked.connect(self.reload_from_cfg)
        build = QPushButton("Build theme/ui/*.bin")
        build.clicked.connect(self._build)
        row.addWidget(refresh)
        row.addWidget(build)
        row.addStretch(1)
        root.addLayout(row)
        root.addStretch(1)

        self._loading = False
        self.reload_from_cfg()

    def _apply_bg_preset(self, rgb: tuple[int, int, int]) -> None:
        self._loading = True
        c = QColor(rgb[0], rgb[1], rgb[2])
        for btn in self._bg_btns.values():
            btn.set_color(c)
        self._loading = False
        self._persist_and_refresh()

    def _apply_care_shared(self) -> None:
        """Copy feed bg to play/sleep (care buttons share one look)."""
        feed = self._bg_btns.get("ui_feed")
        if feed is None:
            return
        c = feed.color()
        self._loading = True
        for stem in ("ui_play", "ui_sleep"):
            if stem in self._bg_btns:
                self._bg_btns[stem].set_color(c)
        self._loading = False
        self._persist_and_refresh()

    def _on_edit(self, *_args) -> None:
        if self._loading:
            return
        self._persist_and_refresh()

    def _persist_and_refresh(self) -> None:
        cfg = self.ctx.load_cfg()
        for bin_stem, assets_stem, _ in skin_core.UI_ICONS:
            mode = self._mode_boxes[assets_stem].currentData()
            bg = rgb_tuple(self._bg_btns[assets_stem].color())
            skin_core.set_theme_ui_icon_cfg(cfg, bin_stem, str(mode), bg)
        skin_core.save_cfg(cfg, self.ctx.cfg_path)
        self.refresh_previews(cfg)

    def reload_from_cfg(self) -> None:
        cfg = self.ctx.load_cfg()
        self._loading = True
        for bin_stem, assets_stem, _ in skin_core.UI_ICONS:
            opt = skin_core.theme_ui_icon_cfg(cfg, bin_stem)
            bg = opt["bg"]
            self._bg_btns[assets_stem].set_color(QColor(bg[0], bg[1], bg[2]))
            box = self._mode_boxes[assets_stem]
            idx = box.findData(opt["mode"])
            box.setCurrentIndex(idx if idx >= 0 else 0)
        self._loading = False
        self.refresh_previews(cfg)

    def refresh_previews(self, cfg: dict | None = None) -> None:
        base = self.ctx.cfg_path.parent
        cfg = cfg if cfg is not None else self.ctx.load_cfg()
        for bin_stem, assets_stem, _ in skin_core.UI_ICONS:
            opt = skin_core.theme_ui_icon_cfg(cfg, bin_stem)
            mode = opt["mode"]
            bg = opt["bg"]
            lab = self._paths[assets_stem]
            prev = self._previews[assets_stem]
            src = skin_core.find_ui_icon_src(base, assets_stem)

            if mode == "solid":
                lab.setText(f"纯色 {hex_color(QColor(bg[0], bg[1], bg[2]))}")
            elif src is None:
                lab.setText("(无图 — 字母 fallback；可改纯色)")
            else:
                try:
                    lab.setText(str(src.relative_to(base)))
                except ValueError:
                    lab.setText(str(src))

            try:
                pix = skin_core.preview_ui_icon_pixels(
                    base, bin_stem, assets_stem, mode, bg
                )
                rgb = skin_core.rgb565_to_qimage_bytes(
                    pix, skin_core.UI_ICON_SIZE, skin_core.UI_ICON_SIZE
                )
                img = QImage(
                    rgb,
                    skin_core.UI_ICON_SIZE,
                    skin_core.UI_ICON_SIZE,
                    skin_core.UI_ICON_SIZE * 3,
                    QImage.Format.Format_RGB888,
                ).copy()
                prev.setPixmap(
                    QPixmap.fromImage(img).scaled(
                        48,
                        48,
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )
                prev.setText("")
                prev.setStyleSheet(
                    f"background:{hex_color(QColor(bg[0], bg[1], bg[2]))}; "
                    "border:1px solid #3a4258; border-radius:28px;"
                )
            except Exception as exc:  # noqa: BLE001
                prev.setText("err")
                lab.setText(str(exc))

    def _import_one(self, assets_stem: str) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            f"Import {assets_stem}",
            str(self.ctx.cfg_path.parent / "assets"),
            "Images (*.png *.webp *.jpg *.jpeg *.bmp)",
        )
        if not path:
            return
        src = Path(path)
        dest_dir = self.ctx.cfg_path.parent / "assets"
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / f"{assets_stem}{src.suffix.lower()}"
        for old in dest_dir.glob(f"{assets_stem}.*"):
            if old != dest and old.suffix.lower() in skin_core.IMAGE_EXTS:
                try:
                    old.unlink()
                except OSError:
                    pass
        dest.write_bytes(src.read_bytes())
        # Prefer image mode after import
        box = self._mode_boxes[assets_stem]
        idx = box.findData("image")
        if idx >= 0:
            self._loading = True
            box.setCurrentIndex(idx)
            self._loading = False
        self.ctx.info(f"copied → {dest.relative_to(self.ctx.cfg_path.parent)}")
        self._persist_and_refresh()

    def _build(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
            for bin_stem, assets_stem, _ in skin_core.UI_ICONS:
                mode = self._mode_boxes[assets_stem].currentData()
                bg = rgb_tuple(self._bg_btns[assets_stem].color())
                skin_core.set_theme_ui_icon_cfg(cfg, bin_stem, str(mode), bg)
            skin_core.save_cfg(cfg, self.ctx.cfg_path)
            msg = skin_core.build_theme_ui_icons(
                self.ctx.out_dir, self.ctx.cfg_path.parent, cfg
            )
            self.ctx.info(msg)
            self.refresh_previews(cfg)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"theme UI build error: {exc}")
