# desktop_pet 对齐「小智」语音助手开发计划

**版本**：2.2  
**日期**：2026-08-19  
**工程**：[`desktop_pet/`](../../desktop_pet/)  
**状态**：方案对齐小智；**§11 Q1–Q6 已拍板**；实施中（见 §13）  
**参考**：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)、[乐鑫小智组件说明](https://docs.espressif.com/projects/esp-iot-solution/zh_CN/latest/ai/xiaozhi.html)、现有 [`audio`](../../desktop_pet/main/source/audio.c)  
**文档集**：[`README.md`](README.md) · [`framework.md`](framework.md) · [`product.md`](product.md) · [`CONTEXT.md`](CONTEXT.md)

---

## 1. 目标与边界

### 1.1 产品目标

桌宠作为**语音交互入口**，对齐小智主路径：

1. **本地**唤醒（可选按键触发会话）→ 采音 → Opus 编码 → 上行流  
2. **云端**流式 ASR → LLM → TTS  
3. **本地**解码 Opus → ES8311 播放；屏幕显示状态/字幕/表情  
4. （后续）经 MCP / 业务层驱动舵机、灯效、表情等

**ES8311 只负责采音/放音**；**不新增 ASR 专用芯片**；重识别与对话在云端（或自建小智兼容服务端）。

### 1.2 与 v1.0 的关系

| | v1.0（已废弃为主路径） | v2.0（本版） |
|--|------------------------|--------------|
| 形态 | 录完再 POST WAV | **边说边上 Opus 流** |
| 云端 | 仅 ASR | **ASR + LLM + TTS** |
| 触发 | 手动 ASR 键 | **ESP-SR 唤醒 + 按键兜底** |
| 协议 | HTTPS REST | **WebSocket（优先）或 MQTT+UDP** |
| 现有 Rec/Play | 产品功能 | **保留为音频通路验收与调试工具** |

现有录音/播放验证的价值：证明 **MIC→ES8311→I2S→PSRAM→I2S→PA** 通，是对齐小智的**硬件前提**，不是最终产品交互形态。

### 1.3 非目标（首版可砍）

| 排除项 | 说明 |
|--------|------|
| 外挂 ASR 芯片 | 不需要 |
| 板端跑完整 ASR/LLM | ESP32-S3 不做；与小智一致 |
| 全双工实时打断（AEC） | 列为后期；首版半双工「说完再听播报」即可 |
| 声纹识别（3D Speaker） | 后期 |
| 必须对接官方 `xiaozhi.me` | 可用社区自建 server；协议对齐即可 |
| 4G Cat.1 | 本板仅 WiFi |

### 1.4 成功标准（分档）

| 档位 | 标准 |
|------|------|
| **M1 音频对齐** | 已具备：录/放通、STA 可出网 |
| **M2 流式会话** | 按键开会话 → Opus 上行 → 服务端回 Opus TTS → 喇叭可听、屏有简短状态 |
| **M3 唤醒对齐** | 主界面「嗨乐鑫」本地唤醒 → 开对话页并自动听（γ）；模型在 SD，缺则无唤醒仍可启动 |
| **M4 桌宠业务** | 对话结果驱动表情/舵机/灯（MCP 或本地意图钩子） |

---

## 2. 已确认决策

| # | 议题 | 决策 | 备注 |
|---|------|------|------|
| D1 | 总体架构 | **对齐小智：本地唤醒 + 流式 Opus + 云端 ASR/LLM/TTS** | 不以一句话 HTTP ASR 为主路径 |
| D2 | 额外芯片 | **不需要** | ESP32-S3 N16R8 + ES8311 足够 |
| D3 | 设备↔云协议 | **WebSocket 优先**（与小智 `docs/websocket.md` 同思路） | MQTT+UDP 为备选 |
| D4 | 音频编解码 | **Opus** 上下行 | PCM 仅板内；调试可保留 WAV 落盘 |
| D5 | 采样率策略 | **板内统一后与服务器 hello 协商** | 现网 16 kHz；若服务端要 16/24 kHz 则重采样或改 Codec 时钟 |
| D6 | 服务端 | **小智兼容服务端（自建）优先** | 避免设备直连多家 ASR/TTS SDK；密钥集中在服务端 |
| D7 | 唤醒 | **ESP-SR WakeNet**；仅主界面；「聊」/按键兜底 | 对话页内停检测；SPEAKING 不可唤醒打断 |
| D8 | 现有 Rec/Play UI | **保留为调试页/厂测**；产品页另做「对话态」 | 不删已验证通路 |
| D9 | 密钥 | 设备侧仅会话 Token / 设备 ID；**大模型与 ASR Key 不放固件** | |
| D10 | 服务端选型 | **[xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)** 最简安装 | 局域网自建；优先 Docker，无 Docker 则 conda+源码 |
| D11 | 唤醒词 | 默认 **嗨乐鑫**（`wn9s_hilexin`）；模型**整包在 SD** `sr_models/`；`/sdcard/config` 开机选模型 | 缺模型 → 正常启动、不做唤醒；真定制=换 SD 模型 |
| D12 | 双工策略 | **首版半双工** | LISTENING 时少播；SPEAKING 时 mute 上行 |
| D13 | 采样率 | **hello 协商**；板内优先软件重采样 | 现网 Codec 16 kHz |
| D14 | 对话/调试切换 | **GPIO0（BTN_ID_CONFIRM）** | 单击切换对话态↔调试 Rec/Play；与现有 `start.c` 按键钩子对齐 |
| D15 | 唤醒后回复 | **B+C：叮 + 本地短句随机**；资源 SD `sfx/wake/`；缺则静默仍开听；播完再 `listen_start` | 非云端 TTS；不属于皮肤包 |

---

## 3. 当前能否实现——可行性评估

### 3.1 结论（先看这）

| 维度 | 结论 |
|------|------|
| **硬件** | **能**。与主流小智 ESP32-S3 + ES8311 板同级，无硬性缺口。 |
| **软件（现状）** | **不能开箱即用**。已验证的是「本地录放」，缺 Opus、WebSocket 流、ESP-SR、会话状态机、兼容服务端。 |
| **软件（投入后）** | **能**。属工程移植/自研对齐，不是换板或加芯片问题。 |
| **风险点** | Flash/RAM 余量（LVGL+WiFi+SR+Opus+WS 同开）、采样率协商、半双工策略、服务端选型与运维。 |

### 3.2 硬件对照

| 能力 | 小智典型板 | 本桌宠（现状） | 判定 |
|------|------------|----------------|------|
| MCU | ESP32-S3（常配 PSRAM） | **ESP32-S3 N16R8** | ✅ 充足 |
| Codec | ES8311 / ES8374 等 | **ES8311 全双工 I2S + MCLK** | ✅ |
| 喇叭功放 | PA_EN | **SPE_EN GPIO14**（播放已用） | ✅ |
| MIC | 板载/模拟麦 | 已能录到有效 PCM | ✅ |
| 网络 | WiFi / 部分 4G | **WiFi SoftAP+STA + web_ctrl** | ✅ WiFi 够用 |
| 显示 | OLED/LCD | **GC9A01 240 圆屏 + IT7259** | ✅ |
| 执行器 | GPIO/舵机等 | **TB6612 电机 + WS2812** | ✅ 利于 M4 |
| 存储 | Flash±SD | **16MB Flash + SD** | ✅ |

**硬件结论：不需要为对齐小智再买 ASR 芯片；现板可做。**

### 3.3 软件对照（差距清单）

| 小智能力 | 本工程现状 | 差距 |
|----------|------------|------|
| I2S 录/放 | ✅ 已通（16 kHz mono，PSRAM 缓冲，Play/Pause） | 需改为**流式环形缓冲**，而非仅「录满 8s」 |
| Opus 编解码 | ❌ | 需接入（如 `esp-opus` / 小智同款组件） |
| WebSocket 双向音视频会话 | ❌（仅有设备作 HTTP **服务器** 的 web_ctrl） | 需 **WS 客户端** + hello/会话协议 |
| 云端 ASR+LLM+TTS | ❌ | 用**兼容小智协议的服务端**承接，设备不直连各厂 ASR |
| ESP-SR 唤醒 / AFE | ❌ | 模型改放 **SD**（减 Flash）；需测与 LVGL 共存；无模型则跳过唤醒 |
| 会话状态机（听/说/闲） | ✅ 骨架（agent + 对话页） | M3：主界面唤醒 → 开页并自动听 |
| MCP 控设备 | ❌ | M4 再做；可先本地钩子转表情/电机 |
| 调试 WAV 落盘 | ✅ | 保留 |

### 3.4 资源粗估（需实机测，以下为经验风险）

| 项 | 说明 |
|----|------|
| PSRAM 8MB | 足够同时撑 LVGL 缓冲 + 音频环形缓冲 + WS；✅ |
| 内部 SRAM | WiFi + TLS + Opus + SR 模型峰值要盯；可能需关部分调试功能或把缓冲外置 PSRAM |
| Flash app 分区 | 唤醒模型在 SD，Flash 压力低于「模型打进固件」；仍需 `size` 验收 Opus+LVGL+esp-sr 运行时 |
| CPU | S3@240MHz 跑半双工流式对话常见可行；全双工 AEC 更吃紧，故首版半双工 |

### 3.4.1 内存分相（强制约定，避免「全都开」）

ESP32-S3 **不能**把会碰 Flash 的任务栈放到 PSRAM。内部 SRAM 撑不住 **WakeNet + httpd(~8–10KB 栈) + agent TLS** 同时峰值：

- 保 WakeNet + 保 httpd 进对话 → TLS `esp-aes` OOM  
- 毁 WakeNet 再 `create` → 离开后 `largest` 过小，且 httpd `listen(112)` 难恢复  

| 相 | 允许常驻 | 必须释放 / 禁止 |
|----|----------|-----------------|
| **HOME** | WakeNet + 检测；httpd | 不开 agent 上下行 |
| **CHAT** | agent WS / Opus / capture；**WakeNet 实例保留**；**STA 保留** | `web_ctrl_http_suspend`；停检测（`wake_enter_chat_mode`） |
| **离开对话** | 回 HOME | 再武装检测；`web_ctrl_http_resume`（**不** destroy/recreate 模型） |

原则：

1. **大块数据 → PSRAM**  
2. **碰 Flash 的任务栈 → 内部 RAM**  
3. 对话期停网页换 TLS 余量；模型常驻避免 SD 再加载  
4. HTTP suspend/失败 → **保留 STA**  
5. 仅关 SD 管理 API **不够**（httpd 栈仍在）  
6. `listen(112)`：socket 池、ctrl_port 轮换、stop 后延时；唤醒不依赖网页立即恢复  

日志：进对话 `PAUSE (keep model, http down)`；离开 `HTTP resumed`。

### 3.5 可行性一句话

> **板子够格对齐小智；当前固件只走完了「Codec 通路验收」。**  
> 要产品级对齐，工作量在：**流式音频管线 + 协议 + 服务端 + 唤醒**，而不是改原理图加芯片。

---

## 4. 目标架构（对齐小智）

```
                    ┌─────────────────────────────────────┐
                    │     小智兼容服务端（自建或托管）        │
                    │   流式 ASR → LLM → TTS（+ 可选 MCP）   │
                    └──────────────▲────────────────────────┘
                                   │ WebSocket
                                   │ JSON 控制 + 二进制 Opus
┌──────────────────────────────────┴────────────────────────┐
│ desktop_pet                                                │
│  WakeNet(ESP-SR，SD 模型) / 主界面；「聊」兜底              │
│       │                                                    │
│  MIC→ES8311→I2S RX→[AFE可选]→PCM→Opus Enc→WS 上行          │
│  SPK←ES8311←I2S TX←PCM←Opus Dec←WS 下行                     │
│       │                                                    │
│  UI：对话态 / 调试 Rec·Play（现有） / 表情                  │
│  业务：电机 / LED / （后期 MCP）                            │
└────────────────────────────────────────────────────────────┘
```

### 4.1 建议模块划分

| 模块 | 职责 |
|------|------|
| `desktop_pet_audio` | Codec/I2S、PA、PCM 环形缓冲；保留 debug 录满/WAV/本地 Play |
| `desktop_pet_opus` | PCM↔Opus |
| `desktop_pet_agent` 或 `desktop_pet_xiaozhi` | WS 会话、hello、状态机、上下行调度 |
| `desktop_pet_wake` | ESP-SR（SD 模型）；仅主界面；命中 → 开对话页 + 自动听；缺模型则 no-op |
| `desktop_pet_ui` | 对话 UI + 保留调试页 |
| 服务端 | 社区 xiaozhi-esp32-server 一类，或自研兼容协议 |

依赖：`desktop_pet/main` → common / IDF / esp-sr / opus；**不**把协议塞进 `bsp_driver`。

### 4.2 设备侧状态机（半双工首版）

```
主界面 IDLE
  ├─「聊」──────► CONNECTING ──hello OK──► OPEN（等点身体再听）
  └─唤醒嗨乐鑫──► CONNECTING ──hello OK──► [叮+短句] ──► LISTENING
LISTENING ──listen stop──►（等 STT/LLM）──► SPEAKING
SPEAKING ──播完/点打断──► LISTENING 或 OPEN
对话页内不跑唤醒；离开对话页 → 恢复主界面唤醒
任意态 ──断网/错误──► IDLE + UI 提示
```

- LISTENING：上行 Opus，本地尽量少播。  
- SPEAKING：下行 Opus 播放，上行可 mute（半双工）。  
- 与现有 `record`/`play` 调试互斥：进对话态时停 debug 录放。

---

## 5. 分阶段工作项（按小智路径）

### 阶段 0 — 基线（✅ 基本完成）

| ID | 任务 | 状态 |
|----|------|------|
| Z0-1 | ES8311 录音有效 | ✅ |
| Z0-2 | ES8311 播放 + PA | ✅ |
| Z0-3 | STA / SoftAP 配网 | ✅（web_ctrl） |
| Z0-4 | 文档明确：录放 = 通路验证，非最终 UX | 本版 |

### 阶段 1 — 流式最小闭环（M2，优先）

| ID | 任务 | 验收 |
|----|------|------|
| Z1-1 | 选定并部署**小智兼容服务端**（局域网可） | 选型 ✅；部署指引见 §6.1；本机起服务待执行 |
| Z1-2 | 设备 WebSocket 客户端 + hello / 鉴权头 | 握手成功有 session |
| Z1-3 | 录音改为**实时读 I2S → Opus → WS 二进制帧** | 服务端能收到音并出 STT 日志 |
| Z1-4 | WS 收 Opus → 解码 → I2S 播放 | 喇叭听到 TTS |
| Z1-5 | UI：连接中 / 聆听 / 说话 / 失败 | 圆屏可辨 |
| Z1-6 | 按键或调试钮「开始/结束会话」 | 无唤醒也能测 |

### 阶段 2 — 唤醒对齐（M3）

| ID | 任务 | 验收 |
|----|------|------|
| Z2-0 | ~~拍板唤醒后回复词（D15 / Q6）~~ | ✅ B+C：SD `sfx/wake/` 叮 + 随机短句 |
| Z2-1 | 集成 ESP-SR WakeNet；模型从 SD `sr_models/` 加载 | 「嗨乐鑫」可触发；无卡/无模型仍正常开机 |
| Z2-2 | 主界面唤醒 → 开对话页 → 播 wake reply → `listen_start`（γ）；进页停唤醒 | 与「聊」入口行为可区分；无 reply 文件则静默开听 |
| Z2-3 | 测 RAM；与 LVGL/agent 音频互斥 | 实机稳定 30min；无模型路径无崩溃 |

### 阶段 3 — 体验与桌宠业务（M4）

| ID | 任务 | 验收 |
|----|------|------|
| Z3-1 | STT 文本上屏（短句） | 与小智「看见自己说的话」类似 |
| Z3-2 | LLM 意图 → 表情 / 舵机 / LED | 至少 2～3 个动作 |
| Z3-3 | （可选）设备端 MCP | 与小智生态工具对齐 |
| Z3-4 | 保留 Rec/Play 为厂测隐藏页或串口开关 | 量产不误触 |

### 阶段 4 — 增强（不阻塞上市试点）

- AFE 降噪 / VAD 更稳  
- 全双工 + AEC（硬件与算法条件具备时）  
- 多语言、声纹  
- 官方云与自建云配置切换  

---

## 6. 服务端策略（推荐）

| 方案 | 优点 | 注意 |
|------|------|------|
| **社区小智 server**（Python/Java/Go） | 协议现成，ASR/LLM/TTS 可配 | 跟版本、安全加固 |
| 官方 xiaozhi.me | 少运维 | 账号/配额/定制受限 |
| 完全自研协议 | 可控 | 工作量大，不推荐首版 |

**已选定（D10）**：局域网自建 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) **最简化安装**（先打通 M2，再谈全模块智控台与公网）。

