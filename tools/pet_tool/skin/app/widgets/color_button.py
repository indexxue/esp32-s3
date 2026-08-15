"""Color swatch button shared by Splash / Design panels."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QColorDialog, QPushButton, QWidget


def hex_color(c: QColor) -> str:
    return f"#{c.red():02X}{c.green():02X}{c.blue():02X}"


def rgb_tuple(c: QColor) -> tuple[int, int, int]:
    return (c.red(), c.green(), c.blue())


class ColorButton(QPushButton):
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
        self.setText(hex_color(self._color))
        lum = 0.299 * self._color.red() + 0.587 * self._color.green() + 0.114 * self._color.blue()
        fg = "#10141c" if lum > 160 else "#eef2ff"
        self.setStyleSheet(
            f"QPushButton {{ background:{hex_color(self._color)}; color:{fg}; "
            f"border:1px solid #3a4258; border-radius:4px; padding:2px 8px; }}"
        )
