# ESP32-S3 OTA 开发计划与决策记录

**版本**：2.2  
**依据**：[`flash_partition/partitions_16m_n16r8.md`](../flash_partition/partitions_16m_n16r8.md)、[`doc/partition_switch_development_plan.md`](partition_switch_development_plan.md)、[`common/ota/`](../common/ota/)  
**状态**：阶段 1 **O1–O6 实机验收通过**（2026-06-26）；阶段 2 **W5–W9 已完成、O7 实机验收通过**（2026-06-26），**阶段 2 交付收尾**。**O8–O10 实机验收延后**（待办）。**阶段 3 W10–W11 已实现**（`project` signed_ota / secure_boot profile）；**O11–O13 实机待办**（见 [`doc/secure_boot_production.md`](secure_boot_production.md)）。

---

## 术语（与分区切换文档对齐）

| 术语 | 分区 / API | 说明 |
|------|------------|------|
| **运行槽** `run` | `esp_ota_get_running_partition()` | 当前正在执行的 app 分区 |
| **对侧槽** `target` | `esp_ota_get_next_update_partition()` | OTA 写入目标（非运行槽） |
| **下次启动槽** `next` | `esp_ota_get_boot_partition()` | `otadata` 记录的下次上电槽（`boot_q` / status 展示） |
| **App_A / 槽 A** | `app_a` / `ota_0` | 量产主应用默认槽（见分区表） |
| **App_B / 槽 B** | `app_b` / `ota_1` | 对侧槽 / 厂测备用槽 |

串口 **`boot_a` / `boot_b` / `boot_q`** 走 [`common/ota/src/boot_slot.c`](../common/ota/src/boot_slot.c)（`esp_ota_set_boot_partition`）。
OTA **`apply`** 同样经 IDF OTA API 切槽，**不经过** `boot_slot_request_*`，但语义一致。

---

## 已确认决策

| # | 议题 | 决策 | 日期 | 备注 |
|---|------|------|------|------|
| D1 | OTA 首要使用场景 | **E：分阶段组合** | 2026-06-25 | **阶段 1**：SoftAP 本地上传。**阶段 2**：STA + HTTP(S) 云端拉包。 |
| D2 | OTA v1 覆盖工程 | **A：仅 `project/`** | 2026-06-25 | v1 在量产工程打通全链路；`ballot_guard/` 待 v1.1 复用 `ota`。 |
| D3 | Bootloader rollback | **A：v1 即启用** | 2026-06-25 | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；**平台自检全部通过后**调用 `esp_ota_mark_app_valid_cancel_rollback()`（见「Rollback 保护窗口」）。 |
| D4 | 镜像签名校验 / Secure Boot | **D：分三阶段** | 2026-06-25 / **2026-06-26 细化** | **阶段 1**：仅 `esp_ota_end` 格式检查。**阶段 2**：SHA256 + manifest 字段（完整性，非身份）。**阶段 3**：Secure Boot v2 + 发布链签名（身份 + 启动强制），见「阶段 3」与「SHA256 vs 镜像签名」。 |
| D5 | 阶段 1 OTA 触发入口 | **E：Web 页 + HTTP API** | 2026-06-25 | `/ota` 维护页 + 独立 REST（见 D8）；v1 不含串口 OTA 命令。 |
| D6 | 版本号与 anti-rollback | **D：硬拒绝降级 + 统一版本源** | 2026-06-25 | 新包版本须**严格大于**当前（semver 三段比较）；`PROJECT_VER` → `esp_app_desc.version` + `NVS_APP_VERSION_STRING`（[`common/CMakeLists.txt`](../common/CMakeLists.txt)）。**未**启用 eFuse `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`（阶段 2 再评估）。 |
| D7 | 升级失败与断电行为 | **B：旧槽保底 + abort 清理** | 2026-06-25 | 见「边界场景」；断连 `esp_ota_abort`；写完未 `apply` 仍旧槽；崩溃 rollback（D3）。 |
| D8 | HTTP API 形态 | **A：独立 `/api/ota/*`** | 2026-06-25 | 不走 `/api/cmd`；与 `/api/wifi/*` 同风格，支持大文件与长会话。 |

---

## SHA256 vs 镜像签名（D4 细化）