设备固件**不要**直接堆讯飞+通义+TTS 三套 SDK；与小智一样，**复杂 AI 留在服务端**。

### 6.1 Z1-1 部署清单（本机）

> 当前联调：**Docker Desktop**，目录 `D:\Docker\xiaozhi-server`。简介与启停命令见 [`README.md`](README.md)「小智服务端（Docker）」。

**目标地址形态（设备侧后续写入配置）**：

| 用途 | 示例（把 IP 换成 PC 局域网地址） |
|------|----------------------------------|
| WebSocket | `ws://192.168.x.x:8000/xiaozhi/v1/` |
| OTA/引导（可选） | `http://192.168.x.x:8003/xiaozhi/ota/` |

**路径 A — Docker（本机已用）**

```powershell
cd D:\Docker\xiaozhi-server
docker compose up -d
docker logs -f xiaozhi-esp32-server
```

1. 目录含 `docker-compose.yml`、`data/.config.yaml`、`models/SenseVoiceSmall/model.pt`。
2. 在 `.config.yaml` 配置 LLM Key（**只放服务端**，勿入库）。
3. 日志确认 WS；**PC/手机浏览器勿当 WS 测**。
4. 防火墙放行 **8000**（及需要的 8003）。

**路径 B — conda 源码最简（无 Docker 时）**

