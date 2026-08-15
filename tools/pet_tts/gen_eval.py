# -*- coding: utf-8 -*-
"""Generate one female eval clip per pet SFX category (edge + piper)."""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
TTS = ROOT / "tts.py"

CLIPS = [
    ("eat", "好吃"),
    ("play", "耶，开心"),
    ("poke", "呀"),
    ("refuse", "不要啦"),
    ("sleep", "晚安"),
]


def synth(engine: str, text: str, out: Path, *, voice_key: str | None = None, lang: str | None = None) -> None:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as tf:
        tf.write(text)
        text_path = Path(tf.name)
    try:
        args = [str(PY), str(TTS), "synth", "-f", str(text_path), "-e", engine, "-o", str(out)]
        if engine == "edge":
            args.extend(["--voice-key", voice_key or "zh-f"])
        else:
            args.extend(["--lang", lang or "zh"])
        print(f"{engine} {out.name}: {text}", flush=True)
        subprocess.run(args, check=True, cwd=str(ROOT))
    finally:
        text_path.unlink(missing_ok=True)


def main() -> int:
    edge_dir = ROOT / "out" / "eval" / "edge"
    piper_dir = ROOT / "out" / "eval" / "piper"
    edge_dir.mkdir(parents=True, exist_ok=True)
    piper_dir.mkdir(parents=True, exist_ok=True)

    for clip_id, text in CLIPS:
        synth("edge", text, edge_dir / f"{clip_id}.wav", voice_key="zh-f")
        synth("piper", text, piper_dir / f"{clip_id}.wav", lang="zh")

    print("done: out/eval/{edge,piper}/*.wav")
    return 0


if __name__ == "__main__":
    sys.exit(main())
