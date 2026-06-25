# web_ctrl 组件说明

本目录为 ESP-IDF 自定义组件 **`web_ctrl`**，提供 SoftAP / STA 基座、HTTP 服务、`POST /api/cmd` 与 SoftAP 网页配网（REST + `/provision`）。

---

## 当前进度（联调）

| 里程碑 | 状态 |
|--------|------|
| 阶段 1：SoftAP + `esp_netif` / 事件 / IP 日志 | **已在实机验证** |
| 阶段 2：`GET /`、**`GET /api/health`** | **已在实机验证**（浏览器可访问） |
| 阶段 3：**`POST /api/cmd`**、`web_ctrl_cmd` 任务、`cmd_process_line_for_web` | **已合入**（单槽同步；与 USB 厂测命令表复用） |
| 阶段 4：正式单页 UI、与灯效/LCD/SD 专项压测 | **部分完成**（首页仅极简表单；**`/provision` 配网页**已合入；其余见总计划） |
| SoftAP 网页配网 | **已合入**（见下文 §2.1；实机扫描/保存建议自测） |
| 阶段 5：HTTP 硬化、厂测文档链接、鉴权等 | 未实现，见总计划文档 |

---

## 1. 目录与文件职责

| 路径 | 作用 |
|------|------|
| **`CMakeLists.txt`** | 注册组件：源文件、`inc` 为对外头目录；`REQUIRES` 含 **`common`**（`cmd`）、`esp_netif`、`esp_wifi`、`esp_event`、`nvs_flash`、`esp_http_server`。 |
| **`Kconfig`** | **`WEB_CTRL_AUTO_START`**；**`WEB_CTRL_CMD_SYNC_TIMEOUT_MS`**；**`WEB_CTRL_STA_CONNECT_TIMEOUT_MS`**（策略 A：STA 等待超时后回退 SoftAP）。 |
| **`inc/web_ctrl.h`** | **对外主入口**：`web_ctrl_config_merge_nvs` 合并 **SoftAP、STA 凭据、HTTP 端口**；`web_ctrl_start` 等。 |
| **`inc/web_ctrl_wifi_api.h`** | **`web_ctrl_wifi_api_register`**：配网 REST。 |
| **`src/web_ctrl_wifi_api.c`** | 配网实现；任务 **`w_wifi_scan`** 内 **`esp_wifi_scan_start`**（阻塞）。 |
| **`inc/web_ctrl_cmd.h`** | **阶段 3**：`web_ctrl_cmd_start` / `stop`、`web_ctrl_cmd_execute_sync`；宏 **`WEB_CTRL_CMD_REPLY_MAX`**（应答缓冲上限，与槽位一致）。 |
| **`src/web_ctrl.c`** | **策略 A** 与编排：`net_wifi_start` → **`web_ctrl_cmd_start`** → **`web_server_start`** → **`web_ctrl_wifi_api_register`**；`stop` 顺序相反。 |
| **`src/web_ctrl_cmd.c`** | **阶段 3**：单槽 + `web_ctrl_cmd` 任务，在任务上下文调用 **`cmd_process_line_for_web`**；HTTP 侧通过 **`web_ctrl_cmd_execute_sync`** 阻塞等待或超时。 |
| **`inc/net_wifi.h`** | **Wi‑Fi 抽象**：`NET_WIFI_MODE_SOFTAP` / `NET_WIFI_MODE_STA`；`net_wifi_wait_sta_got_ip`、`net_wifi_get_mode`、`net_wifi_sta_has_ipv4`、`net_wifi_is_softap_mode` 等。 |
| **`src/net_wifi.c`** | `esp_netif` / 事件 / SoftAP 或 STA；STA **GOT_IP** 事件组；**`esp_wifi_connect`**。 |
| **`inc/web_server.h`** | **`web_server_start`** / **`web_server_stop`** / **`web_server_get_handle`** / **`web_server_is_running`**。 |
| **`src/web_server.c`** | `GET /`（含 **`/provision`** 链接）、`GET /api/health`、**`POST /api/cmd`**。 |

---

## 2. 当前已实现的功能摘要