1. 安装 Anaconda/Miniconda → `conda create -n xiaozhi-esp32-server python=3.10`。
2. `conda install libopus ffmpeg`；clone 仓库，进入 `main/xiaozhi-server`，`pip install -r requirements.txt`。
3. 同路径 A：模型文件 + `.config.yaml`（含 LLM Key）+ `python app.py`。
4. 确认日志中的 `Websocket地址是 ws://<LAN-IP>:8000/xiaozhi/v1/`。

**Z1-1 验收**：同网 PC 侧能启动服务且日志出现 WebSocket 地址；LLM/ASR/TTS 模块无启动即崩（可先云端 ASR/TTS API，SenseVoice 本地亦可）。

**本切片状态**：容器 Up；设备 WS 见下方 §13。
---

## 7. 安全与合规

| 项 | 要求 |
|----|------|
| 设备 | Device-Id、会话 Token；禁止提交云厂商永久 Key |
| 传输 | WSS（TLS）；开发可用局域网 WS，量产必须加密 |
| 隐私 | 产品说明「语音上传云端处理」 |
| 日志 | 不打印完整 Token；限制 PCM dump |

---

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| SRAM/Flash 不够同时开 SR+LVGL+WS | 分阶段打开；模型选小；缓冲进 PSRAM；`idf.py size` 门禁 |
| 16 kHz 与服务端参数不一致 | hello 协商；板内重采样或改 `DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ` |
| 只做本地录放误以为「已对齐小智」 | 本文件 M1/M2 分档；验收以流式会话为准 |
| 协议跟小智主线漂移 | 钉死参考 commit / 协议文档版本；抽象 `agent` 层 |
| 半双工体验差 | 先可用；再评估 AEC 板级与算法 |

