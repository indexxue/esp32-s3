"""Circular body doodle canvas for authoring pet body frames."""

from __future__ import annotations

from PySide6.QtCore import QPoint, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import QWidget

import skin_core


class BodyPaintCanvas(QWidget):
    """Paint inside a circular clip; size matches body frame (≤180).

    Canvas is ARGB: transparent pixels stay alpha=0 so packing can fill
    screen BG (#202020). RGB32 flattening would bake black and show a halo.
    """

    changed = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._size = skin_core.BODY_SIZE_DEFAULT
        self._brush = QColor(74, 163, 200)
        self._brush_r = 6
        self._erase = False
        self._bg = QColor(*skin_core.BG)
        self._img = self._blank()
        self._drawing = False
        self._last: QPoint | None = None
        self.setFixedSize(220, 220)
        self.setMouseTracking(True)

    def _blank(self) -> QImage:
        img = QImage(self._size, self._size, QImage.Format.Format_ARGB32)
        img.fill(Qt.GlobalColor.transparent)
        return img

    def body_size(self) -> int:
        return self._size

    def set_body_size(self, size: int) -> None:
        size = skin_core.clamp_body_size(size)
        if size == self._size:
            return
        scaled = self._img.scaled(
            size,
            size,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self._size = size
        self._img = self._blank()
        p = QPainter(self._img)
        p.drawImage(0, 0, scaled)
        p.end()
        self.update()
        self.changed.emit()

    def set_brush_color(self, color: QColor) -> None:
        self._brush = QColor(color)
        self._brush.setAlpha(255)

    def set_brush_radius(self, r: int) -> None:
        self._brush_r = max(1, min(32, int(r)))

    def set_erase(self, on: bool) -> None:
        self._erase = on

    def clear(self) -> None:
        self._img.fill(Qt.GlobalColor.transparent)
        self.update()
        self.changed.emit()

    def fill_disk(self, color: QColor) -> None:
        """Synthetic body starter: opaque disk, transparent outside."""
        self._img.fill(Qt.GlobalColor.transparent)
        p = QPainter(self._img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        fill = QColor(color)
        fill.setAlpha(255)
        p.setBrush(fill)
        p.setPen(Qt.PenStyle.NoPen)
        margin = int(self._size * 0.08)
        p.drawEllipse(margin, margin, self._size - 2 * margin, self._size - 2 * margin)
        p.end()
        self.update()
        self.changed.emit()

    def load_qimage(self, image: QImage) -> None:
        scaled = image.convertToFormat(QImage.Format.Format_ARGB32).scaled(
            self._size,
            self._size,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self._img.fill(Qt.GlobalColor.transparent)
        ox = (self._size - scaled.width()) // 2
        oy = (self._size - scaled.height()) // 2
        p = QPainter(self._img)
        p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
        p.drawImage(ox, oy, scaled)
        p.end()
        self.update()
        self.changed.emit()

    def to_qimage(self) -> QImage:
        return self._img.copy()

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor("#10141c"))
        side = min(self.width(), self.height()) - 8
        x = (self.width() - side) // 2
        y = (self.height() - side) // 2
        pm = QPixmap.fromImage(self._img).scaled(
            side,
            side,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        path = QPainterPath()
        path.addEllipse(QRectF(x, y, side, side))
        painter.setPen(QPen(QColor("#3a4258"), 2))
        painter.drawEllipse(x, y, side, side)
        painter.setClipPath(path)
        painter.drawPixmap(x, y, pm)

    def _map_pos(self, pos: QPoint) -> QPoint | None:
        side = min(self.width(), self.height()) - 8
        x0 = (self.width() - side) // 2
        y0 = (self.height() - side) // 2
        lx = pos.x() - x0
        ly = pos.y() - y0
        if lx < 0 or ly < 0 or lx >= side or ly >= side:
            return None
        sx = int(lx * self._size / side)
        sy = int(ly * self._size / side)
        return QPoint(max(0, min(self._size - 1, sx)), max(0, min(self._size - 1, sy)))

    def _stroke(self, a: QPoint, b: QPoint) -> None:
        p = QPainter(self._img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._erase:
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
            color = QColor(0, 0, 0, 0)
        else:
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
            color = QColor(self._brush)
            color.setAlpha(255)
        pen = QPen(
            color,
            self._brush_r * 2,
            Qt.PenStyle.SolidLine,
            Qt.PenCapStyle.RoundCap,
            Qt.PenJoinStyle.RoundJoin,
        )
        p.setPen(pen)
        p.drawLine(a, b)
        p.end()
        self.update()
        self.changed.emit()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pt = self._map_pos(event.position().toPoint())
        if pt is None:
            return
        self._drawing = True
        self._last = pt
        self._stroke(pt, pt)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if not self._drawing or self._last is None:
            return
        pt = self._map_pos(event.position().toPoint())
        if pt is None:
            return
        self._stroke(self._last, pt)
        self._last = pt

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._drawing = False
            self._last = None
