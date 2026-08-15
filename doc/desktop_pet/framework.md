# desktop_pet 框架地基

**版本**：1.3  
**日期**：2026-08-15  
**工程**：[`desktop_pet/`](../../desktop_pet/)  
**状态**：引擎 + 资源包 + 分层表演；照料分档（喂/戳）；语音会话只留事件口  
**文档集**：[`README.md`](README.md) · [`CONTEXT.md`](CONTEXT.md) · [`product.md`](product.md) · [`cloud_asr.md`](cloud_asr.md)

---

## 1. 三条硬规则

1. **`pet_core` 零 LVGL、零 ESP-IDF、零 `board.h`**。板端与 `lvgl_sim` 编同一份大脑。
2. **固件是引擎，SD 是皮肤**。clip 帧与衰减表来自 `/sdcard/pet/`；无卡时色块身体，不空屏。LVGL 五官叠加 **reserved**（本阶段不画）。
3. **输入只产生事件，输出只消费意图**。UI / 灯 / 电机禁止互相直调。会话模块以后只 `pet_core_post(PET_EVT_LISTEN)`。

---

## 2. 模块与依赖

| 模块 | 路径 | 允许依赖 |
|------|------|----------|
| 大脑 | [`desktop_pet/main/source/pet/pet_core/`](../../desktop_pet/main/source/pet/pet_core/) | C 标准库 |
| 文件系统 | [`desktop_pet/main/source/pet/pet_fs.h`](../../desktop_pet/main/source/pet/pet_fs.h) | `stdio` |
| 卡根配置 | [`desktop_pet/main/source/pet/sd_cfg.h`](../../desktop_pet/main/source/pet/sd_cfg.h) | `stdio` + POSIX/`dirent`（Win32 模拟器用 `FindFirstFile`） |
| 资源包 | [`desktop_pet/main/source/pet/pet_res/`](../../desktop_pet/main/source/pet/pet_res/) | `pet_fs`、`pet_core` |
| 表演 | [`desktop_pet/main/source/pet/pet_view/`](../../desktop_pet/main/source/pet/pet_view/) | LVGL 9、`pet_core`、`pet_res` |
| 板端口 | [`desktop_pet/main/source/ui.c`](../../desktop_pet/main/source/ui.c) | 板级 + 上列模块 |
| 模拟器端口 | [`tools/pet_sim/`](../../tools/pet_sim/) | 上列模块 + Windows LVGL 模拟器 |
| 作者工具伞 | [`tools/pet_tool/`](../../tools/pet_tool/) | `skin` 打包 · `web` 预览 · `font` 字库 |

`common/` 不放宠物逻辑（其它工程用不上）。

```
触摸 / 按键 / IMU / 1s tick / （预留 agent）
        │  pet_evt
        ▼
     pet_core  →  needs + clip FSM  →  pet_intent
        │
        ├─ pet_view（CLIP / FACE / HUD）
        └─ 板端 hook（LED / MOTOR / SFX）
```

---

## 3. 事件（输入）

| id | 含义 | 谁投 |
|----|------|------|
| `PET_EVT_TICK_1S` | 衰减 / 睡眠恢复 / clip 倒计时 | `pet_view` 100 ms 定时器每 10 次 |
| `PET_EVT_TOUCH_TAP` | 点身体 | view 点身体 |
| `PET_EVT_TOUCH_HOLD` | 长按身体 → 喂食 | view |
| `PET_EVT_IMU_SHAKE` | 晃 → 玩 | 板端 IMU |
| `PET_EVT_IMU_FLIP` | 翻转静置 → 睡 | 板端 IMU |
| `PET_EVT_CARE_FEED` / `PLAY` / `SLEEP` / `WAKE` | 照料 | 左弧 / 键盘 / 手势 |
| `PET_EVT_LISTEN` / `SPEAK` / `EMOTION` | 预留小智 | 对话页 / agent；`arg0` 可为 face id |

`pet_core_post()` 非线程安全：只在 LVGL/`pet_view` 任务投递。

---

## 4. 意图（输出）

| id | `arg0` | 消费者 |
|----|--------|--------|
| `PET_INTENT_CLIP` | `pet_clip_id_t` | `pet_view` 换身体帧 |
| `PET_INTENT_FACE` | `pet_face_id_t` | 预留：`pet_view` 消费但不画五官 |
| `PET_INTENT_HUD` | —（读 `pet_core_get_needs`） | Needs 底弧 |
| `PET_INTENT_LED` | 0=eat 1=play | `led_scene` |
| `PET_INTENT_MOTOR` | 预留 | 未接 TB6612 |
| `PET_INTENT_SFX` | 预留 | 未接 |
| `PET_INTENT_OPEN_CHAT` | — | 右侧「聊」；`pet_view` 直投 hook，不经 core |

---

## 5. Needs 与 clip

三格 0–100%：`hunger` / `mood` / `energy`。无死亡、无进化；掉到 0 只切 `sad` / `sleepy`。值经 NVS（`pet_needs`）持久化。

