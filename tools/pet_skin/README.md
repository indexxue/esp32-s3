# pet_skin — 桌宠皮肤工具

作者侧唯一入口：把 PNG / 涂鸦打成设备可读的 `/sdcard/pet/` 二进制。  
写盘只经 [`skin_core.py`](skin_core.py)；GUI / CLI 不得另写第二套落盘逻辑。

**本阶段固件只显示身体**，不叠五官。开机仅静态 `boot/splash.bin`。`boot/anim/`、sfx/font/theme 预留。

---

## 1. 安装与启动

在仓库根目录：

```powershell
py -3 -m pip install -r tools/pet_skin/requirements.txt

# GUI（推荐）
py -3 tools/pet_skin/run_gui.py
# 或双击 tools/pet_skin/pet_skin.cmd
```

依赖：Python 3.10+、PySide6、Pillow。

默认读取 [`pack.json`](pack.json)，默认输出 [`tools/pet_sim/sdcard/pet/`](../pet_sim/sdcard/pet/)（模拟器与拷卡共用）。

---

## 2. 你要准备什么图

透明底 PNG（RGBA）。**不要画眼睛、嘴巴、眉毛**（五官预留，固件不显示）。透明区在设备上会填成屏底 `#202020`。

| 文件名 | 画布 | clip | 帧 |
|--------|------|------|----|
| `idle_0.png` / `idle_1.png` | 160×160 | idle | 2 |
| `sleepy.png` | 160×160 | sleepy | 1 |
| `sad.png` | 160×160 | sad | 1 |
| `eat_0.png` / `eat_1.png` | 160×160 | eat | 2 |
| `play_0.png` / `play_1.png` | 160×160 | play | 2 |
| `poke.png` | 160×160 | poke | 1 |
| `sleep_loop.png` | 160×160 | sleep_loop | 1 |
| `splash.png` | 240×240 | 开机 | 单帧 |

文件名必须对上，扩展名可以是 `.png` / `.webp` / `.jpg`。出图提示见 [`doc/desktop_pet/skin_ai_asset_brief.md`](../../doc/desktop_pet/skin_ai_asset_brief.md)。

缺某张图时，该 clip **退回色块身体**，不会空屏。

---

## 3. GUI 使用（推荐）

启动后左侧：Splash → Design → Pack → Body。

### 3.1 最快路径（已有一套 PNG）

