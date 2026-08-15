"""Project paths shared by all feature panels."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QObject, Signal

import skin_core


class ProjectContext(QObject):
    cfg_changed = Signal(str)
    out_changed = Signal(str)
    log = Signal(str)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._cfg = Path(skin_core.DEFAULT_CFG)
        self._out = Path(skin_core.DEFAULT_OUT)

    @property
    def cfg_path(self) -> Path:
        return self._cfg

    @cfg_path.setter
    def cfg_path(self, value: Path) -> None:
        self._cfg = Path(value)
        self.cfg_changed.emit(str(self._cfg))

    @property
    def out_dir(self) -> Path:
        return self._out

    @out_dir.setter
    def out_dir(self, value: Path) -> None:
        self._out = Path(value)
        self.out_changed.emit(str(self._out))

    def load_cfg(self) -> dict:
        return skin_core.load_cfg(self._cfg)

    def info(self, msg: str) -> None:
        self.log.emit(msg.rstrip())