| 维度 | SHA256（阶段 2 ✅） | 镜像签名 + Secure Boot（阶段 3 计划） |
|------|---------------------|--------------------------------------|
| **防什么** | 传输损坏、manifest 与 bin 不一致 | **未授权固件**写入 Flash 后**无法启动** |
| **谁生成** | `idf release` 对 `.bin` 算哈希 | 发布机用 **私钥**对镜像签名（`espsecure.py` / IDF 签名流程） |
| **设备谁验** | OTA 层 `ota_upload_end` 比对期望值 | **Bootloader** + `esp_ota_end` 验签（启用 SB 后 IDF 自动） |
| **能否证明发布方** | 否（攻击者可同时替换 manifest + bin） | 是（仅持有私钥者可产出可启动镜像） |
| **产线影响** | 无 | **eFuse 一次性烧录**密钥摘要；开发/量产密钥分离 |

**建议（2026-06-26 评估）**

| 场景 | 是否需要阶段 3 |
|------|----------------|
| 仅 **SoftAP 现场维护**、操作员可信、设备无公网 OTA | **可暂缓**；rollback + semver + AP 子网限制已覆盖主要误操作风险 |
| **STA 云端拉包**成为主路径，或设备长期暴露在客户网络 | **建议启用**；SHA256 不能抵御「恶意服务器整包替换」 |
| **`ballot_guard/`** 等安全敏感产品 | **建议启用**（可与 W9 一并规划） |
| **首批量产烧录前** | **必须完成 Grill**（P3-*）；eFuse 开启后**不可回退**，且 OTA **不能**更新已签名的 2nd-stage bootloader |

结论：**当前 v1/v2 不必阻塞发版**；**在首批量产 eFuse 烧录前**应完成阶段 3 决策与产线验证。若仅内网维护、无云端 OTA，可明确记录「接受风险、暂不启用 SB」并签字留档。

---

## 边界场景（D7）

| 场景 | 期望行为 |
|------|----------|
| **S1** 写入过程中断电/断连 | 继续从**旧槽**运行；**`esp_ota_abort`**；对侧槽半成品**不可 apply**，下次覆盖重写 |
| **S2** `esp_ota_end` 成功、尚未 `apply` | 仍从**旧槽**启动；用户 **`POST /api/ota/apply`** 后再 `esp_ota_set_boot_partition` + 复位 |
| **S3** 已 `apply` 但新固件在 **mark_valid 之前**反复崩溃 | Bootloader **rollback** 回旧槽（D3） |
| **S4** 上传版本 ≤ 当前版本 | **`ota_upload_begin` 拒绝**（D6），HTTP **409** |
| **S5** 新上传抢占旧会话 | 新 `upload` 前 **`abort` 旧会话**再开始（单会话抢占，非 503 排队） |

---

## Rollback 保护窗口（D3 细化）

新固件 `apply` 重启后处于 **`ESP_OTA_IMG_PENDING_VERIFY`**，在调用 `esp_ota_mark_app_valid_cancel_rollback()` 之前，Bootloader 可因启动失败自动回滚。

| 项目 | 阶段 1 现状 |
|------|-------------|
| **调用点** | `app_init()` 全阶段 OK 后，或 `web_ctrl_start` 成功后（[`project/main/start.c`](../project/main/start.c)） |
| **保护范围** | 覆盖 BoardInit / 按键 / 灯效失败；Web 启动失败时不 mark_valid |
| **O5 测试** | `CONFIG_OTA_ROLLBACK_TEST` 专用包 + **Flash 内 bootloader 已带 rollback**（须 USB 刷过）；**2026-06-26 实机通过** |

**原则**：「启动成功」= 文档 D3 与 W1 所指的自检通过，**不等于**仅 BoardInit 返回 OK。

---

## 阶段 1 HTTP API

与 `web_ctrl` 现有 REST 风格一致；SoftAP 下 **写操作**（upload / abort / apply）限制在 AP 子网（对齐 `/api/wifi/save`）。`GET /api/ota/status` 与 `GET /ota` 只读，不限子网。

| 方法 | 路径 | 作用 |
|------|------|------|
| `GET` | `/ota` | 维护页（选文件、进度、确认重启） |
| `GET` | `/api/ota/status` | JSON：`state`、`run`、`target`、`run_ver`、`pending_ver`、`written`、`total` |
| `POST` | `/api/ota/upload` | 二进制 body（`Content-Length` 必填）；`ota_upload_begin` → 分块 `ota_upload_write` → `ota_upload_end` |
| `POST` | `/api/ota/abort` | 放弃当前会话（S1、S5） |
| `POST` | `/api/ota/apply` | `OTA_SESSION_READY` 时 `esp_ota_set_boot_partition` + `esp_restart()`（S2） |
| `POST` | `/api/ota/pull` | **阶段 2**：JSON body `manifest_url`；STA 有 IPv4 时后台 HTTP(S) 拉包（见下） |

### 阶段 2 HTTP API（`/api/ota/pull`）

