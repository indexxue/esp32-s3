# desktop_pet 产品约定

**版本**：0.4（卡根 `/sdcard/config` 上电读取）  
**日期**：2026-08-14  
**入口**：[`README.md`](README.md) · 术语 [`CONTEXT.md`](CONTEXT.md) · 引擎 [`framework.md`](framework.md) · 语音 [`cloud_asr.md`](cloud_asr.md)  
**预览**：`py -3 tools/pet_tool/serve.py` → 仅产品态 [`web/`](../../tools/pet_tool/web/)（开机 C / 主界面 / 对话 D）

本文合并原皮肤包 / 主界面 / 对话页约定与 ADR-0001～0004。

---

## A. 皮肤包与开机

### A.1 边界

| 类别 | 位置 | 谁写 | 换肤时 |
|------|------|------|--------|
| **卡根配置** | `/sdcard/config` | PC 预置 / 以后固件可改 | **不覆盖** |
| **皮肤包** | 内容根 `/sdcard/pet/` | PC 工具 / 拷贝工具产物 | 整树可替换 |
| **运行时数据** | 例：`/sdcard/record/` | 设备固件 | 不随皮肤包覆盖 |

硬规则：**固件是引擎，SD 是皮肤**；无卡或缺文件时 LVGL fallback。

### A.2 目录树

```
/sdcard/
  config              ← 卡根 KEY=VALUE；上电读取；换肤不覆盖
  pet/
    pack.bin
    body/*.bin          ← RGBH 身体帧
    boot/splash.bin     ← 开机单帧 RGBH（可选）
      theme/ui/*.bin      ← 按键/网络图标 RGBH（可选；缺则字母或绘制）
      record/               ← 运行时（固件写；目录可缺）
```

模拟器镜像：[`tools/pet_sim/sdcard/`](../../tools/pet_sim/sdcard/)（含 `config` + `pet/`）。`pack.bin` / `RGBH` 见 [`framework.md`](framework.md) §6。拷卡时拷整棵 `sdcard/` 到设备挂载根，不要只拷 `pet/`。

### A.3 合法路径与 reserved

| 相对路径（相对 `/sdcard/`） | 格式 | 缺失时 |
|----------|------|--------|
| `config` | UTF-8 `KEY=VALUE`（`#` 注释） | 默认 `content_root=pet`、`record_root=record`，仍扫描默认目录 |
| `pet/pack.bin` | `PETP` | 色块身体 + 默认 needs |
| `pet/body/<name>.bin` | `RGBH` | fallback |
| `pet/boot/splash.bin` | `RGBH`，建议 240×240 | LVGL 开机 fallback |
| `pet/theme/ui/{feed,play,sleep,chat}.bin` | `RGBH`，28×28（≤32） | 护理/聊按钮图标；缺则字母 |
| `pet/theme/ui/{wifi_on,wifi_off}.bin` | `RGBH`，28×28（≤32） | 主界面网络状态；缺则绘制扇形 |
| `pet/theme/ui/settings.bin` | `RGBH`，28×28（≤32） | 设置入口；缺则字母 S |
| `pet/font/caption.bin` | LVGL binary 字库（14px 建议） | 内置思源 CJK 子集（约 1k 字） |

`config` 首版键：`version`、`content_root`、`record_root`。未知键保留在内存。凡键名以 `_root` 结尾且值为相对路径的，上电后扫描该目录（深度 2）并记下文件名与大小。

Reserved（卡上不预建空目录；固件暂不读）：`pet/sfx/`、`pet/theme/` 下除 `ui/` 外、`pet/boot/anim/`。

字幕完整汉字：用 [`tools/pet_tool/font/make_caption_bin.py`](../../tools/pet_tool/font/make_caption_bin.py) 从 TTF 生成 `caption.bin`，拷/网页上传到 `pet/font/` 后**需重启**（开机 Splash 阶段加载进内存）。

### A.4 开机（视觉 C）

| 项 | 约定 |
|----|------|
| 路径 | `boot/splash.bin` **静态单帧**；与 `pack.bin` 解耦 |
| 视觉 | **C**：身体 + 环形进度（LVGL 弧可绑加载） |
| 工具 | 画布固定 240×240；背景/身体色与内容尺寸可调；`--fit contain|cover` |
| Splash Gate | ≥1000 ms 且皮肤 pack 加载尝试结束 → 再读字幕字库 → 进主界面；**不等** WiFi / agent |
| 动画 | `boot/anim/` reserved，**暂不做** |

### A.5 工具（`tools/pet_tool/`）