| 能力 | 说明 |
|------|------|
| **SoftAP** | 默认 SSID `ESP32-WebCtrl`、密码 `esp32web1`；若 NVS 中有有效 **`web_ctrl`** 记录，上电 **`web_ctrl_config_merge_nvs`** 会覆盖默认值。USB 命令 **`webcfg`**：`show` 查看；**`set <ssid> [<password>]`**（仅改 SSID可只写一个参数；密码省略则保持当前/NVS；**`-`** 表示开放热点）；**`tune <ch> [<max> [<port>]]`** 调信道/最大连接/HTTP 端口；**`save`** 写入 NVS 并重启；**`reset`** 清除 NVS 并重启；**`discard`** 放弃暂存。日志打印 AP IP（一般为 `192.168.4.1`）。 |
| **STA（基础）** | `NET_WIFI_MODE_STA` + `sta_ssid` / `sta_password`；`GOT_IP` 日志。 |
| **HTTP** | 端口 `web_ctrl_config_t.http_port`（`0` 表示 80）；**`GET /`**、**`GET /api/health`**、**`POST /api/cmd`**。 |
| **`POST /api/cmd`** | Body：`Content-Type: application/json`，JSON **`{"line":"<与 USB 相同的厂测命令行>"}`**；必须带 **`Content-Length`** 头（否则 **411**）；成功 **200** + `{"ok":true,"reply":"…"}`（`reply` 为 JSON 转义后的串口风格应答）；**503 busy**（单槽并发）、**504 timeout**（见 Kconfig）。 |
| **`cmd` 复用** | 依赖 **`common`**：`cmd_embed_register_defaults_if_needed`、`cmd_process_line_for_web`；与 USB 读行 **`cmd_process_line`** 通过 **`cmd.c` 内递归互斥**串行，避免应答串台。 |
| **生命周期** | `web_ctrl_start` / `web_ctrl_stop`；**`nvs_flash_init`** 须在之前完成（本工程 `nvs_init` 之后即可）。 |
| **可选自动启动** | Kconfig **`WEB_CTRL_AUTO_START`**：`project/main/start.c` 在 **`BoardInit` 成功之后** 调用 `web_ctrl_start()`（内部 **`web_ctrl_config_merge_nvs`** 已包含 STA 字段）。 |

### 2.1 SoftAP 网页配网

| 能力 | 说明 |
|------|------|
| **NVS** | `nvs_web_ctrl_settings_t` 含 **`sta_ssid` / `sta_password`**；与旧版 108 字节 blob **自动迁移**（读时补零 STA 字段）。 |
| **策略 A** | NVS 中 **`sta_ssid` 非空** 时先 **STA**，**`WEB_CTRL_STA_CONNECT_TIMEOUT_MS`** 内未拿到 IPv4 则 **停 Wi‑Fi → SoftAP** 再拉起 HTTP。 |
| **`GET /provision`** | 极简页：Scan → 轮询 **`GET /api/wifi/scan_result`** → 填 SSID/密码 → **`POST /api/wifi/save`**（须 **`Content-Length`**）；成功后 **重启**。 |
| **`GET /api/wifi/status`** | JSON：`mode`、`wifi_started`、`sta_has_ip`、`softap`。 |
| **`POST /api/wifi/scan`** | **202**：触发后台扫描；并发扫描返回 **503**。 |
| **`GET /api/wifi/scan_result`** | 完成前 **`pending":true`**；完成后返回 **`aps`** 列表（隐藏 SSID 显示为 `(hidden)`）。 |
| **`POST /api/wifi/save`** | Body：`{"ssid":"…","password":"…"}`；**`password` 可省略**（开放网络）。SoftAP 模式下仅允许客户端位于 **192.168.4.0/24**（防误触）；纯 STA 模式允许局域网写入。 |
| **`POST /api/wifi/sta`** | **501**：试连未实现；请使用 **`save`**。 |

## 3. menuconfig 配置过程（本工程菜单路径）

在 ESP-IDF 工程中执行 `idf.py menuconfig`（工作目录一般为 **`project/`**）。

1. **进入组件配置菜单**  
   在主界面选择 **`Component config  --->`**（回车进入）。

