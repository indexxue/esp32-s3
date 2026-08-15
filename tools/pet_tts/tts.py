#!/usr/bin/env python3
"""Desktop-pet TTS helper: Piper (offline) + edge-tts (online) → 16 kHz mono PCM WAV."""

from __future__ import annotations

import argparse
import asyncio
import io
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parent
VOICES_DIR = ROOT / "voices"
OUT_DIR = ROOT / "out"
TARGET_RATE = 16000

PIPER_VOICES = {
    "zh": "zh_CN-huayan-medium",
    "en": "en_US-lessac-medium",
}

# edge-tts shortlist useful for pet lines
EDGE_VOICES = {
    "zh-f": "zh-CN-XiaoxiaoNeural",   # 晓晓
    "zh-f2": "zh-CN-XiaoyiNeural",    # 晓伊（偏幼）
    "zh-m": "zh-CN-YunxiNeural",
    "en-f": "en-US-JennyNeural",
    "en-m": "en-US-GuyNeural",
}


def resample_mono(pcm: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    if src_rate == dst_rate:
        return pcm.astype(np.float32, copy=False)
    if pcm.size == 0:
        return pcm.astype(np.float32)
    n = max(1, int(round(pcm.size * float(dst_rate) / float(src_rate))))
    x_old = np.linspace(0.0, 1.0, num=pcm.size, endpoint=False)
    x_new = np.linspace(0.0, 1.0, num=n, endpoint=False)
    return np.interp(x_new, x_old, pcm.astype(np.float32)).astype(np.float32)


def write_pcm16_wav(path: Path, pcm_f32: np.ndarray, sample_rate: int = TARGET_RATE) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    clipped = np.clip(pcm_f32, -1.0, 1.0)
    pcm_i16 = (clipped * 32767.0).astype(np.int16)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_i16.tobytes())


def ffmpeg_exe() -> str:
    import imageio_ffmpeg

    return imageio_ffmpeg.get_ffmpeg_exe()


def mp3_bytes_to_pcm16k(mp3: bytes) -> np.ndarray:
    """Decode mp3 via bundled ffmpeg → float32 mono @ 16 kHz."""
    cmd = [
        ffmpeg_exe(),
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        "pipe:0",
        "-ac",
        "1",
        "-ar",
        str(TARGET_RATE),
        "-f",
        "s16le",
        "pipe:1",
    ]
    proc = subprocess.run(cmd, input=mp3, capture_output=True, check=False)
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"ffmpeg decode failed: {err}")
    pcm_i16 = np.frombuffer(proc.stdout, dtype=np.int16)
    return pcm_i16.astype(np.float32) / 32768.0


def synth_piper(text: str, model_stem: str) -> tuple[np.ndarray, int]:
    from piper import PiperVoice

    onnx = VOICES_DIR / f"{model_stem}.onnx"
    if not onnx.is_file():
        raise FileNotFoundError(
            f"missing Piper model: {onnx}\nRun: powershell -File tools/pet_tts/setup.ps1"
        )
    voice = PiperVoice.load(str(onnx))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        voice.synthesize_wav(text, wf)
    buf.seek(0)
    pcm, rate = sf.read(buf, dtype="float32", always_2d=False)
    if getattr(pcm, "ndim", 1) > 1:
        pcm = pcm.mean(axis=1)
    return pcm.astype(np.float32), int(rate)


async def synth_edge_async(
    text: str,
    voice: str,
    *,
    rate: str = "+0%",
    pitch: str = "+0Hz",
) -> bytes:
    import edge_tts

    communicate = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
    chunks: list[bytes] = []
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            chunks.append(chunk["data"])
    if not chunks:
        raise RuntimeError("edge-tts returned empty audio")
    return b"".join(chunks)


def synth_edge(
    text: str,
    voice: str,
    *,
    rate: str = "+0%",
    pitch: str = "+0Hz",
) -> np.ndarray:
    mp3 = asyncio.run(synth_edge_async(text, voice, rate=rate, pitch=pitch))
    return mp3_bytes_to_pcm16k(mp3)


def resolve_piper_model(lang: str | None, model: str | None) -> str:
    if model:
        return model
    key = (lang or "zh").lower()
    if key not in PIPER_VOICES:
        raise SystemExit(f"unknown --lang {lang!r}; use: {', '.join(PIPER_VOICES)}")
    return PIPER_VOICES[key]


def resolve_edge_voice(voice_key: str | None, voice: str | None) -> str:
    if voice:
        return voice
    key = (voice_key or "zh-f").lower()
    if key not in EDGE_VOICES:
        raise SystemExit(
            f"unknown --voice-key {voice_key!r}; use: {', '.join(EDGE_VOICES)} "
            "or pass full --voice name"
        )
    return EDGE_VOICES[key]


def cmd_list_edge(_: argparse.Namespace) -> int:
    import edge_tts

    async def _run() -> None:
        voices = await edge_tts.list_voices()
        for v in voices:
            loc = v.get("Locale", "")
            if loc.startswith("zh") or loc.startswith("en"):
                print(f"{v['ShortName']}\t{v.get('Gender', '')}\t{loc}")

    asyncio.run(_run())
    return 0


def cmd_synth(args: argparse.Namespace) -> int:
    text = args.text
    if args.file:
        text = Path(args.file).read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit("empty text")

    out = Path(args.out) if args.out else OUT_DIR / "out.wav"
    if args.engine == "piper":
        model = resolve_piper_model(args.lang, args.model)
        pcm, rate = synth_piper(text, model)
        pcm = resample_mono(pcm, rate, TARGET_RATE)
    else:
        voice = resolve_edge_voice(args.voice_key, args.voice)
        pcm = synth_edge(text, voice, rate=args.rate, pitch=args.pitch)

    write_pcm16_wav(out, pcm, TARGET_RATE)
    print(f"ok: {out} ({TARGET_RATE} Hz mono PCM16, {pcm.size} samples)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Text → 16 kHz mono 16-bit WAV for pet/sfx (piper | edge)."
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("synth", help="Synthesize one clip")
    s.add_argument("text", nargs="?", default="", help="Text to speak")
    s.add_argument("-f", "--file", help="Read text from UTF-8 file")
    s.add_argument(
        "-e",
        "--engine",
        choices=("piper", "edge"),
        default="piper",
        help="piper=offline, edge=edge-tts online",
    )
    s.add_argument("-o", "--out", help="Output WAV path (default: out/out.wav)")
    s.add_argument("--lang", default="zh", help="Piper language shortcut: zh|en")
    s.add_argument("--model", help="Piper model stem, e.g. zh_CN-huayan-medium")
    s.add_argument(
        "--voice-key",
        default="zh-f",
        help="edge shortcut: " + "|".join(EDGE_VOICES),
    )
    s.add_argument("--voice", help="Full edge-tts voice name")
    s.add_argument("--rate", default="+0%", help="edge rate, e.g. +10%% / -5%%")
    s.add_argument("--pitch", default="+0Hz", help="edge pitch, e.g. +15Hz")
    s.set_defaults(func=cmd_synth)

    le = sub.add_parser("list-edge", help="List zh/en edge-tts voices")
    le.set_defaults(func=cmd_list_edge)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
