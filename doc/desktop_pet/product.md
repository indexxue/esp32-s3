# desktop_pet 产品约定

**版本**：0.7（卡根 config；唤醒词 SD 模型；可选 UI 铬 `lang/` + 照料提示音 `sfx/`）  
**日期**：2026-08-19  
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
    font/caption.bin    ← 字幕字库（可选）
    lang/{en,zh}.txt    ← UI 铬文案（可选；覆盖固件底表）
    sfx/<clip>/*.wav    ← 照料提示音（可选；每 clip 多句随机抽 1）
  sr_models/            ← ESP-SR 唤醒模型整包（可选；缺则无唤醒，正常开机）
  sfx/wake/             ← 唤醒回复：可选 ding + 多句短语音（可选；缺则静默仍开听）
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
| `pet/lang/en.txt` | UTF-8 `KEY=VALUE`（`#` 注释） | 固件 EN 底表 |
| `pet/lang/zh.txt` | 同上 | 固件 ZH 底表 |
| `pet/sfx/<clip>/*.wav` | 16 kHz mono 16-bit PCM WAV | 该 clip 静默 |
| `sr_models/<wake_model>/` | ESP-SR 模型目录（含 `_MODEL_INFO_` 等） | **不做唤醒**；其它功能正常 |
| `sfx/wake/*.wav` | 16 kHz mono 16-bit PCM WAV | 无回复音；仍开对话并自动听 |

`config` 键：`version`、`content_root`、`record_root`；唤醒相关（开机加载，缺省见下表）。未知键保留在内存。凡键名以 `_root` 结尾且值为相对路径的，上电后扫描该目录（深度 2）并记下文件名与大小。

| `config` 键 | 默认 | 说明 |
|-------------|------|------|
| `wake_enable` | `1` | `0` 关闭唤醒检测 |
| `wake_model` | `wn9s_hilexin` | WakeNet 模型目录名（相对 `wake_model_path`） |
| `wake_label` | `嗨乐鑫` | 展示/日志用；**不参与识别** |
| `wake_model_path` | `sr_models` | 模型根目录，相对 SD 挂载根 |
| `wake_reply_path` | `sfx/wake` | 唤醒回复音目录，相对 SD 挂载根；**不属于皮肤包** |
| `screen_blank_s` | `60` | 空闲息屏秒数；`0`=关闭 |

无卡、缺 `sr_models/<wake_model>/`、或模型加载失败 → 打日志，**正常启动且不做唤醒识别**。真·自定义唤醒词需替换/增加 SD 上的已训模型，并改 `wake_model`（不能只改中文标签）。

**唤醒回复 (Wake Reply，D15)**：命中唤醒并开对话页后、**开听之前**播放本地反馈（B+C）：

1. 若目录内存在 `ding.wav`（大小写不敏感）→ **先播叮一声**  
2. 其余 `*.wav` 为短句池 → **随机抽 1 句**播完（池 ≥2 时尽量不连播同一文件）  
3. 仅有 ding、或仅有短句、或目录空/缺文件 → 有则播、无则**静默**；然后仍 `listen_start`  
4. 播报期间**不开上行麦**（避免把自己录进 STT）

建议短句文案（作者录进 wav；文件名 8.3 友好即可）：

| 建议文件名 | 口播 |
|------------|------|
| `ding.wav` | （提示音，非语音） |
| `zaine.wav` | 在呢 |
| `wozai.wav` | 我在 |
| `en.wav` | 嗯？ |
| `laile.wav` | 来了 |
| `zenme.wav` | 怎么啦 |

`lang/*.txt` 按 key **覆盖**固件内建 UI 铬表（设置/对话模式/短提示）；未知 key 忽略。用户语种偏好在 NVS，不在皮肤包。

**照料提示音 (Care SFX)**：`pet/sfx/<clip>/` 下可放多句 `.wav`（建议 ≤8；文件名 8.3 友好）。触发跟身体 clip：`eat` / `play` / `poke` / `refuse` / `sleep`（引擎 `SLEEP_LOOP`→目录 `sleep`）。每次只播一句：随机；池 ≥2 时尽量不连播同一文件。可打断上一句 Care SFX；对话页听/说中不播。缺目录或空 → 静默。网页换肤 zip **无**任一 `sfx/**/*.wav` 时保留卡上原 `pet/sfx/`。

首版 key 表（与固件 `pet_i18n` / [`tools/pet_sim/sdcard/pet/lang/`](../../tools/pet_sim/sdcard/pet/lang/) 对齐）：

| Key | EN 默认 | ZH 默认 |
|-----|---------|---------|
| `settings.title` | Settings | 设置 |
| `settings.firmware` | Firmware | 固件 |
| `settings.skin_pack` | Skin pack | 皮肤包 |
| `settings.network` | Network | 网络 |
| `settings.battery` | Battery | 电量 |
| `settings.language` | Language | 语言 |
| `settings.skin` | Skin | 皮肤 |
| `settings.touch_calib` | Touch calib | 触摸校准 |
| `settings.soon` | soon | 即将 |
| `settings.net_off` | off | 关闭 |
| `settings.net_ap` | AP | 热点 |
| `settings.net_online` | online | 在线 |
| `settings.net_offline` | offline | 离线 |
| `settings.chg` | chg | 充 |
| `settings.lang_en` | EN | EN |
| `settings.lang_zh` | 中文 | 中文 |
| `chat.tap_to_talk` | tap to talk | 点按说话 |
| `chat.mode_listen` | listen | 听 |
| `chat.mode_speak` | speak | 说 |
| `chat.mode_connecting` | connecting | 连接中 |
| `chat.mode_ready` | ready | 就绪 |
| `home.no_pack` | NO PACK | 无皮肤 |

Reserved（卡上不预建空目录；固件暂不读）：`pet/theme/` 下除 `ui/` 外、`pet/boot/anim/`。

字幕完整汉字：用 [`tools/pet_tool/font/make_caption_bin.py`](../../tools/pet_tool/font/make_caption_bin.py) 从 TTF 生成 `caption.bin`，拷/网页上传到 `pet/font/` 后**需重启**（开机 Splash 阶段加载进内存）。

### A.4 开机（视觉 C）

| 项 | 约定 |
|----|------|
| 路径 | `boot/splash.bin` **静态单帧**；与 `pack.bin` 解耦 |
| 视觉 | **C**：身体 + 环形进度（LVGL 弧可绑加载） |
| 工具 | 画布固定 240×240；背景/身体色与内容尺寸可调；`--fit contain|cover` |
| Splash Gate | ≥1000 ms 且皮肤 pack 加载尝试结束 → 再读字幕字库与 UI 铬 `lang/` → 进主界面；**不等** WiFi / agent |
| 动画 | 位图仍单帧；LVGL 淡入 / 宠图轻呼吸 / 环旋转 / 短淡出。`boot/anim/` reserved，**暂不做** |

### A.5 工具（`tools/pet_tool/`）

| 能力 | 状态 |
|------|------|
| `skin/skin_core.py` | 唯一写盘核心（bind/check、身体 / splash / theme UI） |
| `skin/cli.py` | `--bind` / `--import` / `--check` / `--splash` / `--theme-ui` / `--zip` |
| Qt GUI `skin/run_gui.py` | Splash / Design / Pack / Body / **Theme**；Pack 可 Export zip |
| Theme UI 图标 | `theme/ui/*.bin`；透明圆图（PNG 透明区不填色）或纯色圆；缺图字母 fallback |
| 网页预览 `serve.py` | 读 `skin/pack.json` + PNG；`http://127.0.0.1:8765/web/` |
| 网页换肤 | 设备 `GET /` 上传 `pet.zip` → 覆盖 `/sdcard/pet/`（无 `font/caption.bin` 保留 `font/`；无 `lang/` 保留 `lang/`；无 `sfx/**/*.wav` 保留 `sfx/`）→ 重启 |
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
  电量状态（WiFi 左侧偏上同弧）
  设置入口（WiFi 右侧偏下同弧）
  异常短提示（可选淡出）
  左侧护理弧：喂 / 玩 / 睡（独立圆钮，中钮更靠外）
  右侧 Chat：聊（独立圆钮）
```
五官 LVGL 叠加与表情演出：**预留，本阶段固件不创建、不显示**。