消耗在 **30 s** 节拍上结算（非每秒）：默认饥饿 900 s/%、心情 1200 s/%、精力 1500 s/%；睡觉时精力 180 s/% 恢复。pack 可覆盖。HUD 随消耗节拍或照料动作刷新。

| clip | 优先级 | 时长 | 触发 |
|------|--------|------|------|
| `idle` / `sleepy` / `sad` / `sleep_loop` | 0 | 循环 | needs / 睡眠 |
| `poke` | 10 | 1 s | 点身体（心情尚可） |
| `refuse` | 15 | 2 s | 喂拒 / 冷戳；缺包帧时 `pet_res` fallback → `sad` |
| `eat` / `play` | 20 | 3 s / 4 s | 喂（未拒）/ 玩；播完前回不去低优先级 |

### 5.1 照料分档（反应深度 · 首版喂+戳）

选型：**规则定档**（Needs 区间）+ **档内随机后置**；clip 采用**混合挂载**（复用 `eat`/`poke`，共享 one-shot `refuse`）。玩分档与 LED/SFX 强度后续专项。

**喂**（看动作前 `hunger`；长按身体与照料「喂」同一路径）

| 档 | 条件 | clip / face | Needs |
|----|------|-------------|-------|
| 爽 | `< 40` | `eat` + happy | hunger/mood 照常加 |
| 平 | `40–84` | `eat` | 照常加 |
| 拒 | `≥ 85` | `refuse` + sad | **不涨** hunger；mood 不变 |

**戳**

| 档 | 条件 | clip / face | Needs |
|----|------|-------------|-------|
| 醒 | 正在睡 | 无 poke；只 wake → idle | 仅醒 |
| 平 | 醒着且 mood `≥ 30` | `poke` + happy | mood 小加（`tap_mood`） |
| 冷 | 醒着且 mood `< 30` | `refuse` + sad | mood **不加** |

玩（未分档）：仍 `play`；累玩软惩等后续。五官本阶段不画，冷/拒靠 `refuse` 身体帧可感知。

眨眼由 `pet_view` 本地定时，不占 clip（**本阶段不画五官，眨眼也不做**）。

五官 id 预留：`idle` / `happy` / `sad` / `sleepy` / `hungry` / `angry`。`pet_core` 仍发 `PET_INTENT_FACE`；`pet_view` 本阶段不叠眼/嘴/眉。

---

## 6. `pack.bin` 布局（小端）

设备**不解析 JSON**。作者侧 [`pack.json`](../../tools/pet_tool/skin/pack.json) → `skin_core` / `cli.py` → `pack.bin` + `body/*.bin`。

### 6.1 文件头 20 字节

| 偏移 | 类型 | 内容 |
|------|------|------|
| 0 | 4 ASCII | `PETP` |
| 4 | u16 | version = 1 |
| 6 | u16 | flags = 0 |
| 8 | u16 | hunger_decay_s |
| 10 | u16 | mood_decay_s |
| 12 | u16 | energy_decay_s |
| 14 | u16 | energy_recover_s |
| 16 | u8 | feed_hunger |
| 17 | u8 | play_mood |
| 18 | u8 | tap_mood |
| 19 | u8 | clip_count |

### 6.2 clip 记录

每条：`clip_id` u8、`frame_count` u8、`fps` u8、`reserved` u8，随后 `frame_count` 个 **32 字节** 相对路径（ASCII、NUL 填充），例如 `body/idle_0.bin`。

`clip_id` 与 `pet_clip_id_t` 一致：0 idle，1 sleepy，2 eat，3 play，4 sad，5 sleep_loop，6 poke，7 refuse。旧包无 `refuse` 时加载器回退到 `sad` 帧。

### 6.3 身体帧 `.bin`（RGBH）

12 字节头 + RGB565 行优先。

| 偏移 | 内容 |
|------|------|
| 0 | `RGBH` |
| 4 | u16 width |
| 6 | u16 height |
| 8 | u32 flags（0 = 行优先） |
| 12 | `width * height * 2` 字节 RGB565 |

建议边长 ≤ 180。PSRAM 双缓冲最多 2 帧。无 alpha；画面外像素填屏背景色 `0x202020`。

### 6.4 五官锚点（约定；显示延后）

**本阶段**：二进制与作者侧字段保留；`pet_view` **不应用、不画五官**。打开叠加时按下列约定落地。

**目标**：五官不钉死屏中心；躺/趴等姿势由每帧锚点驱动 LVGL 眼/嘴/眉的位置与旋转。标定只在作者侧工具完成，设备只读二进制。

#### 作者侧 `pack.json`（每 clip 或每帧）

相对**该身体帧位图左上角**，单位像素；`angle` 为度，有符号（顺时针为正，与实现时 LVGL transform 约定在落地时对齐一次即可）。

