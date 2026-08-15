"""Main window: sidebar feature list + stacked panels + log."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QSplitter,
    QStackedWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app.context import ProjectContext
from app.registry import built_in_features


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("pet_skin")
        self.resize(1400, 840)

        self.ctx = ProjectContext(self)
        self.features = built_in_features(self.ctx)

        self.nav = QListWidget()
        self.nav.setFixedWidth(200)
        self.stack = QStackedWidget()
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(140)
        font = QFont("Consolas")
        font.setStyleHint(QFont.StyleHint.Monospace)
        self.log.setFont(font)

        for feat in self.features:
            item = QListWidgetItem(feat.title)
            badge = {"ready": "", "preview": " [preview]", "reserved": " [reserved]"}
            item.setToolTip(feat.subtitle + badge.get(feat.status, ""))
            if feat.status == "reserved":
                item.setForeground(Qt.GlobalColor.gray)
            elif feat.status == "preview":
                item.setForeground(Qt.GlobalColor.darkYellow)
            self.nav.addItem(item)
            self.stack.addWidget(feat.create_widget())

        self.nav.currentRowChanged.connect(self.stack.setCurrentIndex)
        self.nav.setCurrentRow(0)
        self.ctx.log.connect(self._on_log)

        header = QLabel("pet_skin | write via skin_core only | extend FeatureModule in registry")
        header.setStyleSheet("color:#8b93a7; padding:4px;")

        right = QVBoxLayout()
        right.addWidget(self.stack, 1)
        right.addWidget(QLabel("Log"))
        right.addWidget(self.log)
        right_w = QWidget()
        right_w.setLayout(right)

        split = QSplitter()
        split.addWidget(self.nav)
        split.addWidget(right_w)
        split.setStretchFactor(1, 1)

        central = QWidget()
        lay = QVBoxLayout(central)
        lay.addWidget(header)
        lay.addWidget(split, 1)
        self.setCentralWidget(central)

        self.ctx.info(
            "ready | bind assets/ → pack.bin | splash=static | face overlay reserved"
        )

    def _on_log(self, msg: str) -> None:
        self.log.append(msg)
