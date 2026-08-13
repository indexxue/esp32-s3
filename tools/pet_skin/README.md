# pet_skin — 桌宠皮肤工具

作者侧唯一入口：生成 `/sdcard/pet/` 合法路径下的二进制。  
写盘只经 [`skin_core.py`](skin_core.py)；GUI / CLI 不得另写第二套落盘逻辑。

**开机**：仅静态 `boot/splash.bin`（视觉 C）。`boot/anim/` 延后，本工具不做。

## 怎么跑

```powershell
# GUI（推荐）
py -3 -m pip install -r tools/pet_skin/requirements.txt
py -3 tools/pet_skin/run_gui.py
# 或双击 tools/pet_skin/pet_skin.cmd

# CLI
py -3 tools/pet_skin/cli.py --splash
```

默认输出：`tools/pet_sim/sdcard/pet/`（拷到设备 `/sdcard/pet/`）。

## 目录索引

| 路径 | 职责 |
|------|------|
| [`skin_core.py`](skin_core.py) | 写盘核心：`pack.bin` / `body/*.bin` / `boot/splash.bin` |
| [`cli.py`](cli.py) | 命令行入口 |
| [`run_gui.py`](run_gui.py) / [`pet_skin.cmd`](pet_skin.cmd) | Qt GUI 入口 |
| [`pack.json`](pack.json) | 作者侧 clip / needs 配置（设备不读 JSON） |
| [`requirements.txt`](requirements.txt) | PySide6 + Pillow |
| [`app/main_window.py`](app/main_window.py) | 主窗：侧栏 + 堆叠页 + 日志 |
| [`app/registry.py`](app/registry.py) | **登记新功能**的唯一清单 |
| [`app/base_feature.py`](app/base_feature.py) | `FeatureModule` 合约 |
| [`app/context.py`](app/context.py) | 共享 `pack.json` / 输出目录 / 日志 |
| [`app/features/`](app/features/) | 各功能页（含 Design） |
| [`app/widgets/round_preview.py`](app/widgets/round_preview.py) | 固定 240 圆屏预览（缩放仅检视） |
| [`app/widgets/body_paint.py`](app/widgets/body_paint.py) | 圆形身体涂鸦画布 |

## 侧栏功能状态

| id | 面板 | 状态 |
|----|------|------|
| `splash` | 开机 Splash | ready（静态单帧；背景/身体色 + 内容尺寸可调） |
| `design` | 宠物设计 | ready（涂鸦 / 导入 PNG / 调色 / 尺寸 → pack.json + body） |
| `pack` | 资源包 pack | ready |
| `body` | 身体帧 | preview（列表；编辑走 Design） |
| `sfx` / `font` / `theme` | 预留 | reserved stub |

`boot/anim`：产品 reserved，**暂不挂侧栏**。

### Design 用法（简）

1. 选 clip（idle / eat / …）→ 调色或圆内涂鸦，或 Import PNG。
2. **Apply doodle → clip** 写入 `assets/<clip>.png` 并记入 `pack.json` 的 `source`。
3. **Build pack.bin** → 拷贝 `tools/pet_sim/sdcard/pet/` 到设备 `/sdcard/pet/`。
4. 固件只消费 RGBH；**设备上没有设计器**。五官由固件 LVGL 叠加。

`pack.json` 可选字段：`source`（单图）、`sources`（逐帧）、`fit`=`contain|cover`。

## 如何加功能

1. 在 `app/features/` 新建 `*_feature.py`，继承 `FeatureModule`。
2. 在 `registry.built_in_features()` 登记。
3. 落盘调用 `skin_core`（缺 API 则先扩 `skin_core`，再改 UI）。
4. 若改合法路径 / 开机语义：先改 `doc/desktop_pet/product.md` 与 `CONTEXT.md`。

## 关联文档 / 工程

| 文档或工程 | 关系 |
|------------|------|
| [`doc/desktop_pet/product.md`](../../doc/desktop_pet/product.md) §A | 皮肤树、开机 C、工具约定 |
| [`doc/desktop_pet/CONTEXT.md`](../../doc/desktop_pet/CONTEXT.md) | 术语：皮肤包、开机画面、Splash Gate |
| [`doc/desktop_pet/framework.md`](../../doc/desktop_pet/framework.md) §6 | `pack.bin` / RGBH 二进制 |
| [`tools/pet_sim/`](../pet_sim/) | 模拟器读同一 `sdcard/pet` 镜像 |
| [`tools/pet_ui_preview/`](../pet_ui_preview/) | HTML 构图预览（快捷键 3 = splash C） |
| [`desktop_pet/main/source/pet/`](../../desktop_pet/main/source/pet/) | 固件 `pet_res` / `pet_view` 消费产物 |

原目录名 `tools/pet_pack` 已废弃，请勿再引用。
