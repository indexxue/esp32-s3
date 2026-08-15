# pet_skin — 桌宠皮肤工具

作者侧唯一入口：把 PNG / 涂鸦打成设备可读的 `/sdcard/pet/` 二进制。  
写盘只经 [`skin_core.py`](skin_core.py)；GUI / CLI 不得另写第二套落盘逻辑。

路径：本目录现为 `tools/pet_tool/skin/`（伞目录见 [`../README.md`](../README.md)）。

**本阶段固件只显示身体**，不叠五官。开机仅静态 `boot/splash.bin`。可选按键图标 `theme/ui/*.bin`（透明圆图 / 纯色圆；缺则字母 F/P/S/C）。`boot/anim/`、sfx/font 仍预留。

网页预览（结合本目录 PNG）：`py -3 tools/pet_tool/serve.py` → http://127.0.0.1:8765/web/

---

## 1. 安装与启动

在仓库根目录：

```powershell
py -3 -m pip install -r tools/pet_tool/skin/requirements.txt

# GUI（推荐）
py -3 tools/pet_tool/skin/run_gui.py
# 或双击 tools/pet_tool/skin/pet_skin.cmd
```

依赖：Python 3.10+、PySide6、Pillow。

默认读取 [`pack.json`](pack.json)，默认输出 [`tools/pet_sim/sdcard/pet/`](../../pet_sim/sdcard/pet/)（模拟器与拷卡共用）。

---

## 2. 你要准备什么图

透明底 PNG（RGBA）。**不要画眼睛、嘴巴、眉毛**（五官预留，固件不显示）。透明区在设备上会填成屏底 `#202020`。

| 文件名 | 画布 | clip | 帧 |
|--------|------|------|----|
| `idle_0.png` / `idle_1.png` … | 160×160 | idle | 多帧 |
| `sleepy.png` | 160×160 | sleepy | 1 |
| `sad.png` | 160×160 | sad | 1 |
| `eat_0.png` / `eat_1.png` | 160×160 | eat | 2 |
| `play_0.png` / `play_1.png` … | 160×160 | play | 多帧 |
| `poke.png` | 160×160 | poke | 1 |
| `sleep_loop.png` | 160×160 | sleep_loop | 1 |
| `refuse.png`（可选） | 160×160 | refuse | 1；**照料分档**喂拒/冷戳；缺则 Bind 可用 `sad.png`，设备也可 fallback sad |
| `splash.png` | 240×240 | 开机 | 单帧 |
| `ui_feed/play/sleep/chat/wifi_*.png` | 28×28 | 按键/网络 | 可选 |

缺某张图时，该 clip **退回色块身体**，不会空屏。出图提示见 [`doc/desktop_pet/skin_ai_asset_brief.md`](../../../doc/desktop_pet/skin_ai_asset_brief.md)。

---

## 3. GUI 使用（推荐）

启动后左侧：Splash → Design → Pack → Body → Theme。

1. 把上表文件放进 `assets/`，或 Design / Body 点 **Import folder…**。
2. 点 **Bind assets/**。
3. Design 里逐个 clip 看圆屏预览。
4. 点 **Build pack.bin** 或 **Build pack + splash + UI icons**。
5. 拷整棵 `tools/pet_sim/sdcard/` 到卡，或 Export zip 网页上传（会尽量带上 `font/caption.bin`；设备端若 zip 无字库会保留原 `pet/font`）。

---

## 4. CLI

在仓库根目录。默认 `--cfg tools/pet_tool/skin/pack.json`，`--out tools/pet_sim/sdcard/pet`。

```powershell
py -3 tools/pet_tool/skin/cli.py --check
py -3 tools/pet_tool/skin/cli.py --import D:\out_png --bind --splash
py -3 tools/pet_tool/skin/cli.py --bind --splash --theme-ui
py -3 tools/pet_tool/skin/cli.py --bind --splash --zip
```

---

## 5. 扩展

在 `app/features/` 新增 `FeatureModule`，登记到 `registry.built_in_features()`；写盘只调用 `skin_core`。
