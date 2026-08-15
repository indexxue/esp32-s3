# -*- coding: utf-8 -*-
"""Pet Design — author body color / doodle / PNG per clip; write via skin_core."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QBuffer, QByteArray, QIODevice, Qt, QTimer
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QRadioButton,
    QScrollArea,
    QSlider,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

import struct

import skin_core
from app.base_feature import FeatureModule
from app.context import ProjectContext
from app.widgets.body_paint import BodyPaintCanvas
from app.widgets.color_button import ColorButton, hex_color, rgb_tuple
from app.widgets.frame_strip import FrameStrip
from app.widgets.round_preview import RoundPreview


class DesignFeature(FeatureModule):
    id = "design"
    title = "Design"
    subtitle = "PNG body → pack.bin | face anchors reserved"
    status = "ready"
    order = 15

    def create_widget(self, parent: QWidget | None = None) -> QWidget:
        return DesignPanel(self.ctx, parent)


class DesignPanel(QWidget):
    def __init__(self, ctx: ProjectContext, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ctx = ctx
        self._cfg: dict = {}
        self._clip_id = "idle"
        self._frame_i = 0
        self._face_part = "eye_l"

        self.clip_combo = QComboBox()
        self.clip_combo.currentTextChanged.connect(self._on_clip)

        self.size_slider = QSlider(Qt.Orientation.Horizontal)
        self.size_slider.setRange(skin_core.BODY_SIZE_MIN, skin_core.BODY_SIZE_MAX)
        self.size_slider.setValue(skin_core.BODY_SIZE_DEFAULT)
        self.size_lbl = QLabel(f"{skin_core.BODY_SIZE_DEFAULT}px")
        self.size_slider.valueChanged.connect(self._on_size)

        self.btn_color = ColorButton(QColor(74, 163, 200))
        self.btn_color.colorChanged.connect(self._on_color)
        self.btn_brush = ColorButton(QColor(74, 163, 200))
        self.btn_brush.colorChanged.connect(
            lambda c: self.canvas.set_brush_color(c)
        )

        self.brush_slider = QSlider(Qt.Orientation.Horizontal)
        self.brush_slider.setRange(1, 24)
        self.brush_slider.setValue(6)
        self.brush_lbl = QLabel("6px")
        self.brush_slider.valueChanged.connect(self._on_brush)

        self.chk_erase = QCheckBox("Eraser")

        self.fit_contain = QRadioButton("contain")
        self.fit_cover = QRadioButton("cover")
        self.fit_contain.setChecked(True)
        fit_g = QButtonGroup(self)
        fit_g.addButton(self.fit_contain)
        fit_g.addButton(self.fit_cover)

        self.chk_face = QCheckBox("Preview LVGL face overlay (firmware currently off)")
        self.chk_face.setChecked(False)
        self.chk_face.toggled.connect(lambda _: self.refresh_preview())

        self.frame_spin = QSpinBox()
        self.frame_spin.setMinimum(0)
        self.frame_spin.valueChanged.connect(self._on_frame)

        self.fps_spin = QSpinBox()
        self.fps_spin.setRange(1, 12)
        self.fps_spin.setValue(4)
        self.fps_spin.valueChanged.connect(self._on_fps)

        self.chk_all_frames = QCheckBox("应用到全部帧（静图，设备不闪）")
        self.chk_all_frames.setChecked(False)
        self.anim_lbl = QLabel()
        self.anim_lbl.setWordWrap(True)
        self.anim_lbl.setStyleSheet("color:#c9a227;")

        self.frames_spin = QSpinBox()
        self.frames_spin.setRange(1, skin_core.CLIP_FRAMES_MAX)
        self.frames_spin.setValue(1)
        self.frames_spin.valueChanged.connect(self._on_frames_count)

        self.chk_play = QCheckBox("循环预览")
        self.chk_play.setChecked(True)
        self.chk_play.toggled.connect(self._sync_play_timer)
        self.play_lbl = QLabel("帧 0/1")
        self.play_lbl.setStyleSheet("color:#8b93a7;")

        self._play_i = 0
        self._play_timer = QTimer(self)
        self._play_timer.timeout.connect(self._tick_play)

        self.part_combo = QComboBox()
        for p in skin_core.FACE_PARTS:
            self.part_combo.addItem(p)
        self.part_combo.currentTextChanged.connect(self._on_part)

        self.face_x = QSpinBox()
        self.face_y = QSpinBox()
        self.face_angle = QSpinBox()
        for sp in (self.face_x, self.face_y):
            sp.setRange(-40, 220)
        self.face_angle.setRange(-180, 180)
        self.face_x.valueChanged.connect(self._on_face_spin)
        self.face_y.valueChanged.connect(self._on_face_spin)
        self.face_angle.valueChanged.connect(self._on_face_spin)

        btn_face_default = QPushButton("Reset face defaults")
        btn_face_default.clicked.connect(self._face_defaults)
        btn_face_copy = QPushButton("Copy face → all frames")
        btn_face_copy.clicked.connect(self._face_copy_all_frames)

        self.src_lbl = QLabel("source: (synthetic color)")
        self.src_lbl.setStyleSheet("color:#8b93a7;")
        self.src_lbl.setWordWrap(True)

        self.canvas = BodyPaintCanvas()
        self.canvas.changed.connect(self.refresh_preview)
        self.chk_erase.toggled.connect(self.canvas.set_erase)

        self.preview = RoundPreview(view_px=300, show_caption=True)
        self.preview.set_show_arc(False)
        self.strip = FrameStrip()
        self.strip.frame_selected.connect(self._on_strip_select)

        # --- actions ---
        btn_disk = QPushButton("Fill disk")
        btn_disk.clicked.connect(self._fill_disk)
        btn_clear = QPushButton("Clear canvas")
        btn_clear.clicked.connect(self.canvas.clear)
        btn_import = QPushButton("Import PNG…")
        btn_import.clicked.connect(self._import_png)
        btn_apply = QPushButton("Apply doodle → clip")
        btn_apply.clicked.connect(self._apply_doodle)
        btn_clear_src = QPushButton("Use color only")
        btn_clear_src.clicked.connect(self._clear_source)
        btn_apply_all = QPushButton("Color → all clips")
        btn_apply_all.clicked.connect(self._color_all)
        btn_bind = QPushButton("Bind assets/")
        btn_bind.clicked.connect(self._bind_assets)
        btn_import_dir = QPushButton("Import folder…")
        btn_import_dir.clicked.connect(self._import_folder)
        btn_save = QPushButton("Save pack.json")
        btn_save.clicked.connect(self._save_json)
        btn_build = QPushButton("Build pack.bin")
        btn_build.clicked.connect(self._build)
        btn_both = QPushButton("Build pack + splash")
        btn_both.clicked.connect(self._build_both)
        btn_dup = QPushButton("复制帧")
        btn_dup.clicked.connect(self._dup_frame)
        btn_del = QPushButton("删除帧")
        btn_del.clicked.connect(self._del_frame)

        help_lbl = QLabel(
            "<b>怎么用</b>"
            "<p>每个动作（clip）最多 <b>8 帧</b>，设备按 fps 循环播放。"
            "idle 默认 2 帧，就是呼吸。</p>"
            "<b>1. 选动作</b><br/>idle 呼吸 · eat 吃 · play 玩 · 其余多为单帧。<br/><br/>"
            "<b>2. 设帧数</b><br/>用「帧数」或「复制帧 / 删除帧」。"
            "新帧会复制上一帧，再单独改差别。<br/><br/>"
            "<b>3. 点右侧缩略图选中要改的帧</b><br/>"
            "再 Import PNG，或圆内涂鸦后 Apply。默认<b>只改当前帧</b>。<br/><br/>"
            "<b>4. 看循环预览</b><br/>"
            "右侧圆屏按 fps 切帧，和设备一样。两帧差太大就会闪。<br/><br/>"
            "<b>5. 要静图、不闪</b><br/>"
            "勾选「应用到全部帧」再 Import / Apply。<br/><br/>"
            "<b>6. 拷到设备</b><br/>"
            "Import / Apply 会立刻写 pack.bin。把输出目录的 <code>pet/</code> 整份拷到 SD。"
        )
        help_lbl.setWordWrap(True)
        help_lbl.setTextFormat(Qt.TextFormat.RichText)
        help_lbl.setStyleSheet("color:#c5cde0; padding:4px;")
        help_inner = QWidget()
        help_lay = QVBoxLayout(help_inner)
        help_lay.setContentsMargins(10, 10, 10, 10)
        title = QLabel("使用说明")
        title.setStyleSheet("font-weight:600; color:#eef2ff; font-size:14px;")
        help_lay.addWidget(title)
        help_lay.addWidget(help_lbl)
        help_lay.addStretch(1)
        help_scroll = QScrollArea()
        help_scroll.setWidget(help_inner)
        help_scroll.setWidgetResizable(True)
        help_scroll.setFrameShape(QFrame.Shape.NoFrame)
        help_scroll.setMinimumWidth(220)
        help_scroll.setMaximumWidth(280)

        form = QVBoxLayout()
        form.setContentsMargins(8, 8, 8, 8)

        clip_row = QHBoxLayout()
        clip_row.addWidget(QLabel("动作"))
        clip_row.addWidget(self.clip_combo, 1)
        clip_row.addWidget(QLabel("fps"))
        clip_row.addWidget(self.fps_spin)
        form.addLayout(clip_row)

        fr_row = QHBoxLayout()
        fr_row.addWidget(QLabel("帧数"))
        fr_row.addWidget(self.frames_spin)
        fr_row.addWidget(btn_dup)
        fr_row.addWidget(btn_del)
        fr_row.addStretch(1)
        form.addLayout(fr_row)

        cur_row = QHBoxLayout()
        cur_row.addWidget(QLabel("当前帧"))
        cur_row.addWidget(self.frame_spin)
        cur_row.addWidget(self.chk_all_frames, 1)
        form.addLayout(cur_row)
        form.addWidget(self.anim_lbl)

        size_row = QHBoxLayout()
        size_row.addWidget(QLabel("Body size"))
        size_row.addWidget(self.size_slider, 1)
        size_row.addWidget(self.size_lbl)
        form.addLayout(size_row)

        color_row = QHBoxLayout()
        color_row.addWidget(QLabel("Clip color"))
        color_row.addWidget(self.btn_color)
        color_row.addWidget(btn_apply_all)
        color_row.addStretch(1)
        form.addLayout(color_row)

        form.addWidget(QLabel("当前帧画布"))
        form.addWidget(self.canvas, 0, Qt.AlignmentFlag.AlignHCenter)
        brush_row = QHBoxLayout()
        brush_row.addWidget(QLabel("Brush"))
        brush_row.addWidget(self.btn_brush)
        brush_row.addWidget(self.brush_slider, 1)
        brush_row.addWidget(self.brush_lbl)
        brush_row.addWidget(self.chk_erase)
        form.addLayout(brush_row)

        doodle_row = QHBoxLayout()
        doodle_row.addWidget(btn_disk)
        doodle_row.addWidget(btn_clear)
        doodle_row.addWidget(btn_import)
        form.addLayout(doodle_row)
        form.addWidget(btn_apply)
        form.addWidget(btn_clear_src)
        form.addWidget(self.src_lbl)

        fit_row = QHBoxLayout()
        fit_row.addWidget(QLabel("Import fit"))
        fit_row.addWidget(self.fit_contain)
        fit_row.addWidget(self.fit_cover)
        fit_row.addStretch(1)
        form.addLayout(fit_row)

        bind_row = QHBoxLayout()
        bind_row.addWidget(btn_bind)
        bind_row.addWidget(btn_import_dir)
        form.addLayout(bind_row)

        face_box = QGroupBox("五官锚点（预留：固件不显示）")
        face_box.setCheckable(True)
        face_box.setChecked(False)
        face_box.toggled.connect(self._on_face_box)
        face_lay = QVBoxLayout(face_box)
        face_lay.addWidget(self.chk_face)
        part_row = QHBoxLayout()
        part_row.addWidget(QLabel("Part"))
        part_row.addWidget(self.part_combo, 1)
        face_lay.addLayout(part_row)
        xy = QHBoxLayout()
        xy.addWidget(QLabel("X"))
        xy.addWidget(self.face_x)
        xy.addWidget(QLabel("Y"))
        xy.addWidget(self.face_y)
        xy.addWidget(QLabel("Angle°"))
        xy.addWidget(self.face_angle)
        face_lay.addLayout(xy)
        face_btns = QHBoxLayout()
        face_btns.addWidget(btn_face_default)
        face_btns.addWidget(btn_face_copy)
        face_lay.addLayout(face_btns)
        form.addWidget(face_box)

        act = QHBoxLayout()
        act.addWidget(btn_save)
        act.addWidget(btn_build)
        act.addWidget(btn_both)
        form.addLayout(act)
        form.addStretch(1)

        editor = QWidget()
        editor.setLayout(form)
        editor_scroll = QScrollArea()
        editor_scroll.setWidget(editor)
        editor_scroll.setWidgetResizable(True)
        editor_scroll.setFrameShape(QFrame.Shape.NoFrame)
        editor_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        editor_scroll.setMinimumWidth(340)

        prev_col = QVBoxLayout()
        prev_col.setContentsMargins(8, 8, 8, 8)
        prev_title = QLabel("多帧预览（设备圆屏）")
        prev_title.setStyleSheet("font-weight:600; color:#eef2ff;")
        prev_col.addWidget(prev_title)
        prev_col.addWidget(self.preview, 0, Qt.AlignmentFlag.AlignHCenter)
        play_row = QHBoxLayout()
        play_row.addWidget(self.chk_play)
        play_row.addWidget(self.play_lbl, 1)
        prev_col.addLayout(play_row)
        prev_col.addWidget(QLabel("帧条 · 点选要编辑的帧"))
        prev_col.addWidget(self.strip)
        prev_col.addStretch(1)
        prev_w = QWidget()
        prev_w.setLayout(prev_col)
        prev_w.setMinimumWidth(320)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(help_scroll)
        split.addWidget(editor_scroll)
        split.addWidget(prev_w)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setStretchFactor(2, 0)
        split.setChildrenCollapsible(False)
        split.setSizes([250, 420, 340])

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.addWidget(split)

        self.ctx.cfg_changed.connect(lambda _: self.reload())
        self.reload()

    def _fit(self) -> str:
        return "cover" if self.fit_cover.isChecked() else "contain"

    def _clip(self) -> dict | None:
        for c in self._cfg.get("clips", []):
            if c.get("id") == self._clip_id:
                return c
        return None

    def _current_face(self) -> dict:
        clip = self._clip() or {}
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        return skin_core.face_for_frame(clip, self._frame_i, w)

    def _write_face(self, face: dict) -> None:
        clip = self._clip()
        if clip is None:
            return
        n = max(1, int(clip.get("frames", 1)))
        faces = clip.get("faces")
        if isinstance(faces, list) and len(faces) == n:
            faces[self._frame_i] = face
            clip["faces"] = faces
            clip.pop("face", None)
        elif n > 1:
            w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
            arr = [skin_core.face_for_frame(clip, i, w) for i in range(n)]
            arr[self._frame_i] = face
            clip["faces"] = arr
            clip.pop("face", None)
        else:
            clip["face"] = face
            clip.pop("faces", None)

    def _load_face_spins(self) -> None:
        face = self._current_face()
        part = face.get(self._face_part, {})
        for sp in (self.face_x, self.face_y, self.face_angle):
            sp.blockSignals(True)
        self.face_x.setValue(int(part.get("x", 0)))
        self.face_y.setValue(int(part.get("y", 0)))
        self.face_angle.setValue(int(part.get("angle", 0)))
        for sp in (self.face_x, self.face_y, self.face_angle):
            sp.blockSignals(False)

    def reload(self) -> None:
        try:
            self._cfg = self.ctx.load_cfg()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design reload error: {exc}")
            return
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        self._cfg["width"] = w
        self._cfg["height"] = w
        self._cfg["version"] = max(int(self._cfg.get("version", 2)), 2)
        self.size_slider.blockSignals(True)
        self.size_slider.setValue(w)
        self.size_slider.blockSignals(False)
        self.size_lbl.setText(f"{w}px")
        self.canvas.set_body_size(w)

        self.clip_combo.blockSignals(True)
        self.clip_combo.clear()
        for c in self._cfg.get("clips", []):
            self.clip_combo.addItem(str(c.get("id")))
        ids = [c.get("id") for c in self._cfg.get("clips", [])]
        if self._clip_id in ids:
            self.clip_combo.setCurrentIndex(ids.index(self._clip_id))
        elif ids:
            self.clip_combo.setCurrentIndex(0)
        self.clip_combo.blockSignals(False)
        name = self.clip_combo.currentText()
        if name:
            self._clip_id = name
        self._load_clip_ui()
        self.refresh_preview()

    def _on_clip(self, name: str) -> None:
        if not name:
            return
        self._clip_id = name
        self._frame_i = 0
        self._play_i = 0
        self._load_clip_ui()
        self.refresh_preview()

    def _on_frame(self, v: int) -> None:
        self._frame_i = int(v)
        self._load_source_image()
        self._load_face_spins()
        self.refresh_preview()
        if not self.chk_play.isChecked():
            self._play_i = self._frame_i
            self._show_preview_frame(self._frame_i)
        self._update_play_lbl()

    def _on_strip_select(self, index: int) -> None:
        self.frame_spin.setValue(int(index))

    def _clip_frame_count(self) -> int:
        clip = self._clip()
        if clip is None:
            return 1
        return max(1, int(clip.get("frames", 1)))

    def _on_frames_count(self, v: int) -> None:
        clip = self._clip()
        if clip is None:
            return
        skin_core.set_clip_frame_count(clip, int(v))
        self._frame_i = min(self._frame_i, self._clip_frame_count() - 1)
        self.ctx.info(f"design: {self._clip_id} frames={self._clip_frame_count()}")
        self._save_json()

    def _dup_frame(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        if self._clip_frame_count() >= skin_core.CLIP_FRAMES_MAX:
            self.ctx.info(f"design: max {skin_core.CLIP_FRAMES_MAX} frames")
            return
        self._frame_i = skin_core.duplicate_clip_frame(clip, self._frame_i)
        self.ctx.info(f"design: duplicated → frame {self._frame_i}")
        self._save_json()

    def _del_frame(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        if self._clip_frame_count() <= 1:
            self.ctx.info("design: last frame, not deleted")
            return
        self._frame_i = skin_core.delete_clip_frame(clip, self._frame_i)
        self.ctx.info(f"design: deleted, now frame {self._frame_i}")
        self._save_json()

    def _rgb565_qimage(self, pixels: bytes, w: int = 240, h: int = 240) -> QImage:
        rgb = skin_core.rgb565_to_qimage_bytes(pixels, w, h)
        return QImage(rgb, w, h, w * 3, QImage.Format.Format_RGB888).copy()

    def _compose_frame(self, i: int) -> bytes:
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        clip = self._clip() or {}
        if i == self._frame_i:
            body, bw, bh = self._body_pixels()
        else:
            body = skin_core.make_clip_frame_pixels(
                clip, i, w, w, self.ctx.cfg_path.parent
            )
            bw, bh = w, w
        face = None
        if self.chk_face.isChecked():
            face = (
                self._current_face()
                if i == self._frame_i
                else skin_core.face_for_frame(clip, i, w)
            )
        return skin_core.compose_home_preview(
            body, bw, bh, skin_core.BG, self.chk_face.isChecked(), face
        )

    def _show_preview_frame(self, i: int) -> None:
        try:
            self.preview.set_rgb565(self._compose_frame(i))
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design preview error: {exc}")
        self._update_play_lbl()

    def _update_play_lbl(self) -> None:
        n = self._clip_frame_count()
        fps = max(1, int(self.fps_spin.value()))
        shown = self._play_i if self.chk_play.isChecked() and n > 1 else self._frame_i
        self.play_lbl.setText(f"播放 {shown}/{max(0, n - 1)}  ·  {n} 帧 @{fps} fps")

    def _rebuild_strip(self) -> None:
        n = self._clip_frame_count()
        images: list[QImage] = []
        for i in range(n):
            try:
                images.append(self._rgb565_qimage(self._compose_frame(i)))
            except Exception:  # noqa: BLE001
                images.append(QImage(240, 240, QImage.Format.Format_RGB888))
        playing = self._play_i if self.chk_play.isChecked() and n > 1 else None
        self.strip.set_frames(images, self._frame_i, playing)
        self._update_play_lbl()

    def _sync_play_timer(self, _on: bool = False) -> None:
        n = self._clip_frame_count()
        fps = max(1, int(self.fps_spin.value()))
        if self.chk_play.isChecked() and n > 1:
            self._play_timer.start(max(80, int(1000 / fps)))
        else:
            self._play_timer.stop()
            self._play_i = self._frame_i
            self._show_preview_frame(self._frame_i)
        self.strip.set_playing_index(
            self._play_i if self.chk_play.isChecked() and n > 1 else None
        )
        self._update_play_lbl()

    def _tick_play(self) -> None:
        n = self._clip_frame_count()
        if n < 2:
            return
        self._play_i = (self._play_i + 1) % n
        self._show_preview_frame(self._play_i)
        self.strip.set_playing_index(self._play_i)

    def _on_part(self, name: str) -> None:
        if not name:
            return
        self._face_part = name
        self._load_face_spins()

    def _on_face_spin(self, _v: int = 0) -> None:
        face = self._current_face()
        face[self._face_part] = {
            "x": int(self.face_x.value()),
            "y": int(self.face_y.value()),
            "angle": int(self.face_angle.value()),
        }
        self._write_face(face)
        self.refresh_preview()

    def _face_defaults(self) -> None:
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        self._write_face(skin_core.default_face(w))
        self._load_face_spins()
        self.ctx.info(f"design: face defaults → {self._clip_id}[{self._frame_i}]")
        self.refresh_preview()

    def _face_copy_all_frames(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        n = max(1, int(clip.get("frames", 1)))
        face = self._current_face()
        clip["faces"] = [
            {p: dict(face[p]) for p in skin_core.FACE_PARTS} for _ in range(n)
        ]
        clip.pop("face", None)
        self.ctx.info(f"design: face copied to {n} frames of {self._clip_id}")
        self.refresh_preview()

    def _on_face_box(self, on: bool) -> None:
        if not on:
            self.chk_face.setChecked(False)
        self.refresh_preview()

    def _bind_assets(self) -> None:
        try:
            notes = skin_core.bind_assets(self._cfg, self.ctx.cfg_path.parent)
            for line in notes:
                self.ctx.info(line)
            self._save_json()
            self.reload()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design bind error: {exc}")

    def _import_folder(self) -> None:
        path = QFileDialog.getExistingDirectory(self, "Import asset folder", "")
        if not path:
            return
        dest = self.ctx.cfg_path.parent / "assets"
        try:
            for line in skin_core.import_asset_folder(Path(path), dest):
                self.ctx.info(line)
            self._bind_assets()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design import error: {exc}")

    def _load_source_image(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        r, g, b = (int(x) for x in clip.get("color", [74, 163, 200]))
        path_str = None
        sources = clip.get("sources")
        if isinstance(sources, list) and self._frame_i < len(sources) and sources[self._frame_i]:
            path_str = str(sources[self._frame_i])
        elif clip.get("source"):
            path_str = str(clip.get("source"))
        if path_str:
            self.src_lbl.setText(f"source: {path_str}")
            try:
                path = skin_core.resolve_asset(path_str, self.ctx.cfg_path.parent)
                img = QImage(str(path))
                if not img.isNull():
                    self.canvas.load_qimage(img)
                    return
            except Exception:  # noqa: BLE001
                pass
        self.src_lbl.setText("source: (synthetic color)")
        self.canvas.fill_disk(QColor(r, g, b))

    def _load_clip_ui(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        r, g, b = (int(x) for x in clip.get("color", [74, 163, 200]))
        self.btn_color.blockSignals(True)
        self.btn_color.set_color(QColor(r, g, b))
        self.btn_color.blockSignals(False)
        self.btn_brush.set_color(QColor(r, g, b))
        self.canvas.set_brush_color(QColor(r, g, b))
        fit = str(clip.get("fit", "contain"))
        self.fit_cover.setChecked(fit == "cover")
        self.fit_contain.setChecked(fit != "cover")
        n = max(1, int(clip.get("frames", 1)))
        self.frames_spin.blockSignals(True)
        self.frames_spin.setValue(n)
        self.frames_spin.blockSignals(False)
        self.frame_spin.blockSignals(True)
        self.frame_spin.setMaximum(max(0, n - 1))
        self.frame_spin.setValue(min(self._frame_i, n - 1))
        self.frame_spin.blockSignals(False)
        self._frame_i = int(self.frame_spin.value())
        self.fps_spin.blockSignals(True)
        self.fps_spin.setValue(max(1, int(clip.get("fps", 4))))
        self.fps_spin.blockSignals(False)
        fps = int(self.fps_spin.value())
        if n > 1:
            self.anim_lbl.setText(
                f"{self._clip_id}: {n} 帧 @{fps} fps 循环。"
                "点帧条选中一帧再改；勾选「全部帧」则一次写入每一帧。"
            )
        else:
            self.anim_lbl.setText(f"{self._clip_id}: 单帧静图。加帧请改「帧数」或点「复制帧」。")
        self._load_source_image()
        self._load_face_spins()
        self._rebuild_strip()
        self._sync_play_timer()

    def _on_size(self, v: int) -> None:
        self.size_lbl.setText(f"{v}px")
        self._cfg["width"] = int(v)
        self._cfg["height"] = int(v)
        self.canvas.set_body_size(int(v))
        self.refresh_preview()

    def _on_color(self, c: QColor) -> None:
        clip = self._clip()
        if clip is None:
            return
        clip["color"] = list(rgb_tuple(c))
        self.btn_brush.set_color(c)
        self.canvas.set_brush_color(c)
        if not clip.get("source") and not clip.get("sources"):
            self.canvas.fill_disk(c)
        self.refresh_preview()

    def _on_fps(self, v: int) -> None:
        clip = self._clip()
        if clip is None:
            return
        clip["fps"] = int(v)
        n = max(1, int(clip.get("frames", 1)))
        if n > 1:
            self.anim_lbl.setText(
                f"{self._clip_id}: {n} 帧 @{int(v)} fps 循环。"
                "点帧条选中一帧再改；勾选「全部帧」则一次写入每一帧。"
            )
        else:
            self.anim_lbl.setText(f"{self._clip_id}: 单帧静图。加帧请改「帧数」或点「复制帧」。")
        self._sync_play_timer()

    def _on_brush(self, v: int) -> None:
        self.brush_lbl.setText(f"{v}px")
        self.canvas.set_brush_radius(v)

    def _fill_disk(self) -> None:
        self.canvas.fill_disk(self.btn_color.color())

    def _qimage_png_bytes(self, img: QImage) -> bytes:
        ba = QByteArray()
        buf = QBuffer(ba)
        buf.open(QIODevice.OpenModeFlag.WriteOnly)
        img.save(buf, "PNG")
        buf.close()
        return bytes(ba.data())

    def _commit_frames(self, *, src: Path | None = None, png: bytes | None = None) -> list[Path]:
        clip = self._clip()
        if clip is None:
            raise ValueError("no clip")
        n = max(1, int(clip.get("frames", 1)))
        apply_all = bool(self.chk_all_frames.isChecked() or n <= 1)
        frame_i = None if apply_all else self._frame_i
        written = skin_core.install_clip_frames(
            self.ctx.cfg_path.parent / "assets",
            self._clip_id,
            n,
            src=src,
            png=png,
            frame_i=frame_i,
        )
        rels: dict[int, str] = {}
        if apply_all:
            for i in range(n):
                rels[i] = f"assets/{skin_core.clip_frame_stem(self._clip_id, i, n)}.png"
        else:
            rels[self._frame_i] = (
                f"assets/{skin_core.clip_frame_stem(self._clip_id, self._frame_i, n)}.png"
            )
        skin_core.set_clip_sources(clip, rels, apply_all)
        clip["fit"] = self._fit()
        clip["color"] = list(rgb_tuple(self.btn_color.color()))
        return written

    def _import_png(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Import body image",
            "",
            "Images (*.png *.jpg *.jpeg *.bmp *.webp);;All (*.*)",
        )
        if not path:
            return
        img = QImage(path)
        if img.isNull():
            self.ctx.info(f"design: cannot open {path}")
            return
        self.canvas.load_qimage(img)
        try:
            written = self._commit_frames(src=Path(path))
            note = ", ".join(p.name for p in written)
            self.ctx.info(f"design: imported → {note}")
            self._build()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design import error: {exc}")

    def _apply_doodle(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        try:
            png = self._qimage_png_bytes(self.canvas.to_qimage())
            written = self._commit_frames(png=png)
            note = ", ".join(p.name for p in written)
            self.ctx.info(f"design: doodle saved → {note}")
            self._build()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design apply error: {exc}")

    def _clear_source(self) -> None:
        clip = self._clip()
        if clip is None:
            return
        n = max(1, int(clip.get("frames", 1)))
        skin_core.clear_clip_assets(self.ctx.cfg_path.parent / "assets", self._clip_id, n)
        clip.pop("source", None)
        clip.pop("sources", None)
        self.src_lbl.setText("source: (synthetic color)")
        self.canvas.fill_disk(self.btn_color.color())
        self.ctx.info(f"design: {self._clip_id} → synthetic color")
        try:
            self._build()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design clear error: {exc}")

    def _color_all(self) -> None:
        rgb = list(rgb_tuple(self.btn_color.color()))
        for c in self._cfg.get("clips", []):
            c["color"] = list(rgb)
        self.ctx.info(f"design: color {hex_color(self.btn_color.color())} → all clips")

    def _body_pixels(self) -> tuple[bytes, int, int]:
        w = skin_core.clamp_body_size(int(self._cfg.get("width", skin_core.BODY_SIZE_DEFAULT)))
        h = w
        img = self.canvas.to_qimage().convertToFormat(QImage.Format.Format_ARGB32)
        if img.width() != w or img.height() != h:
            img = img.scaled(
                w,
                h,
                Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
        bg = skin_core.BG
        out = bytearray()
        for y in range(h):
            for x in range(w):
                c = img.pixelColor(x, y)
                if c.alpha() < 128:
                    out += struct.pack("<H", skin_core.rgb565(*bg))
                else:
                    out += struct.pack("<H", skin_core.rgb565(c.red(), c.green(), c.blue()))
        return bytes(out), w, h

    def refresh_preview(self) -> None:
        try:
            pixels = self._compose_frame(self._frame_i)
            img = self._rgb565_qimage(pixels)
            self.strip.update_thumb(self._frame_i, img, current=self._frame_i)
            if not self.chk_play.isChecked() or self._clip_frame_count() < 2:
                self.preview.set_rgb565(pixels)
            self._update_play_lbl()
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design preview error: {exc}")

    def _save_json(self) -> None:
        try:
            try:
                disk = self.ctx.load_cfg()
            except Exception:  # noqa: BLE001
                disk = {}
            if isinstance(disk, dict) and "splash" in disk:
                self._cfg["splash"] = disk["splash"]
            clip = self._clip()
            if clip is not None:
                clip["fit"] = self._fit()
                clip["color"] = list(rgb_tuple(self.btn_color.color()))
                clip["fps"] = int(self.fps_spin.value())
            self._cfg["width"] = int(self.size_slider.value())
            self._cfg["height"] = int(self.size_slider.value())
            self._cfg["version"] = max(int(self._cfg.get("version", 2)), 2)
            path = skin_core.save_cfg(self._cfg, self.ctx.cfg_path)
            self.ctx.info(f"saved {path}")
            self.ctx.cfg_changed.emit(str(path))
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design save error: {exc}")

    def _build(self) -> None:
        try:
            self._save_json()
            msg = skin_core.build_pack(self._cfg, self.ctx.out_dir, self.ctx.cfg_path)
            self.ctx.info(msg)
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design build error: {exc}")

    def _build_both(self) -> None:
        try:
            self._build()
            self.ctx.info(
                skin_core.build_splash_from_cfg(
                    self.ctx.out_dir, self._cfg, self.ctx.cfg_path.parent
                )
            )
        except Exception as exc:  # noqa: BLE001
            self.ctx.info(f"design pack+splash error: {exc}")
