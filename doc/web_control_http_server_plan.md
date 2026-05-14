# ESP32-S3 轻量 Web 服务与网页指令控制开发计划

**版本**：1.2  
**依据**：ESP-IDF `esp_http_server` / `esp_wifi` 官方组件；本仓库 `doc/application_architecture.md` 分层与任务约定；`common` 中 `cmd_*` 串口命令框架（`cmd.h` / `cmd.c`）。  
**目标**：在 MCU 上运行**轻量 HTTP 服务**，通过局域网内网页下发指令，驱动已有机载能力（灯效、显示、槽切换、传感器读数等），并给出分阶段任务与验收标准。

**代码入口（已实现部分）**：ESP-IDF 组件 **`components/web_ctrl`**；操作说明与联调记录见同目录 [`components/web_ctrl/README.md`](../components/web_ctrl/README.md)。

---

## 0. 实施进度（与仓库对齐）

| WBS 阶段 | 状态 | 说明 |
|----------|------|------|
| **阶段 1** | **已完成，实机已验证** | `net_wifi.c` / `net_wifi.h`：`esp_netif`、`esp_event`、SoftAP/STA、`esp_wifi_*`；默认 SoftAP 下浏览器访问 `http://192.168.4.1/`。 |
| **阶段 2** | **已完成，实机已验证** | `web_server.c` / `web_server.h`：`esp_http_server`，`GET /` 极简页、`GET /api/health` → `{"ok":true}`。 |
| **编排与自动启动** | **已完成** | `web_ctrl.c` / `web_ctrl.h`：`web_ctrl_start`/`stop`；`project/main/start.c` 在 **`BoardInit` 成功之后** 可选调用（由 Kconfig 控制）。 |
| **阶段 3** | **已完成（代码已合入）** | `web_ctrl_cmd.c` / `web_ctrl_cmd.h`：单槽队列 + 专用任务 `web_ctrl_cmd` 执行；`POST /api/cmd`（JSON `{"line":"…"}`）同步等待应答（超时 504、并发 503）；`common/cmd.c` 增加 **`cmd_process_line_for_web`** + **`cmd_embed_register_defaults_if_needed`**，与 USB 读行通过递归互斥串行化，应答写入 HTTP 缓冲而非串口。 |
| **阶段 4** | **部分完成** | `GET /` 内嵌极简 **`fetch('/api/cmd')`** 表单，便于浏览器联调；与灯效/LCD/SD 的专项压测与 UI 完善仍可按 WBS 继续。 |
| **阶段 5** | 未开始 | `CONFIG_ESP_HTTP_SERVER_*` 调优、厂测文档链接、鉴权/限流等硬化。 |

**menuconfig 中开启自动启动（本工程路径）**：主菜单进入 **`Component config`** → **`Web Control Library (web_ctrl)`** → 将 **`Auto-start SoftAP + HTTP server after board init`**（`WEB_CTRL_AUTO_START`）设为启用；同菜单可配置 **`POST /api/cmd` 同步最大等待时间**（`WEB_CTRL_CMD_SYNC_TIMEOUT_MS`，默认 3000 ms）。保存 `sdkconfig` 后重新编译、烧录。详细步骤见 [`components/web_ctrl/README.md`](../components/web_ctrl/README.md) §3～§4。

---

## 1. 可行性结论（资深嵌入式视角）

### 1.1 总体结论：**可行，且为 ESP32-S3 常见用法**

| 维度 | 评估 |
|------|------|
| **硬件** | ESP32-S3 集成 Wi‑Fi（及可选 BLE），算力与 RAM 足以承载 **单客户端或小并发** 的 HTTP 服务；与 LCD / SD / I2C 等外设无根本冲突，需注意 **总线互斥** 与 **任务栈**。 |
| **软件栈** | ESP-IDF 自带 **`esp_http_server`**（基于 `http_parser`）、**`esp_wifi`**（STA / SoftAP）、**`lwip`**，成熟度高，无需自研 TCP 栈。 |
| **实时性** | HTTP 处理在独立任务中执行即可；**禁止在 HTTP 回调里长时间占用** LCD SPI / SDMMC；与现有架构一致：**短回调 + 队列投递 + 业务任务执行**（见 `application_architecture.md` §4～§5）。 |
| **资源** | 典型 `esp_http_server` + Wi‑Fi STA 场景下，需为 **Wi‑Fi 任务、TCP/IP、HTTP 服务任务** 预留足够 **PSRAM / 内部 RAM**（若启用 PSRAM，可将部分静态页或大 JSON 放 PSRAM）；具体以 `idf.py size` 与运行时 **栈高水位** 为准。 |

### 1.2 主要风险与对策

