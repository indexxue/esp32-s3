# desktop_pet 框架地基

**版本**：1.0  
**日期**：2026-08-13  
**工程**：[`desktop_pet/`](../desktop_pet/)  
**状态**：引擎 + 资源包 + 分层表演；语音会话只留事件口  
**并列文档**：[`desktop_pet_cloud_asr_plan.md`](desktop_pet_cloud_asr_plan.md)（M2 流式对话，不画表情）

---

## 1. 三条硬规则

1. **`pet_core` 零 LVGL、零 ESP-IDF、零 `board.h`**。板端与 `lvgl_sim` 编同一份大脑。
2. **固件是引擎，SD 是皮肤**。clip 帧与衰减表来自 `/sdcard/pet/`；无卡时色块身体 + LVGL 五官，不空屏。
3. **输入只产生事件，输出只消费意图**。UI / 灯 / 电机禁止互相直调。会话模块以后只 `pet_core_post(PET_EVT_LISTEN)`。

---

## 2. 模块与依赖

| 模块 | 路径 | 允许依赖 |
|------|------|----------|
| 大脑 | [`desktop_pet/pet_core/`](../desktop_pet/pet_core/) | C 标准库 |
| 文件系统 | [`desktop_pet/pet_fs.h`](../desktop_pet/pet_fs.h) | `stdio` |
| 资源包 | [`desktop_pet/pet_res/`](../desktop_pet/pet_res/) | `pet_fs`、`pet_core` |
| 表演 | [`desktop_pet/pet_view/`](../desktop_pet/pet_view/) | LVGL 9、`pet_core`、`pet_res` |
| 板端口 | [`desktop_pet/main/source/desktop_pet_ui.c`](../desktop_pet/main/source/desktop_pet_ui.c) | 板级 + 上列模块 |
| 模拟器端口 | [`tools/pet_sim/`](../tools/pet_sim/) | 上列模块 + Windows LVGL 模拟器 |
| 打包工具 | [`tools/pet_pack/pack_build.py`](../tools/pet_pack/pack_build.py) | Python 3 |

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
| `PET_EVT_CARE_FEED` / `PLAY` / `SLEEP` / `WAKE` | 照料 | HUD 按钮 / 键盘 / 手势 |
| `PET_EVT_LISTEN` / `SPEAK` / `EMOTION` | 预留小智 | 本阶段不接线；`arg0` 可为 face id |

`pet_core_post()` 非线程安全：只在 LVGL/`pet_view` 任务投递。GPIO0 只置标志，由 UI 定时器切 debug 层。

---

## 4. 意图（输出）

| id | `arg0` | 消费者 |
|----|--------|--------|
| `PET_INTENT_CLIP` | `pet_clip_id_t` | `pet_view` 换身体帧 |
| `PET_INTENT_FACE` | `pet_face_id_t` | `pet_view` 五官 |
| `PET_INTENT_HUD` | —（读 `pet_core_get_needs`） | 顶栏三格 |
| `PET_INTENT_LED` | 0=eat 1=play | `led_scene` |
| `PET_INTENT_MOTOR` | 预留 | 未接 TB6612 |
| `PET_INTENT_SFX` | 预留 | 未接 |

---

## 5. Needs 与 clip

三格 0–100：`hunger` / `mood` / `energy`。无死亡、无进化；掉到 0 只切 `sad` / `sleepy`。

默认衰减（无 pack 时，桌面友好、小时级）：饥饿 180 s/点、心情 240 s/点、精力 300 s/点；睡觉时精力 20 s/点恢复。模拟器默认 pack 用数秒级，便于看见 HUD 变化。

| clip | 优先级 | 时长 | 触发 |
|------|--------|------|------|
| `idle` / `sleepy` / `sad` / `sleep_loop` | 0 | 循环 | needs / 睡眠 |
| `poke` | 10 | 1 s | 点身体 |
| `eat` / `play` | 20 | 3 s / 4 s | 喂 / 玩；播完前回不去低优先级 |

眨眼由 `pet_view` 本地定时，不占 clip。

五官：`idle` / `happy` / `sad` / `sleepy` / `hungry` / `angry`。

---

## 6. `pack.bin` 布局（小端）

设备**不解析 JSON**。作者侧 [`pack.json`](../tools/pet_pack/pack.json) → `pack_build.py` → `pack.bin` + `body/*.bin`。

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

`clip_id` 与 `pet_clip_id_t` 一致：0 idle，1 sleepy，2 eat，3 play，4 sad，5 sleep_loop，6 poke。

### 6.3 身体帧 `.bin`（RGBH）

与图库同类：12 字节头 + RGB565 行优先。

| 偏移 | 内容 |
|------|------|
| 0 | `RGBH` |
| 4 | u16 width |
| 6 | u16 height |
| 8 | u32 flags（0 = 行优先） |
| 12 | `width * height * 2` 字节 RGB565 |

建议边长 ≤ 180。PSRAM 双缓冲最多 2 帧。无 alpha；画面外像素填屏背景色 `0x202020`。

卡上目录：

```
/sdcard/pet/pack.bin
/sdcard/pet/body/idle_0.bin
...
```

模拟器镜像：[`tools/pet_sim/sdcard/pet/`](../tools/pet_sim/sdcard/pet/)。

---

## 7. 圆屏分层

```
needs 三细条（LVGL HUD）
        SD 身体 lv_image（或色块 fallback）
        LVGL 眼 + 瞳孔（可眨）
        LVGL 嘴（随 FACE）
照料按钮 Feed / Play / Sleep
一句状态（hungry / zzz / nom …）
GPIO0：debug Rec/Play/IMU/IP 覆盖层
```

字幕层给以后 STT；首版只显示 needs 文案。

---

## 8. 与语音计划的边界

- 本框架不实现 WebSocket / Opus / ESP-SR。
- M4：agent 投 `PET_EVT_LISTEN` / `SPEAK` / `EMOTION`，由 `pet_core` 改 FACE/clip，**禁止**在 agent 里画表情。
- GPIO0 单击：宠物主界面 ↔ debug Rec/Play（对齐 ASR 文档 D14）。
- `/sdcard/record/` 录音落盘与 Web 管卡保持独立，不进入 `pet_core`。

---

## 9. 模拟器

1. `powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1`
2. `powershell -ExecutionPolicy Bypass -File .\tools\pet_sim\install_into_lvgl_sim.ps1`
3. 用 Visual Studio 打开 `tools\lvgl_sim\lv_port_pc_visual_studio\LVGL.sln`，启动 `LvglWindowsSimulator`（240×240）。
4. 键盘 `1` 喂、`2` 玩、`3` 睡、`4` 醒。工作目录为仓库根，才能读到 `tools/pet_sim/sdcard/pet`。

重新生成资源：`py -3 tools/pet_pack/pack_build.py`

---

## 10. 板端操作

| 输入 | 效果 |
|------|------|
| Feed / 长按身体 | 喂 |
| Play / 晃一下 | 玩 |
| Sleep / 翻转约 1 s | 睡 |
| 点身体 | poke（睡觉则先醒） |
| GPIO0 单击 | 开关 debug 覆盖层 |
