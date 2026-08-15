# pet_tts — 文字转 16 kHz WAV（Piper + edge-tts）

为桌宠 `pet/sfx` 制作短语音。输出固定为 **16 kHz mono 16-bit PCM WAV**（与板端一致）。

| 引擎 | 说明 |
|------|------|
| **piper** | 本地离线。中文 `zh_CN-huayan-medium`，英文 `en_US-lessac-medium` |
| **edge** | 微软 Edge 在线 TTS（量产 Care SFX 默认） |

大模型文件在 `voices/`，已 gitignore，不入库。

## 首次安装

```powershell
powershell -ExecutionPolicy Bypass -File tools\pet_tts\setup.ps1
```

会创建 `.venv`、安装依赖，并下载中/英 Piper 音色。国内默认走 `hf-mirror.com`。

## 生成并写入 SD 卡 pet 包（推荐）

文案 / 音色表：[`sfx_lines.json`](sfx_lines.json)（晓晓 + 晓伊，略提 pitch）。

```powershell
$env:PYTHONIOENCODING='utf-8'
py -3 tools\pet_tts\gen_sfx_pack.py
```

输出：
- 中间产物：`tools/pet_tts/out/sfx_pack/<clip>/N.wav`
- 安装到：`tools/pet_sim/sdcard/pet/sfx/`（整包替换各 clip 目录）

只生成不安装：加 `--no-install`。已有 `out/sfx_pack` 仅安装：`--install-only`。

后续扩展：改 `sfx_lines.json`（每 clip 仍建议 ≤8 句），再跑同一命令。

## 单句试听

```powershell
cd tools\pet_tts
$py = .\.venv\Scripts\python.exe

& $py tts.py synth -f text.txt -e edge --voice-key zh-f2 --rate +10% --pitch +22Hz -o out\t.wav
& $py tts.py list-edge
```

### edge 快捷键

| `--voice-key` | 音色 |
|---------------|------|
| `zh-f` | zh-CN-XiaoxiaoNeural（晓晓） |
| `zh-f2` | zh-CN-XiaoyiNeural（晓伊） |
| `zh-m` | zh-CN-YunxiNeural |
| `en-f` | en-US-JennyNeural |
| `en-m` | en-US-GuyNeural |

`--rate` / `--pitch` 仅 edge 有效（如 `+10%`、`+18Hz`）。
