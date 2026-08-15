# pet_tool — 桌宠作者工具伞

| 子目录 | 用途 |
|--------|------|
| [`skin/`](skin/) | 皮肤打包（GUI / CLI / `skin_core`）；含照料分档 clip `refuse` |
| [`web/`](web/) | 圆屏 HTML 产品预览（读 `skin/pack.json` + PNG） |
| [`font/`](font/) | 字幕字库 `caption.bin` |

LVGL C 模拟器在 [`tools/pet_sim/`](../pet_sim/)（不在本伞内）。

## 网页预览

```powershell
py -3 tools/pet_tool/serve.py
# 或双击 tools/pet_tool/serve.cmd
```

浏览器打开 `http://127.0.0.1:8765/web/`。改 PNG 后点 Reload。

## 皮肤工具

```powershell
py -3 -m pip install -r tools/pet_tool/skin/requirements.txt
py -3 tools/pet_tool/skin/run_gui.py
```

详见 [`skin/README.md`](skin/README.md)。
