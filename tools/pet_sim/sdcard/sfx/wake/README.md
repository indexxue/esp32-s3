# Wake reply (D15)

16 kHz mono 16-bit PCM WAV. Device path: `/sdcard/sfx/wake/` (card root, **not** under `pet/`).

| File | Content |
|------|---------|
| `ding.wav` | short beep |
| `zaine.wav` | 在呢 |
| `wozai.wav` | 我在 |
| `en.wav` | 嗯？ |
| `laile.wav` | 来了 |
| `zenme.wav` | 怎么啦 |

Regenerate:

```powershell
$env:PYTHONIOENCODING='utf-8'
py -3 tools\pet_tts\gen_wake_reply.py
```

Copy the whole `sfx\wake\` folder to the SD card mount root.