| 风险 | 对策 |
|------|------|
| **多任务争用外设**（LCD、SD） | 网页触发的动作统一经 **队列** 投递到现有 `app_mod` 或专用「控制执行」任务；HTTP handler 只做解析与入队。 |
| **看门狗 / 阻塞** | 不在 Wi‑Fi 回调里做重活；文件读写、刷屏分段并 `vTaskDelay` 或降优先级。 |
| **安全** | 首版可限定 **SoftAP + 预共享密钥** 或 **仅局域网 STA**；生产若暴露 STA 侧网络，需补充 **HTTPS**、**认证（Token / 摘要）**、**速率限制**（见 §5）。 |
| **配网体验** | 若走 STA，需 **SmartConfig / BLE Provisioning / WPS / 串口预写 NVS** 之一写入 SSID/密码；否则现场无法联网。 |

### 1.3 非目标（首版可明确排除）

- 公网穿透、云端 MQTT 桥接（可作为后续迭代）。
- 高并发 WebSocket 推送（首版以 **请求-应答 REST 或简单 SSE** 为主即可）。
- 完整 OTA 通过浏览器上传大包（可与现有分区方案另文衔接，首版可不实现）。

---

## 2. 方案概要

### 2.1 网络形态（二选一或组合）

1. **SoftAP（设备开热点）**  
   - 优点：无路由器亦可连接；适合演示与封闭现场。  
   - 缺点：手机/笔记本需切换 Wi‑Fi；注意 DHCP 与信道。

2. **STA（连路由器）**  
   - 优点：与现有局域网融合，浏览器直接访问设备 IP。  
   - 缺点：依赖配网流程与 DHCP。

**推荐**：开发期 **SoftAP + STA 可配置**；量产默认策略由产品定（NVS 配置项）。

### 2.2 应用协议

| 方案 | 说明 |
|------|------|
| **REST + JSON（推荐首版）** | `POST /api/v1/cmd` body `{"cmd":"led","args":["on"]}` 或扁平 `{"action":"scene","id":3}`；易调试、与前端解耦。 |
| **复用文本命令** | 与 `cmd_process_line` 相同语法，由 HTTP 层将 body 转一行字符串；**复用注册表**，减少两套逻辑。需注意 **应答目的地** 从串口改为 HTTP 响应缓冲。 |

**推荐**：内部仍走 **`cmd_register` 风格** 或 **统一「动作 ID + 参数」表**；HTTP 仅做 **鉴权、解析、排队、JSON 封装**。

### 2.3 与现有工程分层对齐

- **入口**：在 `app_start` 成功、`BoardInit` 之后调用 **`web_ctrl_start()`**（或经 **`WEB_CTRL_AUTO_START`** 由 `project/main/start.c` 自动调用）；避免在按键 ISR 路径直接起服务。
- **任务**：HTTP 默认由 `esp_http_server` 内部任务处理；业务侧由 **`web_ctrl_cmd`** 任务消费单槽唤醒队列并调用 **`cmd_process_line_for_web`**（与「HTTP 回调只做解析与入队、重活在独立任务」一致；**阶段 3 已实现**）。
- **依赖**：由 **`components/web_ctrl/CMakeLists.txt`** 声明 `esp_http_server`、`esp_wifi`、`nvs_flash`、`esp_netif` 等；`project/CMakeLists.txt` 的 **`EXTRA_COMPONENT_DIRS`** 含 **`../components`**，`project/main` **`REQUIRES web_ctrl`**。

---

## 3. 安全与健壮性（分阶段）

| 阶段 | 内容 |
|------|------|
| **MVP** | SoftAP 密码 + 固定端口；仅 JSON API，无敏感信息回显；单 IP 简单节流（可选）。 |
| **增强** | API Token（Header）或 HTTP Basic；关闭目录遍历；`Content-Length` 上限。 |
| **生产** | HTTPS（证书策略）、更细 RBAC、审计日志写入 NVS/SD（按需）。 |

---

## 4. 开发阶段与交付物（WBS）

### 阶段 0：需求冻结与网络策略（0.5～1 天）

- 确定 **SoftAP / STA / 双模** 默认与 NVS 配置键名。
- 列出 **网页可调能力清单**（灯效、复位、槽切换、LCD 文案、IMU 读数等）与 **禁止项**（如 Flash 擦写仅维护模式）。
- **交付物**：本文档「能力表」一节补丁（可在阶段 1 末补 PR）。

### 阶段 1：Wi‑Fi 与 TCP/IP 基座（1～2 天）— **已完成**

- `esp_netif_init`、`esp_event_loop_create_default`、`nvs_flash_init`（与现有 `nvs_init` 协调，避免重复初始化）。
- 实现 **SoftAP 或 STA** 最小示例：能 `ping` 通或手机连上后访问固定 IP。
- **交付物**：`components/web_ctrl` 内 **`net_wifi.c` / `net_wifi.h`**；Kconfig **`WEB_CTRL_AUTO_START`** 与 `components/web_ctrl/README.md` §3～§4（menuconfig 路径：**`Component config` → `Web Control Library (web_ctrl)`**）。