| 字段 | 说明 |
|------|------|
| `manifest_url` | `firmware/<ver>/manifest.json` 完整 URL（**http:// 或 https://**） |
| `product` | 默认 `project` |
| `apply` | `true` 时拉取成功后自动 `ota_apply` 并重启 |

**前置**：设备 STA 已取得 IPv4（`net_wifi_sta_has_ipv4()`）；SoftAP 且无 STA 时返回 **503** `sta_required`。  
**并发**：与 upload 相同，全局单会话；拉取进行中 `state=pulling`（写入阶段 `writing`，status 仍可能显示 `pulling`）。  
**校验**：manifest `version` 须高于 `run_ver`（begin 前）；镜像 SHA256 须与 manifest 一致（`ota_upload_end` 后，O8）。  
**完成判定**：`POST /api/ota/pull` 立即返回 `{"state":"pulling"}` 仅表示后台任务已启动；须轮询 `GET /api/ota/status` 至 `state=ready`（或 `apply=1` 后设备重启、`run_ver` 升高）。

### 会话与并发

- **全局单会话**：内存状态机 `idle` / `writing` / `ready` / `pulling`（[`common/ota/inc/ota.h`](../common/ota/inc/ota.h)）。
- **抢占策略**：新 `upload` 会先 `abort` 旧会话再 `begin`（S5）；**非**「进行中返回 503 拒绝第二路」。
- 互斥：`ota` 内部 mutex；HTTP 层与 `web_ctrl_ota.c` 串行处理单连接上传。

### 常见 HTTP 状态

| 状态 | 含义 |
|------|------|
| **409** | 版本不高于当前（upload / D6）；或 apply 时会话非 `ready`（如连点 apply） |
| **403** | SoftAP 下客户端不在 AP 子网 |
| **411** | 缺少 `Content-Length` |
| **413** | 超过 `CONFIG_WEB_CTRL_OTA_UPLOAD_MAX`（默认 8 MiB） |
| **408** | 接收超时（触发 abort，S1） |
| **503** | 内部 `ESP_ERR_INVALID_STATE`（少见）；**pull** 时 STA 无 IPv4（`sta_required`） |

实现：[`components/web_ctrl/src/web_ctrl_ota.c`](../components/web_ctrl/src/web_ctrl_ota.c)。

---

## 代码布局（阶段 1 已完成）

```
common/
  Kconfig            # CONFIG_OTA_ROLLBACK_TEST、CONFIG_OTA_HTTPS_PULL
  ota/inc/ota.h      # 会话 API + pull + SHA256 期望
  ota/inc/ota_manifest.h
  ota/inc/boot_slot.h
  ota/src/ota.c      # begin/write/end/abort/apply/confirm + SHA256
  ota/src/ota_manifest.c
  ota/src/ota_pull.c # HTTPS 流式下载 → ota_upload_*
  ota/src/boot_slot.c
components/web_ctrl/
  src/web_ctrl_ota.c # HTTP 适配层（upload + pull）
project/
  sdkconfig.defaults # rollback ON；ANTI_ROLLBACK / OTA_ROLLBACK_TEST 默认 OFF
  bootloader/sdkconfig.defaults
  main/start.c       # ota_confirm_running_image()（app_init / web_boot 成功后）
```

**版本同源链**：`PROJECT_VER`（[`project/CMakeLists.txt`](../project/CMakeLists.txt) 或 `idf.py release -DPROJECT_VER`）→ `esp_app_desc.version` → compile def `NVS_APP_VERSION_STRING` → 运行时 NVS `appver` 同步（[`common/src/nvs.c`](../common/src/nvs.c)）。详见 [`firmware/README.md`](../firmware/README.md)。

---

## 架构（模块与 seam）

```mermaid
flowchart LR
  subgraph transport [Transport 适配层]
    WEB[web_ctrl_ota]
    PULL[ota_pull HTTP(S)]
  end
  subgraph core [ota 深模块]
    OTA[ota_upload_* / ota_apply]
  end
  subgraph idf [ESP-IDF]
    EU[esp_ota_ops]
  end
  WEB --> OTA
  PULL --> OTA
  OTA --> EU
```

- **Transport seam**：HTTP 上传与未来的 HTTPS 拉包均应对接同一 `ota` 状态机，避免两套 `begin/end` 行为分叉。
- **校验 seam（阶段 2）**：在 `ota_upload_begin` 之前或 `esp_ota_end` 之后插入 manifest / SHA256 校验，不嵌入 `web_ctrl`。

---

## 实现 WBS

### 阶段 1 — v1（已完成）

#### W1：配置与版本

