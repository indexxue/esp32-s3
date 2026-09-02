# -*- coding: utf-8 -*-
"""Generate wake reply pack: ding.wav + phrase wavs → tools/pet_sim/sdcard/sfx/wake/."""

from __future__ import annotations

import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
TTS = ROOT / "tts.py"
OUT = ROOT.parent / "pet_sim" / "sdcard" / "sfx" / "wake"

PHRASES = [
    ("zaine.wav", "在呢"),
    ("wozai.wav", "我在"),
    ("en.wav", "嗯？"),
    ("laile.wav", "来了"),
    ("zenme.wav", "怎么啦"),
]

VOICE = "zh-CN-XiaoyiNeural"
RATE = "+10%"
PITCH = "+22Hz"


def write_ding(path: Path) -> None:
    sr, dur, f0 = 16000, 0.18, 880.0
    n = int(sr * dur)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        frames = bytearray()
        for i in range(n):
            t = i / sr
            env = min(1.0, t / 0.01) * math.exp(-t * 12.0)
            s = int(max(-32767, min(32767, 12000.0 * env * math.sin(2.0 * math.pi * f0 * t))))
            frames += struct.pack("<h", s)
        w.writeframes(frames)


def synth(text: str, out: Path) -> None:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as tf:
        tf.write(text)
        text_path = Path(tf.name)
    try:
        subprocess.run(
            [
                str(PY),
                str(TTS),
                "synth",
                "-f",
                str(text_path),
                "-e",
                "edge",
                "--voice",
                VOICE,
                "--rate",
                RATE,
                "--pitch",
                PITCH,
                "-o",
                str(out),
            ],
            check=True,
            cwd=str(ROOT),
        )
    finally:
        text_path.unlink(missing_ok=True)


def main() -> int:
    if not PY.is_file():
        print("missing pet_tts venv; run tools/pet_tts/setup.ps1", file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    ding = OUT / "ding.wav"
    write_ding(ding)
    print(f"ding: {ding}", flush=True)
    for name, text in PHRASES:
        out = OUT / name
        print(f"{name}: {text}", flush=True)
        synth(text, out)
    print(f"done → {OUT}")
    print("Copy to device: <SD>:\\sfx\\wake\\")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
