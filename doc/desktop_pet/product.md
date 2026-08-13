# desktop_pet 产品约定

**版本**：0.2（文档已集中；固件改版未完成）  
**日期**：2026-08-13  
**入口**：[`README.md`](README.md) · 术语 [`CONTEXT.md`](CONTEXT.md) · 引擎 [`framework.md`](framework.md) · 语音 [`cloud_asr.md`](cloud_asr.md)  
**预览**：[`tools/pet_ui_preview/index.html`](../../tools/pet_ui_preview/index.html)

本文合并原皮肤包 / 主界面 / 对话页约定与 ADR-0001～0004。

---

## A. 皮肤包与开机

### A.1 边界

| 类别 | 位置 | 谁写 | 换肤时 |
|------|------|------|--------|
| **皮肤包** | 内容根 `/sdcard/pet/` | PC 工具 / 拷贝工具产物 | 整树可替换 |
| **运行时数据** | 例：`/sdcard/record/` | 设备固件 | 不随皮肤包覆盖 |

硬规则：**固件是引擎，SD 是皮肤**；无卡或缺文件时 LVGL fallback。

### A.2 目录树

```
/sdcard/pet/
  pack.bin
  body/*.bin          ← RGBH 身体帧
  boot/splash.bin     ← 开机单帧 RGBH（可选）
```

模拟器镜像：[`tools/pet_sim/sdcard/pet/`](../../tools/pet_sim/sdcard/pet/)。`pack.bin` / `RGBH` 二进制见 [`framework.md`](framework.md) §6。

### A.3 合法路径与 reserved

| 相对路径 | 格式 | 缺失时 |
|----------|------|--------|
| `pack.bin` | `PETP` | 色块身体 + 默认 needs |
| `body/<name>.bin` | `RGBH` | fallback |
| `boot/splash.bin` | `RGBH`，建议 240×240 | LVGL 开机 fallback |

Reserved（卡上不预建空目录；固件暂不读）：`sfx/`、`font/`、`theme/`、`boot/anim/`。

### A.4 开机（视觉 C）

| 项 | 约定 |
|----|------|
| 路径 | `boot/splash.bin` **静态单帧**；与 `pack.bin` 解耦 |
| 视觉 | **C**：身体 + 环形进度（LVGL 弧可绑加载） |
| 工具 | 画布固定 240×240；背景/身体色与内容尺寸可调；`--fit contain|cover` |
| Splash Gate | ≥1000 ms 且皮肤加载尝试结束；不等 WiFi/agent |
| 动画 | `boot/anim/` reserved，**暂不做** |

### A.5 工具（`tools/pet_skin/`）

| 能力 | 状态 |
|------|------|
| `skin_core.py` | 唯一写盘核心（色盘 / 图片身体 / splash） |
| `cli.py` | CLI（含 `--splash`） |
| Qt GUI `run_gui.py`（PySide6） | 侧栏含 **Design**（涂鸦/导入） |
| 索引 | [`tools/pet_skin/README.md`](../../tools/pet_skin/README.md) |

---

## B. 主界面

### B.1 目标

- 圆屏第一眼是宠；Needs 可扫视；喂/玩/睡可点；「聊」进对话页。
- 保持 `pet_evt` → `pet_core` → `pet_intent`。

### B.2 分层

```
背景 #202020
  身体 RGBH（ø140–160）+ LVGL 五官
  身体热区（tap / hold→喂）
  Needs 三点（顶中）
  异常短提示（可选淡出）
  Dock：喂 / 玩 / 睡 / 聊（底弧）
```

### B.3 Needs / Dock / 文案

- Needs：顶中三点，绿/蓝/黄 = 饥饿/心情/精力。
- Dock：ø≈28、间距≈8；顺序喂→玩→睡→聊；聊样式区分。
- 默认无常显状态句；`NO PACK` 等短提示后淡出。
- Debug 覆盖层非产品；GPIO0 原切 debug 语义作废后另定。
- 五官：LVGL 叠加；换肤先换身体。

### B.4 延后

Care Feedback（移动/跟随等）；Dock 图标像素稿。

---

## C. 对话页（框架 D）

### C.1 分层

```
左上返回
顶中模式字（听/说/连接中）
大宠脸（可点：中断重讲）
字幕条（当前一轮）
波形（听时动）
```

无 Needs、无照料 Dock。

### C.2 生命周期

| 事件 | 行为 |
|------|------|
| 进页（Dock「聊」） | **进入即听** |
| 返回 / 可选 GPIO0 | **离开即停**（停听停播结束会话） |
| ≈45s 无活动 | 同上（听/说/STT 重置计时） |
| TTS 中说话或点脸 | **中断重讲** → 停播回听 |

字幕：听=STT，说=助手当前句；不滚长历史。「助手再说一遍」未约定。

### C.3 引擎边界

听/说经 `pet_core_post`；禁止 agent 直接画表情。协议细节见 [`cloud_asr.md`](cloud_asr.md)。

---

## D. 决策记录（原 ADR）

### D.1 皮肤包边界 — accepted

皮肤统一挂 `/sdcard/pet/`；开机 `boot/splash.bin` 静态单帧 RGBH，与 pack 解耦；卡上不放 PNG；工具在 `pet_skin`。拒绝：塞进 pack、卡上 PNG、多根散落。

### D.2 主界面构图 — accepted

宠为主体；Needs 三点；底弧四钮；无常显状态字；无产品 debug 页；五官 LVGL 叠加。

### D.3 开机 C + 对话 D — accepted

开机视觉 C；对话框架 D（语音优先）。

### D.4 对话 D 交互 — accepted

进入即听；离开即停；可打断/点脸重讲；当前一轮字幕；45s 超时。

---

## E. 实现顺序（建议）

1. `pet_skin` 写出 splash；固件 splash + Gate。
2. `pet_view`：三点 Needs、四钮 Dock、去常显状态字；去/隔离 debug。
3. 对话页空壳 → 接 agent（进听出停、字幕、打断、45s）。
4. Care Feedback / 图标 / 历史页等专项。

## F. 变更规则

- 改合法路径、主界面骨架、对话进出/打断/字幕：先改**本文**与 [`CONTEXT.md`](CONTEXT.md)，再改预览与固件。
- 引擎事件/`pack.bin` 二进制：改 [`framework.md`](framework.md)。
- 云端协议：改 [`cloud_asr.md`](cloud_asr.md)。