- [x] `project/sdkconfig.defaults` / `project/bootloader/sdkconfig.defaults`：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
- [x] `PROJECT_VER` 单一来源；`NVS_APP_VERSION_STRING` 由 CMake 注入
- [x] `ota_confirm_running_image()` 接入启动流程

#### W1.1：Rollback 窗口补强（已完成）

- [x] 将 `ota_confirm_running_image()` 移至 **`app_init()` 全阶段成功之后**（Web 异步时改由 `web_boot` 任务在 `web_ctrl_start` 成功后调用）
- [x] 自检失败路径：**不** mark_valid，保留 rollback 能力
- [x] 文档化 O5 专用 crash 测试固件构建方式（Kconfig `CONFIG_OTA_ROLLBACK_TEST`）

#### W2：`ota` 核心

- [x] [`common/ota/inc/ota.h`](../common/ota/inc/ota.h)、[`common/ota/src/ota.c`](../common/ota/src/ota.c)
- [x] `esp_ota_get_next_update_partition()` 选对侧槽
- [x] `ota_upload_begin`：首包解析 `esp_app_desc`、semver 比较（D6）
- [x] `ota_upload_write` / `ota_upload_end` / `ota_upload_abort`（D7）
- [x] `ota_apply`：`OTA_SESSION_READY` 检查 + `esp_ota_set_boot_partition` + 复位
- [x] `common/CMakeLists.txt` 注册，`REQUIRES app_update`

#### W3：`web_ctrl` 集成

- [x] `web_ctrl_ota_register()`：`/api/ota/*` + `GET /ota`
- [x] Kconfig `WEB_CTRL_OTA`、`WEB_CTRL_OTA_UPLOAD_MAX`
- [x] `/ota` 页：轮询 status、客户端版本预检、apply/abort
- [x] 仅 `project/` 启用 `CONFIG_WEB_CTRL_OTA`

#### W4：测试与文档

- [x] 测试矩阵 O1–O4、O6 — **2026-06-25 实机通过**（`run_ver` 递增至 1.0.2）
- [x] O5 rollback — **2026-06-26 实机通过**（W1.1 后 crash 包 OTA apply → 回旧槽；见「O5 构建与前提」）
- [x] 正常 OTA 升级 **1.0.4→1.0.5** — **2026-06-26 实机通过**（`CONFIG_OTA_ROLLBACK_TEST` 关闭的 release 包）
- [x] [`README.md`](../README.md) OTA 维护说明

#### W4.1：文档与 UI 同步（已完成）

- [x] `/ota` 页文案：改为推荐 **`idf.py release`** 产物名（`project_x.y.z_YYYYMMDD.bin`），弱化「手改 CMakeLists」
- [x] 指向 `manifest.json` → `products.project.ota.image` 作为现场选文件依据
- [x] apply：上传后 **status=`ready` 检查** + **防连点**；非 ready 时 HTTP 409 + `state=` 明细

---

### 阶段 2 — STA + HTTP(S) 云端 OTA（交付收尾 ✅）

#### 目标

- STA 联网后从 URL 拉取 release 包；校验与切换仍走 `ota` + D3/D7。
- 设备端消费 [`firmware/<ver>/manifest.json`](../firmware/README.md)（schema 2）中的 **SHA256**（D4）。
- `ballot_guard/` 启用 `CONFIG_WEB_CTRL_OTA`（D2 v1.1）。

#### 已冻结决策（阶段 2 Grill）

| # | 议题 | 决策 | 日期 |
|---|------|------|------|
| P2-1 | 下载实现 | **B**：HTTP(S) 流式下载 + 喂给 `ota_upload_*` | 2026-06-26 |
| P2-2 | manifest 校验点 | **C**：begin 前比 manifest version；end 后比 SHA256 | 2026-06-26 |
| P2-3 | STA 写操作安全 | **暂定**：pull 须 STA 有 IPv4；upload/apply 仍沿用 SoftAP 子网限制 | 2026-06-26 |
| P2-4 | eFuse anti-rollback | 暂不启用（与产线一并评估） | — |
| P2-5 | 断点续传 | 阶段 2 不做 | 2026-06-26 |

#### WBS

| 任务 | 内容 | 状态 |
|------|------|------|
| **W5** | `ota`：SHA256 钩子；`ota_pull.c` HTTP(S) adapter | ✅ |
| **W6** | `ota_manifest.c`；release 脚本写入 `sha256` | ✅ |
| **W7** | `POST /api/ota/pull` + `/ota` 页云端区；Kconfig | ✅ |
| **W7.1** | 联调修复：`fetch_headers` 误用、云端 UI 位置、拉包进度轮询 | ✅ 2026-06-26 |
| **W8** | 测试矩阵 O7–O10 | O7 ✅；O8–O10 **延后待办**（见下） |
| **W9** | `ballot_guard` 集成 + 文档 | ✅ 2026-06-26 |