### B.3 Needs / 照料 / Chat / 网络 / 设置 / 文案

- Needs：底缘三条短直线（左→右 H/M/E）+ 百分比；NVS；约 30 s 消耗一拍。
- 网络：顶中偏右小图标（圆屏安全区内，勿贴矩形角）；`theme/ui/wifi_on.bin` / `wifi_off.bin`（可选，≤32）；缺则绘制扇形（在线青 / 离线灰）。判定：`net_wifi_sta_has_ipv4()`。**单击**：在线→断 STA，离线→重连（仅 STA 模式；SoftAP 配网态忽略）。
- 电量：WiFi 左侧偏上同弧电池图形；读 `battery_percent_update` / `battery_info_read`；绿≥50% / 黄≤50% / 红≤20%；充电青色。缺采样时灰空壳。
- 设置：WiFi 右侧偏下同弧；可选 `theme/ui/settings.bin`；缺则字母 S。进页隐藏主界面铬；竖直滚动列表（安全圆内缩）。首版：固件/皮肤包版本、MAC、网络/SSID/IP、电量%；默认英文，有 CJK 字库可切中文（`pet_ui` NVS 记住）；文案来自固件底表，可选 `lang/{en,zh}.txt` 按 key 覆盖；触摸校准入口；换肤切换 reserved（soon，仍用网页）。
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
| 进页（主界面唤醒「嗨乐鑫」） | 开对话页 + **自动开听**；点身体仍可听/停切换（方案 γ） |
| 单击宠身体 | **切换**：未听→开始听；听中→停听等答（`listen stop` → STT/LLM/TTS）；说中→打断再听 |
| 返回 / 可选 GPIO0 | **离开即停**（停听停播结束会话）；回主界面后恢复唤醒检测 |
| ≈45s 无活动 | 同上（听/说/STT 重置计时） |