| 能力 | 状态 |
|------|------|
| `skin/skin_core.py` | 唯一写盘核心（bind/check、身体 / splash / theme UI） |
| `skin/cli.py` | `--bind` / `--import` / `--check` / `--splash` / `--theme-ui` / `--zip` |
| Qt GUI `skin/run_gui.py` | Splash / Design / Pack / Body / **Theme**；Pack 可 Export zip |
| Theme UI 图标 | `theme/ui/*.bin`；透明圆图（PNG 透明区不填色）或纯色圆；缺图字母 fallback |
| 网页预览 `serve.py` | 读 `skin/pack.json` + PNG；`http://127.0.0.1:8765/web/` |
| 网页换肤 | 设备 `GET /` 上传 `pet.zip` → 覆盖 `/sdcard/pet/`（**无字库时保留原 `font/`**）→ 重启 |
| 教程 | [`tools/pet_tool/README.md`](../../tools/pet_tool/README.md) · [`skin/README.md`](../../tools/pet_tool/skin/README.md) |

---

## B. 主界面

### B.1 目标

- 圆屏第一眼是宠；Needs 可扫视；喂/玩/睡可点；「聊」进对话页。
- 保持 `pet_evt` → `pet_core` → `pet_intent`。

### B.2 分层

```
背景 #202020
  身体 RGBH（ø140–160）
  身体热区（tap / hold→喂；收窄避开侧键）
  Needs 底弧三段（饥饿/心情/精力 %）
  网络状态（顶中偏右，ø200 安全圆内；STA 有 IPv4=在线）
  设置入口（WiFi 右侧偏下同弧）
  异常短提示（可选淡出）
  左侧护理弧：喂 / 玩 / 睡（独立圆钮，中钮更靠外）
  右侧 Chat：聊（独立圆钮）
```
五官 LVGL 叠加与表情演出：**预留，本阶段固件不创建、不显示**。

### B.3 Needs / 照料 / Chat / 网络 / 设置 / 文案

- Needs：底缘三条短直线（左→右 H/M/E）+ 百分比；NVS；约 30 s 消耗一拍。
- 网络：顶中偏右小图标（圆屏安全区内，勿贴矩形角）；`theme/ui/wifi_on.bin` / `wifi_off.bin`（可选，≤32）；缺则绘制扇形（在线青 / 离线灰）。判定：`net_wifi_sta_has_ipv4()`。**单击**：在线→断 STA，离线→重连（仅 STA 模式；SoftAP 配网态忽略）。
- 设置：WiFi 右侧偏下同弧；可选 `theme/ui/settings.bin`；缺则字母 S。进页隐藏主界面铬；竖直滚动列表（安全圆内缩）。首版：固件/皮肤包版本、MAC、网络/SSID/IP；默认英文，有 CJK 字库可切中文；触摸校准入口；换肤切换 reserved（soon，仍用网页）。
- 照料弧：ø≈28；左侧上移 F→P→S（喂/玩/睡）；可选 `theme/ui/{feed,play,sleep}.bin`。
- Chat：右侧独立 C；可选 `theme/ui/chat.bin`；样式与照料区分。
- 缺图标时字母 / 绘制 fallback；热区 `ext_click_area` 外扩，身体热区与侧键错开。
- 默认无常显状态句；`NO PACK` 等短提示后淡出。
- Debug 覆盖层非产品；GPIO0 单击默认无动作（见工程 README）。
- 触摸校准：设置页 / GPIO0 长按 / 网页；Needs↑ Dock↓ 确认后 12 点写入 NVS（见工程 README）。
- **五官 / 表情（预留，暂不做）**：`PET_FACE_*`、`PET_INTENT_FACE`、`PET_EVT_EMOTION`、`pack.bin` 五官锚点仍保留；`pet_view` **不叠眼/嘴/眉**。换肤只换身体（+可选按键/网络图标）。落地时见 [`framework.md`](framework.md) §6.4。

### B.4 延后

Care Feedback（移动/跟随等）；Chat 图标皮肤打磨；玩的分档 / 档内随机变体；设置页屏上换肤。

五官叠加 / 表情演出 / 眨眼：**暂不做**（引擎口预留，固件不画）。作者侧 Design 锚点可继续写 `pack.json`，设备本阶段忽略。冷戳/喂拒靠身体 `refuse`（缺则 `sad`）可感知。

对话字幕字体：读 `pet/font/caption.bin`（见 §A.3）；缺则内置 CJK 子集。

### B.5 照料分档（反应深度 · 首版）