#### 阶段 2 验收要点

- O7：STA 下从 manifest URL 拉取合法包 → apply → 对侧槽运行 — **✅ 2026-06-26**（本机 HTTP `8090`，1.0.6→1.0.7）
- O8：SHA256 不匹配 → 拒绝，`ota_abort`，旧槽不变 — **延后待办**
- O9：拉取中断 → 同 S1 — **延后待办**
- O10：仍满足 D6 降级拒绝、D3 rollback（阶段 1 已验 O5）— **延后待办**

#### 阶段 2 验收 O8–O10（延后待办 · 参考步骤）

> **2026-06-26 决策**：O8–O10 暂不进行实机验证；代码路径已实现，需要时可按下表补测。辅助脚本 [`scripts/ota_phase2_o8_corrupt_manifest.ps1`](../scripts/ota_phase2_o8_corrupt_manifest.ps1) 保留。

| 编号 | 前置 | 操作 | 期望 | 辅助 |
|------|------|------|------|------|
| **O8** | 设备 STA 有 IPv4；`run_ver` 已知 | 托管**篡改 SHA256** 的 manifest，拉包 | 串口 `SHA256 mismatch`；`GET /api/ota/status` → `state=idle`；**不可 apply**；`run_ver` 不变 | `scripts/ota_phase2_o8_corrupt_manifest.ps1 -Version <ver>` → 本机 HTTP 8090 提供 `manifest_o8_bad.json` |
| **O9** | 同 O7 | 拉包进行中**停止 HTTP 服务**或断网 | 同 O2：`state=idle`，旧槽运行 | 拉包开始后 `Ctrl+C` Python server 或拔网线 |
| **O10** | Flash 内 bootloader 含 rollback | 同 O5：`CONFIG_OTA_ROLLBACK_TEST` crash 包 OTA apply | 回旧槽 | 阶段 2 代码未改 rollback 路径；复测确认无回归 |

**O8 示例**（project，设备 1.0.6，托管 1.0.7 包）：

```powershell
.\scripts\ota_phase2_o8_corrupt_manifest.ps1 -Version 1.0.7 -Product project
cd firmware\_o8_test\1.0.7
python -m http.server 8090 --bind <PC_LAN_IP>
# POST /api/ota/pull  manifest_url=http://<PC_IP>:8090/manifest_o8_bad.json
```

**ballot_guard O7/O8**：release 须含 `products.ballot_guard`；`/api/ota/pull` 可省略 `product`（默认 `ballot_guard`）。sdkconfig 见 [`ballot_guard/sdkconfig.defaults`](../ballot_guard/sdkconfig.defaults)。

#### 本机 HTTP 联调（开发/验收 O7）

| 步骤 | 说明 |
|------|------|
| 1 | `idf -Project project release <ver>`，`<ver>` **高于** 设备 `run_ver` |
| 2 | `cd firmware/<ver>`，`python -m http.server 8090 --bind <PC_LAN_IP>` |
| 3 | 浏览器先验证 `http://<PC_IP>:8090/manifest.json` 返回 JSON（非 404） |
| 4 | 设备 STA 联网；`/ota` 或 `POST /api/ota/pull`，URL 须 **`http://` 双斜杠** |
| 5 | 轮询 status 至 `ready`，或 `apply=1` 等待重启后确认 `run_ver` |

**常见坑（2026-06-26 实机）**

| 现象 | 原因 | 处理 |
|------|------|------|
| 假 `HTTP 182` | 误把 `esp_http_client_fetch_headers()` 返回值当状态码（实为 Content-Length） | 已修：改用 `esp_http_client_get_status_code()` |
| `HTTP 404` + HTML「Access Error」 | PC 上 **8080 被其它服务占用**（如 ApplicationWebServer），Python 仅绑 `::` | 换端口（如 **8090**）+ `--bind <LAN_IP>` |
| 页面无 manifest 输入框 | 云端 HTML 曾误嵌入 `<script>` 内 | 已修 W7.1 |
| begin 拒绝 / 409 | manifest `version` ≤ `run_ver` | release 更高版本（如设备 1.0.6 → 托管 1.0.7） |

---

### 阶段 3 — Secure Boot v2 + 镜像签名（计划，量产前）

#### 目标

