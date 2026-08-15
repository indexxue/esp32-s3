"""Pack.bin builder feature."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import (
    QCheckBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext


class PackFeature(FeatureModule):
    id = "pack"
    title = "Pack"
    subtitle = "pack.bin + body/*.bin"
    status = "ready"
    order = 20

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return PackPanel(self.ctx, parent)


class PackPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx

        self.cfg_edit = QLineEdit(str(ctx.cfg_path))
        self.out_edit = QLineEdit(str(ctx.out_dir))
        self.summary = QTextEdit()
        self.summary.setReadOnly(True)

        btn_cfg = QPushButton("...")
        btn_cfg.clicked.connect(self._browse_cfg)
        btn_out = QPushButton("...")
        btn_out.clicked.connect(self._browse_out)
        btn_reload = QPushButton("Reload JSON")
        btn_reload.clicked.connect(self._reload)
        btn_bind = QPushButton("Bind assets/")
        btn_bind.clicked.connect(self._bind)
        btn_check = QPushButton("Check")
        btn_check.clicked.connect(self._check)
        btn_build = QPushButton("Build pack.bin")
        btn_build.clicked.connect(self._build)
        btn_both = QPushButton("Build pack + splash + UI icons")
        btn_both.clicked.connect(self._build_both)
        btn_zip = QPushButton("Export zip…")
        btn_zip.clicked.connect(self._export_zip)

        lay = QVBoxLayout(self)
        lay.addWidget(QLabel("pack.json"))
        row1 = QHBoxLayout()
        row1.addWidget(self.cfg_edit, 1)
        row1.addWidget(btn_cfg)
        lay.addLayout(row1)

        lay.addWidget(QLabel("Output pet/"))
        row2 = QHBoxLayout()
        row2.addWidget(self.out_edit, 1)
        row2.addWidget(btn_out)
        lay.addLayout(row2)

        row3 = QHBoxLayout()
        row3.addWidget(btn_reload)
        row3.addWidget(btn_bind)
        row3.addWidget(btn_check)
        row3.addWidget(btn_build)
        row3.addWidget(btn_both)
        row3.addWidget(btn_zip)
        row3.addStretch(1)
        lay.addLayout(row3)
        self.chk_font = QCheckBox("Include font/caption.bin in zip")
        self.chk_font.setChecked(True)
        self.chk_font.setToolTip(
            "Uncheck if caption.bin exceeds device WEB_SKIN_FILE_MAX "
            "(device keeps existing pet/font)."
        )
        lay.addWidget(self.chk_font)
        lay.addWidget(QLabel("Summary"))
        lay.addWidget(self.summary, 1)

        self._reload()

    def _sync_ctx(self) -> None:
        self.ctx.cfg_path = Path(self.cfg_edit.text().strip())
        self.ctx.out_dir = Path(self.out_edit.text().strip())

    def _browse_cfg(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "pack.json", self.cfg_edit.text(), "JSON (*.json)"
        )
        if path:
            self.cfg_edit.setText(path)
            self._reload()

    def _browse_out(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Output pet/", self.out_edit.text())
        if path:
            self.out_edit.setText(path)
            self._sync_ctx()

    def _reload(self) -> None:
        try:
            self._sync_ctx()
            cfg = self.ctx.load_cfg()
            clips = cfg.get("clips", [])
            warns = skin_core.check_pack(cfg, self.ctx.cfg_path)
            lines = [
                f"version={cfg.get('version')}  body={cfg.get('width')}x{cfg.get('height')}",
                f"clips={len(clips)}: " + ", ".join(c.get("id", "?") for c in clips),
                f"needs={cfg.get('needs')}",
                "",
                "declared: " + ", ".join(skin_core.DECLARED_PATHS),
                "reserved: " + ", ".join(skin_core.RESERVED_PATHS),
                "",
                "check: ok" if not warns else "check:",
            ]
            lines.extend(f"  {w}" for w in warns)
            self.summary.setPlainText("\n".join(lines))
            self.ctx.info(f"loaded {self.ctx.cfg_path}")
        except Exception as exc:  # noqa: BLE001
            self.summary.setPlainText(str(exc))
            self.ctx.info(f"pack reload error: {exc}")

    def _bind(self) -> None:
        try:
            self._sync_ctx()
            cfg = self.ctx.load_cfg()
            notes = skin_core.bind_assets(cfg, self.ctx.cfg_path.parent)
            for line in notes:
                self.ctx.info(line)
            path = skin_core.save_cfg(cfg, self.ctx.cfg_path)
            self.ctx.info(f"saved {path}")
            self._reload()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"pack bind error: {exc}")

    def _check(self) -> None:
        try:
            self._sync_ctx()
            self._reload()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"pack check error: {exc}")

    def _build(self) -> None:
        try:
            self._sync_ctx()
            cfg = self.ctx.load_cfg()
            msg = skin_core.build_pack(cfg, self.ctx.out_dir, self.ctx.cfg_path)
            self.ctx.info(msg)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"pack build error: {exc}")

    def _build_both(self) -> None:
        try:
            self._sync_ctx()
            cfg = self.ctx.load_cfg()
            self.ctx.info(skin_core.build_pack(cfg, self.ctx.out_dir, self.ctx.cfg_path))
            self.ctx.info(
                skin_core.build_splash_from_cfg(self.ctx.out_dir, cfg, self.ctx.cfg_path.parent)
            )
            self.ctx.info(
                skin_core.build_theme_ui_icons(self.ctx.out_dir, self.ctx.cfg_path.parent, cfg)
            )
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"pack+splash error: {exc}")

    def _export_zip(self) -> None:
        try:
            self._sync_ctx()
            default = str(self.ctx.out_dir.parent / "pet.zip")
            path, _ = QFileDialog.getSaveFileName(
                self, "Export skin zip", default, "ZIP (*.zip)"
            )
            if not path:
                return
            z, note = skin_core.export_skin_zip(
                self.ctx.out_dir,
                Path(path),
                include_font=self.chk_font.isChecked(),
            )
            self.ctx.info(
                f"wrote {z} — {note}; upload on device web, board reboots "
                "(existing SD font kept if zip has none)"
            )
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"export zip error: {exc}")
            self.summary.setPlainText(str(exc))
