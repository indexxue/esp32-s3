"""Horizontal filmstrip of round body-frame thumbnails."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import QHBoxLayout, QScrollArea, QSizePolicy, QWidget

_THUMB = 72
_PAD = 6


class _Thumb(QWidget):
    clicked = Signal(int)

    def __init__(self, index: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._index = index
        self._pm = QPixmap()
        self._current = False
        self._playing = False
        self.setFixedSize(_THUMB + _PAD * 2, _THUMB + 22)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set_image(self, image: QImage) -> None:
        side = _THUMB
        src = QPixmap.fromImage(image)
        out = QPixmap(side, side)
        out.fill(Qt.GlobalColor.transparent)
        painter = QPainter(out)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        path = QPainterPath()
        path.addEllipse(QRectF(1, 1, side - 2, side - 2))
        painter.setClipPath(path)
        painter.drawPixmap(0, 0, src.scaled(side, side, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation))
        painter.end()
        self._pm = out
        self.update()

    def set_current(self, on: bool) -> None:
        self._current = on
        self.update()

    def set_playing(self, on: bool) -> None:
        self._playing = on
        self.update()

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        x = (self.width() - _THUMB) // 2
        y = 2
        if not self._pm.isNull():
            p.drawPixmap(x, y, self._pm)
        if self._playing:
            pen = QPen(QColor("#7ec8ff"), 3)
        elif self._current:
            pen = QPen(QColor("#e8c547"), 3)
        else:
            pen = QPen(QColor("#3a4258"), 2)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(x + 1, y + 1, _THUMB - 2, _THUMB - 2)
        p.setPen(QColor("#eef2ff") if self._current else QColor("#8b93a7"))
        p.drawText(0, _THUMB + 4, self.width(), 16, Qt.AlignmentFlag.AlignHCenter, f"{self._index}")

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self._index)


class FrameStrip(QWidget):
    """Click a thumb to select that frame for editing."""

    frame_selected = Signal(int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._thumbs: list[_Thumb] = []
        self._inner = QWidget()
        self._row = QHBoxLayout(self._inner)
        self._row.setContentsMargins(0, 0, 0, 0)
        self._row.setSpacing(4)
        self._row.addStretch(1)
        self._scroll = QScrollArea()
        self._scroll.setWidget(self._inner)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFixedHeight(_THUMB + 36)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self._scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self._scroll)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_frames(
        self,
        images: list[QImage],
        current: int,
        playing: int | None = None,
    ) -> None:
        n = len(images)
        while len(self._thumbs) < n:
            i = len(self._thumbs)
            thumb = _Thumb(i)
            thumb.clicked.connect(self.frame_selected)
            self._thumbs.append(thumb)
            self._row.insertWidget(self._row.count() - 1, thumb)
        while len(self._thumbs) > n:
            w = self._thumbs.pop()
            self._row.removeWidget(w)
            w.deleteLater()
        for i, img in enumerate(images):
            t = self._thumbs[i]
            t._index = i
            t.set_image(img)
            t.set_current(i == current)
            t.set_playing(playing is not None and i == playing)
        self._inner.adjustSize()

    def update_thumb(self, index: int, image: QImage, current: int | None = None) -> None:
        if index < 0 or index >= len(self._thumbs):
            return
        self._thumbs[index].set_image(image)
        if current is not None:
            for i, t in enumerate(self._thumbs):
                t.set_current(i == current)

    def set_playing_index(self, playing: int | None) -> None:
        for i, t in enumerate(self._thumbs):
            t.set_playing(playing is not None and i == playing)