- 仅 **经发布私钥签名**的 app（及 bootloader）可在设备上启动；OTA 写入未签名/错签镜像时在 `esp_ota_end` 或启动阶段失败。
- 与 D3 rollback、D6 semver、阶段 2 SHA256 **叠加**：SHA256 仍建议在 pull 路径保留（下载完整性）；SB 负责**身份与启动信任根**。
- **首批量产**通过 USB `--flash-bundle` 烧录已签名 bootloader + partition + otadata + app；此后 OTA 仅更新 **已签名 app**。

#### 待 Grill 决策（P3-*）

| # | 议题 | 倾向 / 待决 | 备注 |
|---|------|-------------|------|
| P3-1 | 签名算法 | **暂定 RSA-3072**（`sdkconfig.defaults.*` + `secure-generate-signing-key -s rsa3072`） | 2026-06-26 开发默认；量产 Grill 可改 |
| P3-2 | 密钥管理 | **暂定**：`keys/dev` 联调、`keys/prod` 离线；私钥不进 Git | 见 [`keys/README.md`](../keys/README.md) |
| P3-3 | 启用时机 | **建议**：W9 完成后、**首批量产前**（O8–O10 可并行或前置补测） | eFuse 不可逆 |
| P3-4 | anti-rollback | 与 P2-4 **一并评估** | SB 常与 `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 组合；须与 D6 semver 策略对齐 |
| P3-5 | SoftAP 上传 | **建议**：SB 启用后 upload 路径**同样**须上传已签名 `.bin`（`esp_ota_end` 验签） | 无需单独「上传验签 API」；IDF 行为 |
| P3-6 | manifest 扩展 | **已实现**：`ota.signing_key_id` / `signing_profile`（schema 2 扩展字段） | 设备侧仍以 IDF 验签为准 |

#### WBS

| 任务 | 内容 | 状态 |
|------|------|------|
| **W10** | sdkconfig：`CONFIG_SECURE_BOOT`、签名 app/bootloader；`project/bootloader` 与 release 签名步骤 | ✅ 2026-06-26（可选 profile：`signed_ota` / `secure_boot`） |
| **W10.1** | `release_ext.py`：release 后自动签名产物；manifest 可选记录 `signing_key_id` | ✅ 2026-06-26（`--signing-profile`、`ota.signing_key_id`） |
| **W10.2** | 产线文档：eFuse 烧录顺序、`--flash-bundle` 首烧、**禁止**对已 SB 设备 USB 刷未签名包 | ✅ [`doc/secure_boot_production.md`](secure_boot_production.md) |
| **W11** | 密钥与 CI：开发/量产密钥分离；`idf release` 集成 `espsecure.py sign_data`（或 IDF 内置 signed app 构建） | ✅ 开发密钥脚本 + IDF 构建时签名；量产 CI 待 Grill |
| **W12** | 测试矩阵 O11–O13；与 O5 rollback、O8 SHA256 交叉复测 | 待实机 |

#### 阶段 3 验收要点

- **O11**：已 SB 设备 OTA 合法签名包 → apply → 正常运行 + mark_valid
- **O12**：OTA 未签名包或篡改签名 → `esp_ota_end` 失败 / 启动失败，旧槽可用
- **O13**：错误 semver 降级仍被 D6 拒绝（SB 不替代 semver）

#### 实现注意（ESP-IDF）

- 启用 SB 后 **2nd-stage bootloader 变更须 USB 重刷**，OTA app 分区不受影响（与 O5 说明一致，扩展至签名 bootloader）。
- 签名在 **构建/release** 侧完成，**不在** `web_ctrl` 或 `ota_pull` 内嵌私钥。
- 参考：[ESP-IDF Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html)、[Signed Applications](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html#signed-applications)。

---

## 测试矩阵

### 阶段 1（验收）

| 编号 | 前置 | 操作 | 期望 | 状态 |
|------|------|------|------|------|
| O1 | A 槽运行合法镜像 | 上传更高版本 `.bin` → apply | 重启进入 B 槽；mark_valid 后 rollback 取消 | ✅ |
| O2 | 同 O1 | 上传中途断连 | 仍 A 槽；state `idle`；不可 apply | ✅ |
| O3 | 同 O1 | 上传完成不 apply 后断电 | 仍 A 槽 | ✅ |
| O4 | 同 O1 | 上传低版本 | begin 拒绝，HTTP 409 | ✅ |
| O5 | 对侧槽待启动镜像 | apply **crash-test 固件**（mark_valid **前**崩溃） | rollback 回旧槽 | ✅ 2026-06-26 |
| O6 | 两槽均有镜像 | `boot_q` / `GET /api/ota/status` | 正确 `run` / `target` / `run_ver` | ✅ |

**O5 测试包要求**：在 `ota_confirm_running_image()` 调用点**之前**触发复位或 `abort()`；不可用「业务任务运行时崩溃」代替，否则当前实现已 mark_valid。

**量产 / 发布 sdkconfig 基线**（[`project/sdkconfig.defaults`](../project/sdkconfig.defaults)）：

| 配置 | 量产默认 |
|------|----------|
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | **y** |
| `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` | **关闭**（D6；勿在 menuconfig 误开） |
| `CONFIG_OTA_ROLLBACK_TEST` | **关闭** |
| `CONFIG_WEB_CTRL_OTA` | **y**（`project/`） |
| `CONFIG_OTA_HTTPS_PULL` | **y**（阶段 2 云端/HTTP 拉包） |
| `CONFIG_WEB_CTRL_OTA_CLOUD_PULL` | **y**（`/api/ota/pull` + `/ota` 云端区） |

**O5 构建**（仅专项测试，测完须关回并重新 release 正常包）：

1. 确认 Flash 内 **bootloader 已含 rollback**（改 rollback 相关项后至少一次 `idf -Project project flash`，OTA **不会**更新 bootloader）。
2. menuconfig → **Component config → OTA** → 勾选 **Crash in app_init before ota_confirm…**，保存 `sdkconfig`。
3. `idf -Project project build`（或临时 release）；**勿 USB 烧 crash 包**，仅 SoftAP OTA 上传 → apply。
4. 测毕：`# CONFIG_OTA_ROLLBACK_TEST is not set`，`build` / `release` 正常固件。

