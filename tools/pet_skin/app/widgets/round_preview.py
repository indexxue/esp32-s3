"""Round device preview: 240×240 canvas always fully visible (device scale)."""

from __future__ import annotations

from PySide6.QtCore import QRectF, QSize, Qt, QTimer
from PySide6.QtGui import QColor, QImage, QPainter, QPainterPath, QPen, QPixmap, QResizeEvent, QShowEvent
from PySide6.QtWidgets import (
    QGraphicsEllipseItem,
    QGraphicsPathItem,
    QGraphicsPixmapItem,
    QGraphicsScene,
    QGraphicsView,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

import skin_core

# Viewport around the 240×240 device canvas (2× after scale-to-fit).
_VIEW_PX = 480
# Firmware PET_SPLASH_ARC_SIZE = 132 on 240 canvas.
_ARC_DIAMETER = 132.0
_ARC_OFFSET_Y = -8.0


class RoundPreview(QWidget):
    """Shows the device 240×240 round screen; the full disk is always in view."""

    def __init__(
        self,
        parent: QWidget | None = None,
        *,
        view_px: int = _VIEW_PX,
        show_caption: bool = True,
    ) -> None:
        super().__init__(parent)
        self._view_px = max(160, int(view_px))
        self._arc_angle = 0.0
        self._show_arc = True
        self._arc_rect = QRectF()
        self._arc_diameter = _ARC_DIAMETER

        self._scene = QGraphicsScene(self)
        self._view = QGraphicsView()
        self._view.setScene(self._scene)
        self._view.setRenderHints(
            QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform
        )
        self._view.setDragMode(QGraphicsView.DragMode.NoDrag)
        self._view.setInteractive(False)
        self._view.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._view.setBackgroundBrush(QColor("#10141c"))
        self._view.setFixedSize(self._view_px, self._view_px)
        self._view.setFrameShape(QGraphicsView.Shape.NoFrame)
        self._view.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorViewCenter)
        self._view.setResizeAnchor(QGraphicsView.ViewportAnchor.AnchorViewCenter)

        self._pix_item = QGraphicsPixmapItem()
        self._scene.addItem(self._pix_item)

        self._bezel = QGraphicsEllipseItem()
        self._bezel.setPen(QPen(QColor("#3a4258"), 3))
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

        tip = QLabel("预览 = 设备圆屏 240×240（完整显示）")
        tip.setStyleSheet("color:#8b93a7;")
        tip.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        tip.setVisible(show_caption)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(4)
        lay.addWidget(self._view, 0, Qt.AlignmentFlag.AlignHCenter)
        lay.addWidget(tip)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        self._caption_h = 22 if show_caption else 0

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick_arc)
        self._timer.start(50)

    def sizeHint(self) -> QSize:  # noqa: N802
        return QSize(self._view_px, self._view_px + self._caption_h)

    def minimumSizeHint(self) -> QSize:  # noqa: N802
        return self.sizeHint()

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
        self._fit_view()

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
        self._scene.setSceneRect(QRectF(0, 0, out.width(), out.height()))
        self._fit_view()
        QTimer.singleShot(0, self._fit_view)

    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802
        super().showEvent(event)
        self._fit_view()

    def resizeEvent(self, event: QResizeEvent) -> None:  # noqa: N802
        super().resizeEvent(event)
        self._fit_view()

    def _fit_view(self) -> None:
        r = self._scene.sceneRect()
        if r.isEmpty() or r.width() < 1 or r.height() < 1:
            return
        vw = self._view.viewport().width()
        vh = self._view.viewport().height()
        if vw < 8 or vh < 8:
            return
        s = min(vw / r.width(), vh / r.height())
        self._view.resetTransform()
        self._view.scale(s, s)
        self._view.centerOn(r.center())

    def _layout_overlays(self) -> None:
        pm = self._pix_item.pixmap()
        w = pm.width() if not pm.isNull() else skin_core.SPLASH_SIZE
        h = pm.height() if not pm.isNull() else skin_core.SPLASH_SIZE
        inset = 1.5
        self._bezel.setRect(QRectF(inset, inset, w - inset * 2, h - inset * 2))
        cx = w * 0.5
        cy = h * 0.5 + _ARC_OFFSET_Y
        rad = self._arc_diameter * 0.5
        self._arc_rect = QRectF(cx - rad, cy - rad, rad * 2, rad * 2)
        self._arc_track.setRect(self._arc_rect)
        self._update_arc_path()

    def _update_arc_path(self) -> None:
        path = QPainterPath()
        path.arcMoveTo(self._arc_rect, self._arc_angle)
        path.arcTo(self._arc_rect, self._arc_angle, -270)
        self._arc_ind.setPath(path)

    def _tick_arc(self) -> None:
        if not self._show_arc or self._arc_rect.isNull():
            return
        self._arc_angle = (self._arc_angle + 8) % 360
        self._update_arc_path()