字幕：听=STT，说=助手当前句；不滚长历史。「助手再说一遍」未约定。

### C.2.1 唤醒（主界面）

| 项 | 约定 |
|----|------|
| 生效范围 | **仅主界面**；进对话页 / 设置页停唤醒检测 |
| 口令 | 默认 **嗨乐鑫**（模型 `wn9s_hilexin`）；模型整包在 SD `sr_models/` |
| 兜底入口 | 右侧「聊」保留（无模型/嘈杂/调试） |
| SPEAKING | **不可**用唤醒打断（对话页内本就不跑唤醒） |
| 唤醒后回复 | **叮一声 + 本地短句**（B+C）；资源在 SD；缺文件则静默后仍自动听；见 [`cloud_asr.md`](cloud_asr.md) D15 |

协议与模块见 [`cloud_asr.md`](cloud_asr.md) M3。

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

「聊」进入只连会话；单击切换听/等答；离开即停；当前一轮字幕；45s 超时。

### D.6 主界面唤醒进对话 — accepted（实现待 M3）

仅主界面 WakeNet；口令默认嗨乐鑫；模型在 SD，缺则无唤醒仍可启动；唤醒 → 开页并自动听（γ）；「聊」保留；说中不可唤醒打断；唤醒后 **叮 + 随机短句**（SD `sfx/wake/`，缺则静默，D15）。

### D.7 息屏 — accepted

默认 **60 s** 空闲关背光（`screen_blank_s`；`0`=关）。任意界面可进；**听 / 说 / 连接中**禁止。触摸 → 仅亮屏（吞掉该次按下）。语音唤醒 → 亮屏 + γ（开对话 + 叮/短句 + 自动听）。与宠物 `sleeping` 正交。

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
9. **M3 唤醒**：SD `sr_models` + 主界面 WakeNet → 叮/短句回复（`sfx/wake`）→ 对话页自动听。

## F. 变更规则

- 改合法路径、主界面骨架、对话进出/打断/字幕：先改**本文**与 [`CONTEXT.md`](CONTEXT.md)，再改预览与固件。
- 引擎事件/`pack.bin` 二进制：改 [`framework.md`](framework.md)。
- 云端协议：改 [`cloud_asr.md`](cloud_asr.md)。