启用 `CONFIG_OTA_ROLLBACK_TEST` 后，`app_init()` 入口 `abort()`（mark_valid 之前），Bootloader 应在下一次启动回旧槽。

### 阶段 2（验收）

| 编号 | 操作 | 期望 | 状态 |
|------|------|------|------|
| O7 | STA + manifest URL 拉包（HTTP 本机或 HTTPS 云端） | 同 O1；manifest SHA256 + 版本校验 | ✅ 2026-06-26（1.0.6→1.0.7，`apply=1`，`ota begin → app_b`） |
| O8 | 篡改 bin 或 SHA256 | end 或校验阶段失败，旧槽运行 | 延后待办 |
| O9 | HTTP(S) 下载中断 | 同 O2 | 延后待办 |
| O10 | 阶段 2 变更后 | 复测 O5；rollback 仍生效 | 延后待办（阶段 1 O5 已通过） |

### 阶段 3（计划）

| 编号 | 操作 | 期望 | 状态 |
|------|------|------|------|
| O11 | 已 SB 设备 OTA 合法签名包 | apply 后新槽启动；mark_valid 正常 | 待办 |
| O12 | OTA 未签名或错签包 | 拒绝或无法启动；rollback/旧槽保底 | 待办 |
| O13 | SB 启用后上传低版本 | 仍 HTTP 409（D6 独立于 SB） | 待办 |

---

## 分阶段交付摘要

### 阶段 1 — SoftAP 本地 OTA（v1）✅

- **传输**：`/ota` + `/api/ota/*`（D5、D8）
- **核心**：`ota` + rollback（D3）+ semver 禁降级（D6）+ abort（D7）
- **安全**：无签名校验；SoftAP 写操作子网限制（D4 阶段 1）
- **范围**：仅 `project/`（D2）
- **发布**：`idf.py release` → [`firmware/`](../firmware/README.md)

### 阶段 2 — STA + HTTP(S) 云端 OTA（交付收尾 ✅）

- **传输**：`POST /api/ota/pull` + `/ota` 云端拉包区；`ota_pull.c`（HTTP/HTTPS 流式）
- **校验**：manifest version + SHA256（D4 阶段 2）；仍走 `ota_upload_*` + rollback（D3）
- **安全**：pull 须 STA IPv4（P2-3）；SoftAP upload 子网限制不变；**无镜像签名**
- **范围**：`project/`、`ballot_guard/` 已打通（W9）；release manifest 按 `products.<工程名>`
- **发布**：`idf release` → 托管 `firmware/<ver>/manifest.json`（含 `sha256`）
- **实机验收**：O7 ✅；O8–O10 延后待办（可选补测，见测试矩阵）

### 阶段 3 — Secure Boot + 镜像签名（计划）

- **信任根**：eFuse 中的 Secure Boot 公钥摘要 + 发布私钥签名链
- **校验**：`esp_ota_end` / 启动验签（D4 阶段 3）；可与阶段 2 SHA256 **并存**
- **产线**：首批 `--flash-bundle` 烧签名 bootloader；eFuse 一次性配置（P3-3）
- **范围**：量产 `project/`（及需 SB 的衍生工程）；Grill P3-* 后再动 sdkconfig