---

## 9. 验收清单

### M2（流式会话）

- [ ] STA 下设备能连接自建/兼容服务端并完成 hello  
- [ ] 按键开听 → 说话 → 服务端 STT 有文本（日志或屏）  
- [ ] 返回 TTS Opus 可播放，PA 正常开关  
- [ ] 断网有 UI 提示且能回到 IDLE 再试  
- [ ] 现有 debug Rec/Play 在非对话态仍可用  

### M3（唤醒）

- [ ] SD 有 `sr_models/wn9s_hilexin` 时，主界面「嗨乐鑫」可开对话并自动听  
- [ ] SD 无模型 / `wake_enable=0` 时正常启动，无唤醒、无崩溃  
- [ ] 对话页内不触发唤醒；「聊」入口仍只连不自动听  
- [ ] 唤醒后：有 `ding.wav` 则先叮，再随机短句；无文件则静默；然后自动听；播报期间不上行  
- [ ] 误唤醒率可接受（室内静音抽测）；长时间运行无泄漏（≥30 min）  

### 工程

- [ ] `idf -Project desktop_pet build` 零新增告警  
- [ ] 仓库无云端永久密钥  

---

## 10. 实施顺序建议

1. **冻结目标**：对齐小智协议，不走「仅 HTTP ASR」主路径（本版 D1）。  
2. **起服务端**：本机 Docker/Python 小智 server，PC 侧先验证账号与模型。  
3. **板上 M2**：WS + Opus 上下行 + 按键会话（复用现有 ES8311）。  
4. **再上 ESP-SR（M3）**。  
5. **桌宠动作（M4）**。  
6. Rec/Play 降为调试能力，对话态为默认 UX。

