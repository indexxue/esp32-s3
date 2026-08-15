"""Body clip inventory — bind assets; edit frames in Design; write via Pack/Design."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext


class BodyFeature(FeatureModule):
    id = "body"
    title = "Body"
    subtitle = "clip inventory | bind assets/"
    status = "ready"
    order = 30

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return BodyPanel(self.ctx, parent)


class BodyPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        lay = QVBoxLayout(self)
        tip = QLabel(
            "Clip 清单。把 PNG 放进 assets/ 后点 Bind；缺文件的 clip 打包时用色块。\n"
            "改图/涂鸦请到 Design。固件只读 RGBH，设备上没有编辑器。"
        )
        tip.setWordWrap(True)
        tip.setStyleSheet("color:#8b93a7;")
        self.list = QListWidget()
        btn_bind = QPushButton("Bind assets/")
        btn_bind.clicked.connect(self._bind)
        btn_import = QPushButton("Import folder…")
        btn_import.clicked.connect(self._import_folder)
        btn_check = QPushButton("Check")
        btn_check.clicked.connect(self._check)
        row = QHBoxLayout()
        row.addWidget(btn_bind)
        row.addWidget(btn_import)
        row.addWidget(btn_check)
        row.addStretch(1)
        lay.addWidget(tip)
        lay.addLayout(row)
        lay.addWidget(self.list, 1)
        self._reload()
        ctx.cfg_changed.connect(lambda _: self._reload())

    def _reload(self) -> None:
        self.list.clear()
        try:
            cfg = self.ctx.load_cfg()
            warns = skin_core.check_pack(cfg, self.ctx.cfg_path)
            for clip in cfg.get("clips", []):
                cid = str(clip.get("id"))
                src = skin_core.clip_source_label(clip)
                if any(f"error: {cid}" in w for w in warns):
                    mark = "MISSING"
                elif any(f"warn: {cid}" in w for w in warns):
                    mark = "SYNTH"
                else:
                    mark = "OK"
                self.list.addItem(
                    f"{mark}  {cid}  frames={clip.get('frames')}  fps={clip.get('fps')}  "
                    f"src={src}"
                )
            splash = skin_core.splash_src_from_assets(self.ctx.cfg_path.parent)
            self.list.addItem(
                f"{'OK' if splash else 'MISSING'}  splash  "
                f"src={splash.name if splash else '(synthetic if written)'}"
            )
        except Exception as exc:  # noqa: BLE001
            self.list.addItem(f"error: {exc}")

    def _bind(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
            notes = skin_core.bind_assets(cfg, self.ctx.cfg_path.parent)
            for line in notes:
                self.ctx.info(line)
            path = skin_core.save_cfg(cfg, self.ctx.cfg_path)
            self.ctx.info(f"saved {path}")
            self.ctx.cfg_changed.emit(str(path))
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"body bind error: {exc}")

    def _import_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Import asset folder", "")
        if not path:
            return
        dest = self.ctx.cfg_path.parent / "assets"
        try:
            for line in skin_core.import_asset_folder(Path(path), dest):
                self.ctx.info(line)
            self._bind()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"body import error: {exc}")

    def _check(self) -> None:
        try:
            cfg = self.ctx.load_cfg()
            warns = skin_core.check_pack(cfg, self.ctx.cfg_path)
            if not warns:
                self.ctx.info("check: ok")
            else:
                self.ctx.info("check: issues")
                for w in warns:
                    self.ctx.info(f"  {w}")
            self._reload()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"body check error: {exc}")