```json
"face": {
  "eye_l":  { "x": 62, "y": 72,  "angle": 0 },
  "eye_r":  { "x": 98, "y": 72,  "angle": 0 },
  "mouth":  { "x": 80, "y": 108, "angle": 12 },
  "brow_l": { "x": 62, "y": 56,  "angle": -8 },
  "brow_r": { "x": 98, "y": 56,  "angle": 8 }
}
```

- 整 clip 共用：字段名 `face`。  
- 逐帧不同：`faces` 数组，长度 = `frames`，元素同上。  
- **每个五官独立 `angle`**（可只有嘴歪、或侧躺时五官都转约 90°）。  
- 省略任一零件 → 该零件用内置默认相对偏移 + `angle=0`。  
- 整段省略 → 行为与 version 1 相同（屏中心固定叠脸）。

工具：[`tools/pet_tool/skin/`](../../tools/pet_tool/skin/) Design 预览上拖拽位置、旋转角度后写回 JSON；**禁止**要求生图 AI 输出锚点。

#### `pack.bin`（version ≥ 2）

文件头 `version` 为 `2`。每帧在 32 字节路径之后追加 **32 字节** 五官块（小端）：

| 偏移 | 类型 | 内容 |
|------|------|------|
| 0 | i16×3 | `eye_l`：x, y, angle_deg |
| 6 | i16×3 | `eye_r` |
| 12 | i16×3 | `mouth` |
| 18 | i16×3 | `brow_l` |
| 24 | i16×3 | `brow_r` |
| 30 | u16 | reserved = 0 |

落地后 `pet_view` 换帧：身体图左上角 + 锚点 → 各 LVGL 对象中心；并对该对象施加对应旋转。表情形状仍由 `PET_FACE_*` 控制；眨眼只改眼高，不改锚点。本阶段跳过此步。

version 1 包：无五官块，继续中心对齐。

卡上目录（产品路径见 [`product.md`](product.md) §A）：

```
/sdcard/config
/sdcard/pet/pack.bin
/sdcard/pet/body/idle_0.bin
/sdcard/pet/boot/splash.bin
```

模拟器镜像：[`tools/pet_sim/sdcard/`](../../tools/pet_sim/sdcard/)（含卡根 `config`）。  
`/sdcard/record/` 为运行时数据，不属于皮肤包。`/sdcard/config` 换肤不覆盖。

---

## 7. 圆屏分层

产品目标分层见 [`product.md`](product.md) §B / §C。摘要：

```
Needs 底弧 → 身体（无五官）→ 左弧喂/玩/睡 + 右侧聊
对话页 D：大身体 + 字幕 + 波形（独立页；五官预留）
```

主界面：Needs + 左护理弧 + 右 Chat；可选 `theme/ui` 图标；debug 覆盖层由 `DESKTOP_PET_ENABLE_DEBUG_UI` 隔离（默认关）。

---

## 8. 与语音计划的边界

- 本框架不实现 WebSocket / Opus / ESP-SR（见 [`cloud_asr.md`](cloud_asr.md)）。
- agent 投 `PET_EVT_LISTEN` / `SPEAK` / `EMOTION`，由 `pet_core` 改 FACE/clip，**禁止**在 agent 里画表情。
- GPIO0：默认不切 debug（`DESKTOP_PET_ENABLE_DEBUG_UI`）；长按触摸校准；产品单击语义另定（[`product.md`](product.md) §B）。
- `/sdcard/record/` 不进入 `pet_core`。

---

## 9. 模拟器

1. `powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1`
2. `powershell -ExecutionPolicy Bypass -File .\tools\pet_sim\install_into_lvgl_sim.ps1`
3. 用 Visual Studio 打开 `tools\lvgl_sim\lv_port_pc_visual_studio\LVGL.sln`，启动 `LvglWindowsSimulator`（240×240）。
4. 键盘 `1` 喂、`2` 玩、`3` 睡、`4` 醒。工作目录为仓库根，才能读到 `tools/pet_sim/sdcard/pet`。

重新生成资源：见 [`tools/pet_tool/skin/README.md`](../../tools/pet_tool/skin/README.md)。

```powershell
py -3 tools/pet_tool/skin/run_gui.py
# 或 CLI：绑定 assets/ 后打 pack + splash + UI icons
py -3 tools/pet_tool/skin/cli.py --bind --splash --theme-ui
```

GUI 扩展：在 `tools/pet_tool/skin/app/features/` 新增 `FeatureModule`，登记到 `registry.built_in_features()`；写盘只调用 `skin_core`。开机仅静态 splash。

---

## 10. 板端操作

| 输入 | 效果 |
|------|------|
| 左弧 F / 长按身体 | 喂 |
| 左弧 P / 晃一下 | 玩 |
| 左弧 S / 翻转约 1 s | 睡 |
| 点身体 | poke（睡觉则先醒） |
| GPIO0 单击 | 默认无动作；`DESKTOP_PET_ENABLE_DEBUG_UI=1` 时切 debug |
| GPIO0 长按 | 触摸校准（进行中单击可取消） |
| 右侧 C「聊」 | 对话页 D：进页连会话 / 单击听↔等答 / 45s；字幕接 STT·TTS |
