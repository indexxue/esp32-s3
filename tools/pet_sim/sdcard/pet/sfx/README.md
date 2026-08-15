# Care SFX（照料提示音）

16 kHz mono 16-bit PCM WAV。每 clip 多句随机抽 1（池 ≤8）。

当前包由 `tools/pet_tts` 用 **edge-tts** 生成（晓晓 / 晓伊，略提 pitch），已写入本目录：

```
sfx/
  eat/0..3.wav
  play/0..3.wav
  poke/0..3.wav
  refuse/0..3.wav
  sleep/0..3.wav
```

| clip | 触发 | 文案（0→3） |
|------|------|-------------|
| eat | 喂食 | 好吃耶～ / 嗯嗯，真香 / 谢谢喂食 / 再来一口嘛 |
| play | 玩耍 | 耶！开心 / 一起玩嘛 / 好开心呀 / 再玩一会儿 |
| poke | 戳戳 | 呀～ / 嘿嘿 / 干嘛呀 / 痒痒的 |
| refuse | 拒绝（饱/冷等） | 不要啦～ / 哼，不要 / 人家饱了 / 才不要呢 |
| sleep | 睡觉 | 晚安～ / 困困了 / 呼呼睡 / 明天再玩 |

重新生成并覆盖本目录：

```powershell
$env:PYTHONIOENCODING='utf-8'
py -3 tools\pet_tts\gen_sfx_pack.py
```

文案与音色见 `tools/pet_tts/sfx_lines.json`（改完再跑上面命令即可扩展）。