---

## 11. 开放问题（已拍板 → §2）

| # | 问题 | 决定 |
|---|------|------|
| Q1 | 服务端 | **社区兼容 server** → D10 `xinnan-tech/xiaozhi-esp32-server` |
| Q2 | 唤醒词 | **嗨乐鑫** + SD 模型 → D11（修订） |
| Q3 | 半双工 / 全双工 | **半双工** → D12 |
| Q4 | 采样率 | **hello 协商 + 优先软件重采样** → D13 |
| Q5 | 对话 UI ↔ debug Rec/Play | **GPIO0 按键** → D14 |
| Q6 | 唤醒后回复 | **B+C：叮 + SD 短句随机** → D15 |

D15 细节（与 [`product.md`](product.md) §A.3 一致）：`/sdcard/sfx/wake/`；可选 `ding.wav` 先播；其余 wav 随机一句；缺资源静默；播完再开听。建议口播：在呢 / 我在 / 嗯？ / 来了 / 怎么啦。

---

## 12. 相关入口

| 路径 | 说明 |
|------|------|
| [`desktop_pet/main/source/audio.c`](../../desktop_pet/main/source/audio.c) | ES8311 录放（M1） |
| [`desktop_pet/main/source/ui.c`](../../desktop_pet/main/source/ui.c) | LVGL 端口 + debug 覆盖层 |
| [`framework.md`](framework.md) | 桌宠大脑 / SD 资源包 / 分层 UI |
| [`product.md`](product.md) | 皮肤 / 主界面 / 对话页约定 |
| [`desktop_pet/main/start.c`](../../desktop_pet/main/start.c) | 启动 / web_ctrl / GPIO0 按键钩子 |
| [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | 设备侧参考 |
| [xiaozhi websocket 协议](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md) | 会话协议参考 |
| [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) | 自建服务端（Z1-1） |
| [`embedded_coding_standard.md`](../embedded_coding_standard.md) | 分层约定 |

---

## 13. 实施进度（分对话切片）

> 约定：**一次对话只推进一个可验收切片**；下一切片开工前先读本表。

| 切片 | 对应 | 状态 | 说明 |
|------|------|------|------|
| S0 | 决策冻结 Q1–Q5 → D10–D14 | ✅ | 本轮完成 |
| S1 | Z1-1 选定服务端 + 部署 | ✅ | 容器 Up；设备用 `ws://192.168.10.198:8000/xiaozhi/v1/`（忽略日志里 172.18） |
| S2 | Z1-2 设备 WS 客户端 + hello/鉴权 | ✅ | 实机 `hello ok` / `session OPEN` |
| S3 | Z1-3 Opus 上行流 | ✅ 代码 | listen + I2S→Opus→WS；验收看 `uplink frames` / 服务端 STT |
| S4 | Z1-4 Opus 下行播放 | ✅ 本对话 | tts start→decode→playout；看 `downlink frames` / 喇叭 |
| S5 | Z1-5 / Z1-6 对话 UI + GPIO0 开会话 | 待办 | 与 D14 对齐 |
| S6+ | M3 唤醒 / M4 业务 | **内存分相**：对话停 httpd（保模型）；离开再开网页 | 见 §3.4.1 |
| S7 | 官方云 OTA 登记 + WSS | ✅ 本对话 | 进会话 POST `/xiaozhi/ota/`；屏显 `code XXXXXX`；忽略 firmware.url |

**填写（起服务后）**：

- 当前默认后端：**官方云** `wss://api.tenclass.net/xiaozhi/v1/`（OTA `https://api.tenclass.net/xiaozhi/ota/`）
- 局域网回退：OTA URL 留空；WS `ws://<PC-LAN-IP>:8000/xiaozhi/v1/`
- 服务端 LAN IP：`192.168.1.13`（电脑 WLAN，与板子同网）
- 部署方式：`[x] Docker`（`D:\Docker\xiaozhi-server`） / `[ ] conda 源码`
- LLM 模块（勿写入仓库密钥）：`ChatGLMLLM`（已配置，密钥仅在 `D:\Docker\...\data\.config.yaml`）
- 状态：官方云需公网 STA；绑设备用 OTA 返回的 6 位码（xiaozhi.me 添加设备）
