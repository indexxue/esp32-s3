"""Round device preview: fixed 240 canvas frame; zoom for inspect only."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QImage, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import (
    QGraphicsEllipseItem,
    QGraphicsPathItem,
    QGraphicsPixmapItem,
    QGraphicsScene,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QSlider,
    QVBoxLayout,
    QWidget,
)

import skin_core

# Fixed Qt viewport around the 240×240 device canvas (2× for comfort).
_VIEW_PX = 480
# Firmware PET_SPLASH_ARC_SIZE = 132 on 240 canvas.
_ARC_DIAMETER = 132.0
_ARC_OFFSET_Y = -8.0


class _ZoomView(QGraphicsView):
    def __init__(self, owner: "RoundPreview") -> None:
        super().__init__()
        self._owner = owner

    def wheelEvent(self, event) -> None:  # noqa: N802
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            step = 10 if event.angleDelta().y() > 0 else -10
            z = self._owner._zoom
            z.setValue(max(z.minimum(), min(z.maximum(), z.value() + step)))
            event.accept()
            return
        super().wheelEvent(event)


class RoundPreview(QWidget):
    """Shows a fixed 240×240 splash with circular clip; content size is authored left."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._arc_angle = 0.0
        self._show_arc = True
        self._arc_rect = QRectF()
        self._arc_diameter = _ARC_DIAMETER

        self._scene = QGraphicsScene(self)
        self._view = _ZoomView(self)
        self._view.setScene(self._scene)
        self._view.setRenderHints(
            QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform
        )
        self._view.setDragMode(QGraphicsView.DragMode.ScrollHandDrag)
        self._view.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self._view.setBackgroundBrush(QColor("#10141c"))
        self._view.setFixedSize(_VIEW_PX, _VIEW_PX)

        self._pix_item = QGraphicsPixmapItem()
        self._scene.addItem(self._pix_item)

        self._bezel = QGraphicsEllipseItem()
        self._bezel.setPen(QPen(QColor("#3a4258"), 10))
        self._bezel.setBrush(Qt.BrushStyle.NoBrush)
        self._scene.addItem(self._bezel)

        self._arc_track = QGraphicsEllipseItem()
        self._arc_track.setPen(QPen(QColor("#2a3148"), 4))
        self._arc_track.setBrush(Qt.BrushStyle.NoBrush)
        self._scene.addItem(self._arc_track)

        self._arc_ind = QGraphicsPathItem()
        self._arc_ind.setPen(QPen(QColor("#7ec8ff"), 4, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        self._arc_ind.setBrush(Qt.BrushStyle.NoBrush)
        self._scene.addItem(self._arc_ind)

        self._zoom = QSlider(Qt.Orientation.Horizontal)
        self._zoom.setRange(50, 400)
        self._zoom.setValue(200)
        self._zoom.valueChanged.connect(self._apply_zoom)
        self._zoom_lbl = QLabel("200%")

        zoom_row = QHBoxLayout()
        zoom_row.addWidget(QLabel("缩放"))
        zoom_row.addWidget(self._zoom, 1)
        zoom_row.addWidget(self._zoom_lbl)

        tip = QLabel("画布固定 240×240 · Ctrl+滚轮缩放 · 拖拽平移")
        tip.setStyleSheet("color:#8b93a7;")

        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self._view, 0, Qt.AlignmentFlag.AlignHCenter)
        lay.addWidget(tip)
        lay.addLayout(zoom_row)
        lay.addStretch(1)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick_arc)
        self._timer.start(50)

        self._apply_zoom(self._zoom.value())

    def set_show_arc(self, on: bool) -> None:
        self._show_arc = on
        self._arc_track.setVisible(on)
        self._arc_ind.setVisible(on)

    def set_view_bg(self, color: QColor) -> None:
        self._view.setBackgroundBrush(color)

    def set_arc_colors(self, track: QColor, indicator: QColor) -> None:
        tw = self._arc_track.pen().widthF() or 4.0
        iw = self._arc_ind.pen().widthF() or 4.0
        self._arc_track.setPen(QPen(track, tw))
        self._arc_ind.setPen(QPen(indicator, iw, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))

    def set_arc_size(self, diameter: float) -> None:
        self._arc_diameter = max(24.0, float(diameter))
        self._layout_overlays()

    def set_rgb565(
        self,
        pixels: bytes,
        w: int = skin_core.SPLASH_SIZE,
        h: int = skin_core.SPLASH_SIZE,
    ) -> None:
        rgb = skin_core.rgb565_to_qimage_bytes(pixels, w, h)
        img = QImage(rgb, w, h, w * 3, QImage.Format.Format_RGB888).copy()
        self.set_image(img)

    def set_image(self, image: QImage) -> None:
        pm = QPixmap.fromImage(image)
        out = QPixmap(pm.size())
        out.fill(Qt.GlobalColor.transparent)
        painter = QPainter(out)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        path = QPainterPath()
        path.addEllipse(QRectF(0, 0, pm.width(), pm.height()))
        painter.setClipPath(path)
        painter.drawPixmap(0, 0, pm)
        painter.end()
        self._pix_item.setPixmap(out)
        self._layout_overlays()
        self._scene.setSceneRect(QRectF(-24, -24, out.width() + 48, out.height() + 48))

    def _layout_overlays(self) -> None:
        pm = self._pix_item.pixmap()
        w = pm.width() if not pm.isNull() else skin_core.SPLASH_SIZE
        h = pm.height() if not pm.isNull() else skin_core.SPLASH_SIZE
        pad = 8
        self._bezel.setRect(QRectF(-pad, -pad, w + pad * 2, h + pad * 2))
        cx = w * 0.5
        cy = h * 0.5 + _ARC_OFFSET_Y
        r = self._arc_diameter * 0.5
        self._arc_rect = QRectF(cx - r, cy - r, r * 2, r * 2)
        self._arc_track.setRect(self._arc_rect)
        self._update_arc_path()

    def _update_arc_path(self) -> None:
        path = QPainterPath()
        # Qt arc angles: 0° at 3 o'clock, counter-clockwise
        path.arcMoveTo(self._arc_rect, self._arc_angle)
        path.arcTo(self._arc_rect, self._arc_angle, -270)
        self._arc_ind.setPath(path)

    def _tick_arc(self) -> None:
        if not self._show_arc or self._arc_rect.isNull():
            return
        self._arc_angle = (self._arc_angle + 8) % 360
        self._update_arc_path()

    def _apply_zoom(self, percent: int) -> None:
        self._zoom_lbl.setText(f"{percent}%")
        scale = percent / 100.0
        self._view.resetTransform()
        self._view.scale(scale, scale)
