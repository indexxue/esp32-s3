"""Stub panels for product reserved paths (boot/anim deferred separately)."""

from __future__ import annotations

from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from app.base_feature import FeatureModule
from app.context import ProjectContext


class ReservedFeature(FeatureModule):
    status = "reserved"

    def __init__(
        self,
        ctx: ProjectContext,
        feature_id: str,
        title: str,
        rel_path: str,
        order: int,
    ) -> None:
        super().__init__(ctx)
        self.id = feature_id
        self.title = title
        self.subtitle = f"reserved | {rel_path}"
        self.order = order
        self.rel_path = rel_path

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        w = QWidget(parent)
        lay = QVBoxLayout(w)
        lab = QLabel(
            f"<b>{self.title}</b><br/>"
            f"Path: <code>{self.rel_path}</code><br/><br/>"
            "Product reserved: do not pre-create empty dirs on card; firmware ignores for now.<br/>"
            "To implement: add a real FeatureModule under app/features/, "
            "register it in registry.built_in_features(), write only via skin_core."
        )
        lab.setWordWrap(True)
        lay.addWidget(lab)
        lay.addStretch(1)
        return w