1. 把上表文件放进 `tools/pet_skin/assets/`，**或**在 Design / Body 点 **Import folder…** 选你的出图目录（只拷贝认识的文件名）。
2. 点 **Bind assets/**：按文件名写入 `pack.json` 的 `source` / `sources`。
3. Design 里逐个 clip 看右侧圆屏预览（Frame 切 idle_0 / idle_1）。
4. 点 **Build pack.bin**（只要身体）或 **Build pack + splash**（连开机图）。
5. 把输出目录拷到卡上（**整棵** `sdcard/`，含根目录 `config`），**或**导出 zip 用网页上传：

```
tools/pet_sim/sdcard/   →   设备 /sdcard/
  config
  pet/pack.bin
  pet/body/*.bin
  pet/boot/splash.bin
```

网页换肤：Pack 面板 **Export zip…**（或 `py -3 tools/pet_skin/cli.py --zip`）得到 `pet.zip`，浏览器打开设备 `http://<IP>/`，选 zip → **上传并重启**。只覆盖 `pet/`，不改 `config`。

卡须 FAT32。无卡时固件用色块 fallback。

### 3.2 Design：多帧身体

三栏：**左侧说明** · **中间编辑当前帧** · **右侧循环预览 + 帧条**。

1. 选动作（idle 呼吸 / eat / play…）。设备按 fps 循环播放该 clip 的全部帧（最多 8）。
2. **帧数** / **复制帧** / **删除帧** 增删帧。新帧先复制上一帧，再单独改差别。
3. 点右侧**帧条**选中要改的帧，再 Import PNG 或涂鸦后 Apply。默认只写当前帧。
4. 右侧圆屏勾选 **循环预览**，按 fps 切帧，和设备观感一致。两帧差太大就会闪。
5. 要静图：勾选 **应用到全部帧** 再 Import / Apply。
6. Import / Apply 会立刻写 `pack.bin`。把输出目录的 `pet/` 整份拷到 SD。

身体边长 48–180（默认 160）。圆屏预览始终完整显示 240×240。

### 3.3 Splash：开机静图

1. **Browse…** 会把图拷成 `assets/splash.png` 并立刻写 `{输出}/boot/splash.bin`。重开工具仍是这张图，Pack / Design 的 Build pack + splash 也用它。
2. 可改 contain/cover、内容尺寸、背景色 / 合成身体色；这些写在 `pack.json` 的 `splash` 里。
3. 进度环只是预览（固件 LVGL 叠加，不写进 `splash.bin`）。
4. 右侧预览始终完整显示 240×240 圆屏，不用缩放或平移。
5. **Write splash.bin** / **Clear** 也会更新输出。Clear 删掉 `assets/splash.*`，改回合成身体。

### 3.4 Pack / Body

- **Body**：每条 clip 的 OK / SYNTH / MISSING；SYNTH = 打包用色块。
- **Pack**：看 `pack.json`、Check、改输出目录、Build。
- **Check**：引用了文件却找不到 → error；没绑图 → warn（合法，用色块）。

### 3.5 五官锚点（预留）

Design 里折叠组 **五官锚点** 默认关。打开后可写 `face` / `faces` 进 `pack.json` / `pack.bin` v2，**设备本阶段忽略、不画眼嘴眉**。以后打开固件叠加时再用。

---

## 4. CLI

在仓库根目录。默认 `--cfg tools/pet_skin/pack.json`，`--out tools/pet_sim/sdcard/pet`。

```powershell
# 检查素材（缺引用文件才失败；色块 fallback 只警告）
py -3 tools/pet_skin/cli.py --check

# 从出图目录拷入 assets/，绑定，打 pack + splash
py -3 tools/pet_skin/cli.py --import D:\out_png --bind --splash

# 已放在 assets/：绑定并打包（splash 优先用 assets/splash.png）
py -3 tools/pet_skin/cli.py --bind --splash

# 只打身体包
py -3 tools/pet_skin/cli.py

# 打 pack + splash，并写出网页上传用的 pet.zip
py -3 tools/pet_skin/cli.py --bind --splash --zip

# 只写开机（不改 pack.bin）
py -3 tools/pet_skin/cli.py --splash --no-pack
py -3 tools/pet_skin/cli.py --splash D:\art\splash.png --no-pack
```

常用参数：`--fit contain|cover`、`--bg #202020`、`--size 240`、`--cfg`、`--out`。

---

## 5. 产物与约定

设备**不读 JSON / PNG**。作者侧 `pack.json` + `assets/*.png` → 工具写出：

```
/sdcard/config
/sdcard/pet/pack.bin
/sdcard/pet/body/idle_0.bin
/sdcard/pet/boot/splash.bin
```

`config` 在卡根（换肤不覆盖）；打包到默认 `.../sdcard/pet` 时工具会补写缺失的兄弟文件 `config`。`pack.bin` / RGBH 布局见 [`doc/desktop_pet/framework.md`](../../doc/desktop_pet/framework.md) §6。合法路径见 [`product.md`](../../doc/desktop_pet/product.md) §A。

---

## 目录索引

| 路径 | 职责 |
|------|------|
| [`skin_core.py`](skin_core.py) | 写盘核心：bind / check / `pack.bin` / `body/*.bin` / `boot/splash.bin` |
| [`cli.py`](cli.py) | 命令行 |
| [`run_gui.py`](run_gui.py) / [`pet_skin.cmd`](pet_skin.cmd) | Qt GUI |
| [`pack.json`](pack.json) | clip / needs（设备不读） |
| [`assets/`](assets/) | 作者侧 PNG |
| [`app/features/`](app/features/) | Splash / Design / Pack / Body |
| [`app/registry.py`](app/registry.py) | 侧栏功能登记 |

## 侧栏状态

| id | 面板 | 状态 |
|----|------|------|
| `splash` | 开机 Splash | ready |
| `design` | 宠物设计 | ready |
| `pack` | 资源包 | ready |
| `body` | clip 清单 + 绑定 | ready |
| `sfx` / `font` / `theme` | 预留 | reserved stub |

## 如何加功能

1. 在 `app/features/` 新建 `*_feature.py`，继承 `FeatureModule`。
2. 在 `registry.built_in_features()` 登记。
3. 落盘只调用 `skin_core`。
4. 改合法路径 / 开机语义：先改 `doc/desktop_pet/product.md` 与 `CONTEXT.md`。

## 关联

| 文档或工程 | 关系 |
|------------|------|
| [`product.md`](../../doc/desktop_pet/product.md) §A | 皮肤树、开机 C |
| [`skin_ai_asset_brief.md`](../../doc/desktop_pet/skin_ai_asset_brief.md) | 出图文件名清单 |
| [`tools/pet_sim/`](../pet_sim/) | 模拟器读同一 `sdcard/pet` |
| [`desktop_pet/main/source/pet/`](../../desktop_pet/main/source/pet/) | 固件消费产物 |

原目录名 `tools/pet_pack` 已废弃。