喂 / 戳按 Needs **规则定档**（档内随机后置）。共享 one-shot clip `refuse`（prio 15 / 2 s）。细则与阈值见 [`framework.md`](framework.md) §5.1。

- 喂：`hunger ≥ 85` → `refuse`，不涨 hunger；否则 `eat`。
- 戳：睡中只醒；`mood < 30` → `refuse` 且不加 mood；否则 `poke`。
- 玩：本阶段仍一律 `play`（软惩后续）。

边界不变：Dock / 热区只 `pet_core_post`；禁止在按钮回调里直接改 clip。

---

## C. 对话页（框架 D）

### C.1 分层

```
左上返回
顶中模式字（听/说/连接中/就绪）
大宠身体（单击切换听↔等答；说中单击打断再听）
字幕条（当前一轮）
波形（听时动）
```

无 Needs、无照料 Dock。可交互评估稿：[`tools/pet_tool/web/chat.html`](../../tools/pet_tool/web/chat.html)。

### C.2 生命周期

| 事件 | 行为 |
|------|------|
| 进页（右侧「聊」） | **只连会话**（不自动听）；提示 tap to talk |
| 单击宠身体 | **切换**：未听→开始听；听中→停听等答（`listen stop` → STT/LLM/TTS）；说中→打断再听 |
| 返回 / 可选 GPIO0 | **离开即停**（停听停播结束会话） |
| ≈45s 无活动 | 同上（听/说/STT 重置计时） |

字幕：听=STT，说=助手当前句；不滚长历史。「助手再说一遍」未约定。

### C.3 引擎边界

听/说经 `pet_core_post`；禁止 agent 直接画表情。协议细节见 [`cloud_asr.md`](cloud_asr.md)。

---

## D. 决策记录（原 ADR）

### D.1 皮肤包边界 — accepted

皮肤统一挂 `/sdcard/pet/`；卡根 `/sdcard/config` 不属于皮肤包（换肤不覆盖）；开机 `boot/splash.bin` 静态单帧 RGBH，与 pack 解耦；卡上不放 PNG；工具在 `pet_skin`。拒绝：把设备配置塞进 pack、卡上 PNG、多根散落。

### D.2 主界面构图 — accepted

宠为主体；Needs 底弧；**左弧照料三钮 + 右侧 Chat**；无常显状态字；无产品 debug 页。
可选 `theme/ui` 按键图标。五官 LVGL 叠加为后续项，本阶段不显示。

（历史「底弧四钮」已废弃，以本节与 §B 为准。）

### D.5 五官锚点 + 分件旋转 — accepted（显示延后）

不采用「五官钉死屏中心」或「整脸烤进 PNG」。采用每帧五官锚点；标定在 `pet_skin`，不在生图侧。每个五官（眼 L/R、嘴、眉 L/R）均可独立 `angle`。缺省兼容旧包中心对齐。

**本阶段**：方案保留，固件不画五官、不做表情演出。

### D.3 开机 C + 对话 D — accepted

开机视觉 C；对话框架 D（语音优先）。

### D.4 对话 D 交互 — accepted

进入只连会话；单击切换听/等答；离开即停；当前一轮字幕；45s 超时。

---

## E. 实现顺序（建议）

1. ~~`pet_skin` 写出 splash；固件 splash + Gate。~~ **已完成**
2. ~~`pet_view`：Needs、照料弧 + Chat、字母/图标、热区；隔离 debug；触摸校准。~~ **已完成（主界面骨架）**
3. ~~对话页空壳 → 接 agent（单击听↔等答、字幕、45s）。~~ **已完成骨架**
4. 补齐 `ui_chat` 图标与皮肤包打磨；板测校准一次并固化 NVS（量产可预写或出厂向导）。
5. **五官/表情（预留）**：`pet_view` 打开叠加 + 锚点应用；此前固件只画身体。
6. Care Feedback / 玩分档 / 历史页等专项。
7. ~~对话页换肤 / `pet/font` 中文字幕字体。~~ **已接 SD `font/caption.bin`**（缺则内置子集；换肤主题另做）
8. ~~照料分档（喂+戳 + `refuse`）。~~ **引擎规则见 framework §5.1**；皮肤补 `refuse` 帧可选（缺则 sad）

## F. 变更规则

- 改合法路径、主界面骨架、对话进出/打断/字幕：先改**本文**与 [`CONTEXT.md`](CONTEXT.md)，再改预览与固件。
- 引擎事件/`pack.bin` 二进制：改 [`framework.md`](framework.md)。
- 云端协议：改 [`cloud_asr.md`](cloud_asr.md)。
