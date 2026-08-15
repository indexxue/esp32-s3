# -*- coding: utf-8 -*-
"""Generate Care SFX from sfx_lines.json (edge) and install into pet_sim sdcard pet pack."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
TTS = ROOT / "tts.py"
LINES = ROOT / "sfx_lines.json"
OUT_PACK = ROOT / "out" / "sfx_pack"
SDCARD_SFX = ROOT.parent / "pet_sim" / "sdcard" / "pet" / "sfx"


def synth_one(text: str, voice: str, rate: str, pitch: str, out: Path) -> None:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as tf:
        tf.write(text)
        text_path = Path(tf.name)
    try:
        args = [
            str(PY),
            str(TTS),
            "synth",
            "-f",
            str(text_path),
            "-e",
            "edge",
            "--voice",
            voice,
            "--rate",
            rate,
            "--pitch",
            pitch,
            "-o",
            str(out),
        ]
        subprocess.run(args, check=True, cwd=str(ROOT))
    finally:
        text_path.unlink(missing_ok=True)


def generate(cfg: dict) -> Path:
    voices = cfg["voices"]
    if OUT_PACK.exists():
        shutil.rmtree(OUT_PACK)
    OUT_PACK.mkdir(parents=True)

    for clip, entries in cfg["clips"].items():
        clip_dir = OUT_PACK / clip
        clip_dir.mkdir(parents=True)
        for ent in entries:
            vk = ent["voice_key"]
            v = voices[vk]
            out = clip_dir / ent["file"]
            print(f"{clip}/{ent['file']}: [{vk}] {ent['text']}", flush=True)
            synth_one(ent["text"], v["voice"], v["rate"], v["pitch"], out)

    # keep a copy of the script used
    shutil.copy2(LINES, OUT_PACK / "sfx_lines.json")
    return OUT_PACK


def install(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for clip_dir in sorted(src.iterdir()):
        if not clip_dir.is_dir():
            continue
        target = dst / clip_dir.name
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        for wav in sorted(clip_dir.glob("*.wav")):
            shutil.copy2(wav, target / wav.name)
            print(f"install {target / wav.name}", flush=True)


def main() -> int:
    p = argparse.ArgumentParser(description="Build cute Care SFX and install to sdcard pet pack")
    p.add_argument("--lines", type=Path, default=LINES)
    p.add_argument("--no-install", action="store_true", help="Only generate under out/sfx_pack")
    p.add_argument("--install-only", action="store_true", help="Copy existing out/sfx_pack → sdcard")
    args = p.parse_args()

    if args.install_only:
        if not OUT_PACK.is_dir():
            raise SystemExit(f"missing {OUT_PACK}; run without --install-only first")
        install(OUT_PACK, SDCARD_SFX)
        return 0

    cfg = json.loads(args.lines.read_text(encoding="utf-8"))
    pack = generate(cfg)
    if not args.no_install:
        install(pack, SDCARD_SFX)
        print(f"done → {SDCARD_SFX}")
    else:
        print(f"done → {pack} (not installed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