---

## 发布工作流（与 OTA 配合）

| 步骤 | 命令 / 产物 |
|------|-------------|
| 打 release 包 | `idf -Project project release 1.2.3`（注入 `PROJECT_VER`；默认仅 OTA app；产线首次烧录加 `--flash-bundle`） |
| 归档位置 | `firmware/1.2.3/project_1.2.3_YYYYMMDD.bin` 等 |
| 选哪个文件 | `firmware/<ver>/manifest.json` → `products.project.ota.image` |
| 现场 SoftAP OTA | 连接 AP → `http://192.168.4.1/ota` → 上传 `.bin`（版本须 **高于** `run_ver`） |
| 现场 STA 拉包 | STA 联网 → `/ota` 填 `http(s)://<host>/firmware/<ver>/manifest.json`，或 `POST /api/ota/pull` |

PowerShell 包装：[`idf.ps1`](../idf.ps1)、[`scripts/release.ps1`](../scripts/release.ps1)。详见 [`firmware/README.md`](../firmware/README.md)。

---

## Grill 队列

**阶段 1 决策（已冻结）**

- [x] D1 → E　[x] D2 → A　[x] D3 → A　[x] D4 → D  
- [x] D5 → E　[x] D6 → D　[x] D7 → B　[x] D8 → A  

**阶段 2 待 Grill**

- [x] P2-1 下载实现　[x] P2-2 manifest 校验点　[x] P2-3 STA 安全（暂定）  
- [ ] P2-4 eFuse anti-rollback　[x] P2-5 断点续传范围  

**阶段 2 延后待办（实机验收，可选）**

- [ ] **O8** SHA256 篡改拒绝（[`ota_phase2_o8_corrupt_manifest.ps1`](../scripts/ota_phase2_o8_corrupt_manifest.ps1)）
- [ ] **O9** HTTP(S) 下载中断 → abort，旧槽不变
- [ ] **O10** 阶段 2 后 rollback 回归（复测 O5）

**阶段 3 待 Grill（量产前必过）**

- [ ] P3-1 签名算法　[ ] P3-2 密钥管理　[ ] P3-3 启用时机  
- [ ] P3-4 anti-rollback 与 SB 组合　[ ] P3-5 SoftAP 上传策略　[ ] P3-6 manifest 扩展  

---

## 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-06-25 | 0.1–0.7 | Grill D1–D7 逐项确认 |
| 2026-06-25 | 1.0 | Grill D8 确认（A）；决策冻结；API 草案、WBS、测试矩阵 |
| 2026-06-25 | 1.1 | 实现阶段 1：ota、web_ctrl OTA HTTP、rollback、PROJECT_VER |
| 2026-06-25 | 1.2 | 实机验收通过（run=app_b, run_ver=1.0.2） |
| 2026-06-25 | 1.3 | 新增 `idf.py release` 与 `firmware/` 发布目录 |
| 2026-06-26 | 1.4 | 文档与实现对齐：术语表、路径修正、并发/HTTP 码、rollback 窗口与 O5 说明、架构 seam、阶段 2 WBS/P2 待决、W1.1/W4.1 待办 |
| 2026-06-26 | 1.6 | W1.1 rollback 窗口：`ota_confirm` 后移至 app_init / web_boot；`CONFIG_OTA_ROLLBACK_TEST`；W4.1 `/ota` 页文案 |
| 2026-06-26 | 1.7 | 阶段 1 收尾：O5 + 1.0.4→1.0.5 实机验收；sdkconfig 量产基线（rollback ON，ANTI_ROLLBACK / OTA_ROLLBACK_TEST OFF）；W4.1 apply 防连点；代码路径改 `common/ota` |
| 2026-06-26 | 1.8 | 阶段 2 W5–W7：SHA256、`ota_manifest`/`ota_pull`、`POST /api/ota/pull`、release manifest sha256 |
| 2026-06-26 | 1.9 | 阶段 2 O7 实机通过（本机 HTTP 8090，1.0.6→1.0.7）；W7.1 联调修复与「本机 HTTP 联调」章节；O8–O10/W9 仍待办 |
| 2026-06-26 | 2.1 | W9 ballot_guard OTA（sdkconfig、rollback、ota_confirm、WEB_CTRL_OTA_DEFAULT_PRODUCT）；O8 辅助脚本与 O8–O10 实机步骤 |
| 2026-06-26 | 2.3 | 阶段 3 W10–W11：`signed_ota`/`secure_boot` profile、密钥脚本、release `--signing-profile`、[`doc/secure_boot_production.md`](secure_boot_production.md) |