### 阶段 2：HTTP 服务骨架（1～2 天）— **已完成**

- 启动 `httpd`，注册 `GET /` 返回极简 **健康检查页** 或静态 `index.html`（可选嵌入 `embed_txtfile`）。
- `GET /api/health` → `{"ok":true}`。
- **交付物**：`components/web_ctrl` 内 **`web_server.c` / `web_server.h`**；与 **`web_ctrl_start`** 编排联调见 `README.md`。

### 阶段 3：指令路径与业务解耦（2～4 天）— **已完成（MVP）**

- **实现要点**（与原文 WBS 对齐，命名以仓库为准）：单槽 **FreeRTOS 队列 + 二进制信号量** 等价于「请求入队、执行线程出队」；未单独定义 `web_ctrl_request_t` 结构体，槽位内为 **`line` + `reply` 缓冲** 即足够。
- HTTP **`POST /api/cmd`**：Body 为 JSON **`{"line":"<与 USB 厂测相同的单行命令>"}`**；须带 **`Content-Length`**（无头返回 **411**）；解析后入队，**同步等待**执行完成（超时 **504**、槽位被占 **503**）；应答 JSON **`{"ok":true,"reply":"<转义后的 cmd 文本应答>"}`**。
- **`web_ctrl_cmd`** 任务内调用 **`cmd_process_line_for_web`**，将 `cmd_reply_*` 输出导入 Web 缓冲；**`cmd_embed_register_defaults_if_needed`** 保证未启动 USB 读行服务时仍具备默认命令表。
- **并发**：当前为 **单飞行（single-flight）** 指令；多客户端同时 POST 时第二条返回 503，后续可扩展为多槽或异步 **202 + `/api/result`**。

### 阶段 4：前端与联调（1～3 天）— **部分完成**

- **`GET /`** 已带输入框与 **`fetch('/api/cmd')`** 按钮（极简联调页）；正式排版、多指令面板、错误提示与 **灯效 / LCD / SD** 专项压测（连续点击、与 `app_mod` 交错负载）仍可按本阶段推进。

### 阶段 5：硬化与文档（1～2 天）

- 配置项：`CONFIG_ESP_HTTP_SERVER_*`、最大 URI、body 长度。
- README 或厂测说明：**如何连热点、默认 IP、示例 curl**。
- **交付物**：`doc/compile_flash_erase_monitor.md` 或 README 增加「Web 控制」小节链接回本文。

---

## 5. 验收标准（建议）

1. 手机或 PC 在 **同一局域网或设备 SoftAP** 下，浏览器可打开页面并下发至少 **3 类** 不同指令（例如：灯效切换、只读状态查询、一次非破坏性动作），**10 次连续** 无崩溃、无泄漏（`heap_caps_check_integrity_all` 抽样）。**（已通过 `POST /api/cmd` + 厂测 `line` 复用 `led` / `version` / `log` 等命令满足「多类指令」路径；「10 次连续 / 泄漏抽样」建议在实机联调时补做并记录。）**
2. 与 **LCD/SD 忙时** 交错下发指令，无总线冲突导致的 **重启或花屏**（允许短暂排队延迟）。
3. `idf.py build` 通过；关键路径有 **LOG_INFO/ERROR** 便于现场排障。

---

## 6. 能力表（随阶段 0/3 持续补全；已填当前已交付 API）

| 网页指令 / API | 执行模块 | 是否允许首版 | 备注 |
|----------------|----------|--------------|------|
| `GET /` | `web_server.c` | 是 | 含极简 **`fetch('/api/cmd')`** 联调区；健康说明与入口 |
| `GET /api/health` | `web_server.c` | 是 | JSON `{"ok":true}` |
| `POST /api/cmd` | `web_server.c` → `web_ctrl_cmd.c` → `cmd_process_line_for_web` | 是（MVP） | JSON body **`{"line":"…"}`**；`Content-Type: application/json`；须 **`Content-Length`**；同步应答或 503/504；与 USB 命令表一致（慎用 `reboot` / `boot_*` 等破坏性命令） |

---

## 7. 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-05-14 | 1.0 | 初稿：可行性结论 + WBS + 与现有架构/cmd 对齐说明 |
| 2026-05-14 | 1.1 | 对齐实现：§0 实施进度；阶段 1/2 标为已完成并指向 `components/web_ctrl`；§2.3 与 Kconfig 路径（Component config）；能力表填入已交付 GET API；§5 注记当前验收覆盖范围 |
| 2026-05-14 | 1.2 | 阶段 3 完成：`POST /api/cmd`、`web_ctrl_cmd`、`cmd_process_line_for_web` / `cmd_embed_register_defaults_if_needed`；阶段 4 首页内嵌 fetch；§0/§4/§5/§6 与 Kconfig（`WEB_CTRL_CMD_SYNC_TIMEOUT_MS`）同步 |
