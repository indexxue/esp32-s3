"""Feature panel contract - add new skin tools by subclassing + registering."""

from __future__ import annotations

from abc import abstractmethod

from PySide6.QtWidgets import QWidget

from app.context import ProjectContext


class FeatureModule:
    """One sidebar entry / stacked page.

    Status:
      ready      - implements write path
      preview    - UI only, write later
      reserved   - product reserved path; panel stub
    """

    id: str = ""
    title: str = ""
    subtitle: str = ""
    status: str = "ready"  # ready | preview | reserved
    order: int = 100

    def __init__(self, ctx: ProjectContext) -> None:
        self.ctx = ctx

    @abstractmethod
    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        raise NotImplementedError
