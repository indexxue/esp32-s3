# pet_skin — 桌宠皮肤工具

作者侧唯一入口：把 PNG / 涂鸦 / **视频抽帧**打成设备可读的 `/sdcard/pet/` 二进制。  
写盘只经 [`skin_core.py`](skin_core.py)；GUI / CLI 不得另写第二套落盘逻辑。

路径：本目录现为 `tools/pet_tool/skin/`（伞目录见 [`../README.md`](../README.md)）。

**本阶段固件只显示身体**，不叠五官。开机仅静态 `boot/splash.bin`。可选按键图标 `theme/ui/*.bin`（透明圆图 / 纯色圆；缺则字母 F/P/S/C）。`boot/anim/`、sfx/font 仍预留。  
每 clip 最多 **12 帧**（与固件 `PET_RES_MAX_FRAMES` 一致）。

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

依赖：Python 3.10+、PySide6、Pillow、imageio、imageio-ffmpeg（视频抽帧）。

默认读取 [`pack.json`](pack.json)，默认输出 [`tools/pet_sim/sdcard/pet/`](../../pet_sim/sdcard/pet/)（模拟器与拷卡共用）。

---

## 2. 你要准备什么图

透明底 PNG（RGBA），或由**短视频抽帧**得到。参考图/视频可带五官。透明区在设备上会填成屏底 `#202020`。

| 文件名 | 画布 | clip | 帧 |
|--------|------|------|----|
| `idle_0.png` / `idle_1.png` … | 160×160 | idle | 多帧（≤12） |
| `sleepy.png` | 160×160 | sleepy | 1 |
| `sad.png` | 160×160 | sad | 1 |
| `eat_0.png` / `eat_1.png` | 160×160 | eat | 多帧 |
| `play_0.png` / `play_1.png` … | 160×160 | play | 多帧 |
| `poke.png` | 160×160 | poke | 1 |
| `sleep_loop.png` | 160×160 | sleep_loop | 1 |
| `refuse.png`（可选） | 160×160 | refuse | 1；缺则 Bind 可用 `sad` |
| `splash.png` | 240×240 | 开机 | 单帧 |
| `ui_feed/play/sleep/chat/wifi_*.png` | 28×28 | 按键/网络 | 可选 |

缺某张图时，该 clip **退回色块身体**。出图提示见 [`doc/desktop_pet/skin_ai_asset_brief.md`](../../../doc/desktop_pet/skin_ai_asset_brief.md)（单图参考 → PNG 帧；适配 240 圆屏）。

### 视频 → 某 clip 多帧

本地 mp4/webm/mov… **均匀抽帧**写入 `assets/<clip>_i.png`，并更新 `pack.json` 帧数（1–12）。

- GUI Design：选中动作 → **Import video…**（旁侧「抽帧」默认 12）
- CLI：

```powershell
py -3 tools/pet_tool/skin/cli.py --import-video D:\clip.mp4 --clip idle --frames 12 --bind --no-pack
```

---

## 3. GUI 使用（推荐）

启动后左侧：Splash → Design → Pack → Body → Theme。

1. 把上表文件放进 `assets/`，或 Design / Body 点 **Import folder…**；或 Design **Import video…**。
2. 点 **Bind assets/**。
3. Design 里逐个 clip 看圆屏预览。
4. 点 **Build pack.bin** 或 **Build pack + splash + UI icons**。
5. 拷整棵 `tools/pet_sim/sdcard/` 到卡，或 Pack **Export zip** 网页上传。

Export zip 勾选 **Include font** 时会尝试带上 `font/caption.bin`。若字库超过设备单文件上限，导出失败并提示；可取消勾选（设备保留原字库）。

---

## 4. CLI

在仓库根目录。默认 `--cfg tools/pet_tool/skin/pack.json`，`--out tools/pet_sim/sdcard/pet`。

```powershell
py -3 tools/pet_tool/skin/cli.py --check
py -3 tools/pet_tool/skin/cli.py --import D:\out_png --bind --splash
py -3 tools/pet_tool/skin/cli.py --import-video D:\idle.mp4 --clip idle --frames 12 --bind
py -3 tools/pet_tool/skin/cli.py --bind --splash --theme-ui
py -3 tools/pet_tool/skin/cli.py --no-pack --zip "$env:USERPROFILE\Desktop\pet.zip" --no-font
```

### 设备 zip 上限（与 `web_skin.c` 对齐）

| 项 | 上限 |
|----|------|
| 单文件（解压后） | 1536 KiB |
| zip 体积 | 4 MiB |
| 条目数 | 64 |
| 路径字符 | ASCII `A-Za-z0-9._-/` |

超限时 `export_skin_zip` / `--zip` **失败退出**，不留下半成品。

---

## 5. 扩展

在 `app/features/` 新增 `FeatureModule`，登记到 `registry.built_in_features()`；写盘只调用 `skin_core`。
