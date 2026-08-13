"""Body clip list — sources shown; edit in Design, write via Pack/Design."""

from __future__ import annotations

from PySide6.QtWidgets import QLabel, QListWidget, QVBoxLayout, QWidget

from app.base_feature import FeatureModule
from app.context import ProjectContext


class BodyFeature(FeatureModule):
    id = "body"
    title = "Body"
    subtitle = "body/*.bin list | design in Design panel"
    status = "preview"
    order = 30

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return BodyPanel(self.ctx, parent)


class BodyPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        lay = QVBoxLayout(self)
        tip = QLabel(
            "Clip inventory. Author body in Design (doodle / PNG / color),\n"
            "then Build pack.bin. Firmware reads RGBH only — no on-device editor."
        )
        tip.setWordWrap(True)
        tip.setStyleSheet("color:#8b93a7;")
        self.list = QListWidget()
        lay.addWidget(tip)
        lay.addWidget(self.list, 1)
        self._reload()
        ctx.cfg_changed.connect(lambda _: self._reload())

    def _reload(self) -> None:
        self.list.clear()
        try:
            cfg = self.ctx.load_cfg()
            for clip in cfg.get("clips", []):
                src = clip.get("source") or (
                    ",".join(clip.get("sources") or []) if clip.get("sources") else "-"
                )
                self.list.addItem(
                    f"{clip.get('id')}  frames={clip.get('frames')}  fps={clip.get('fps')}  "
                    f"color={clip.get('color')}  src={src}"
                )
        except Exception as exc:  # noqa: BLE001
            self.list.addItem(f"error: {exc}")