2. **找到本组件菜单**  
   在 **`Component config`** 列表中进入 **`Web Control Library (web_ctrl)  --->`**。

3. **自动启动（可选）**  
   将 **`Auto-start SoftAP + HTTP server after board init`**（**`WEB_CTRL_AUTO_START`**）置为 **`[*]`**。

4. **指令超时（可选）**  
   同一菜单下调整 **`POST /api/cmd: max wait for command execution (ms)`**（**`WEB_CTRL_CMD_SYNC_TIMEOUT_MS`**），默认 **3000**。

5. **STA 等待超时（可选）**  
   调整 **`STA connect wait before SoftAP fallback (ms), strategy A`**（**`WEB_CTRL_STA_CONNECT_TIMEOUT_MS`**），默认 **15000**。

6. **保存并退出**  
   按 **`S`** 保存到 `sdkconfig`，或 **`Q`** 退出并按提示保存。

7. **若找不到菜单项**  
   menuconfig 中按 **`/`** 搜索 **`WEB_CTRL`**；仍无则 **`idf.py reconfigure`** 或清理 **`project/build`** 后重新配置，并确认 **`project/main/CMakeLists.txt`** 中含 **`REQUIRES web_ctrl`**。

---

## 4. 配置完成后的编译、烧录与验证

1. **编译**：在仓库根目录执行 `.\scripts\build.ps1`，或在 `project/` 下执行 `idf.py build`。  
2. **烧录**：`idf.py flash`（或你现有脚本）；需要日志时加 **`monitor`**。  
3. **连接**：手机或 PC 连接 Wi‑Fi **`ESP32-WebCtrl`**，密码 **`esp32web1`**（未改默认时）。  
4. **浏览器**（使用 **http**，勿用 https）：  
   - **`http://192.168.4.1/`** — 健康页 + 命令输入框与 **POST /api/cmd** 按钮  
   - **`http://192.168.4.1/provision`** — Wi‑Fi 配网（扫描 / 保存并重启）  
   - **`http://192.168.4.1/api/health`** — 应返回 **`{"ok":true}`**  
5. **curl 示例**（须带 **`Content-Length`**，与 `curl -d` 默认行为一致）：

```bash
curl -s -X POST http://192.168.4.1/api/cmd ^
  -H "Content-Type: application/json" ^
  -d "{\"line\":\"version\"}"
```

Linux / macOS 将行续符 `^` 换为 `\`。

---

## 5. 尚未在本组件内完全覆盖的内容（见总计划）

- **阶段 4 余量**：正式 UI、多指令面板、与 **灯效 / LCD / SD** 交错负载压测与问题闭环。  
- **阶段 5**：`CONFIG_ESP_HTTP_SERVER_*` 调优、根目录 README「Web 控制」小节、API Token / HTTPS / 限流等。

以上迭代以本 README 与 [`doc/embedded_coding_standard.md`](../../doc/embedded_coding_standard.md) 为准。

---

## 6. 工程如何依赖本组件

- **`project/CMakeLists.txt`**：`EXTRA_COMPONENT_DIRS` 需包含 **`../components`**（已配置）。  
- **应用 `CMakeLists.txt`**：在 `idf_component_register` 的 **`REQUIRES`** 中加入 **`web_ctrl`**（`project/main` 已加）。  
- **手动启动示例**（与 `WEB_CTRL_AUTO_START` 二选一，避免重复 `web_ctrl_start`）：

```c
#include "web_ctrl.h"

web_ctrl_config_t cfg;
web_ctrl_config_init_defaults(&cfg);
/* 按需修改 cfg.wifi / cfg.http_port */
(void)web_ctrl_start(&cfg);
```

---

## 7. 与编码规范及分层的关系

实现遵循 [`doc/embedded_coding_standard.md`](../../doc/embedded_coding_standard.md)：模块前缀命名、固定 `ESP_LOGx` 的 `TAG`、`esp_err_t` 检查、头文件最小暴露。

**依赖关系**：本组件 **`REQUIRES common`**，用于复用厂测命令表与 **`cmd_process_line_for_web`**；**不反向依赖** `project/main` 或业务模块。应用层通过 **`web_ctrl_start`** 决定是否拉起网络与控制栈。
