# 网页上传 BMP → 可选 LCD 显示 — 开发计划

**版本**：1.0  
**日期**：2026-05-14  
**关联文档**：[`doc/web_control_http_server_plan.md`](web_control_http_server_plan.md)、[`doc/sd_lcd_bitmap_plan.md`](sd_lcd_bitmap_plan.md)、[`doc/application_architecture.md`](application_architecture.md)  
**代码基线**：`components/web_ctrl`（`esp_http_server`）、`common/src/lcd_gallery.c`（SD 图库与 BMP 子集解码）、`project/main/start.c`（`BoardInit` 后可选 `web_ctrl_start`）

---

## 1. 目标

| 能力 | 说明 |
|------|------|
| **网页上传** | 用户在设备 HTTP 服务页选择本地 **BMP** 文件并提交；设备接收后**持久化或缓存**到可访问路径。 |
| **可选上屏** | 同一表单提供选项（如复选框）：**仅保存** / **保存并立即在 LCD 显示**。 |
| **体验与安全** | 明确错误提示（格式不支持、过大、无 SD、LCD 忙等）；首版可沿用 **SoftAP/局域网** 场景，后续再议鉴权。 |

**非目标（首版可排除）**：浏览器端 PNG/JPEG 解码、多文件并发上传、断点续传、HTTPS。

---

## 2. 现状与复用点

### 2.1 Web 层（`components/web_ctrl`）

- `web_server.c`：`GET /`（内嵌单页）、`GET /api/health`、`POST /api/cmd`（JSON `line` → `web_ctrl_cmd_execute_sync`）。
- POST 体读取：`web_http_read_post_body()` 将 **`content_len` 以内整包读入连续缓冲**（当前命令体上限约 **256 B** 量级），**不适合直接用于大图**。
- HTTP 配置：`max_uri_handlers` 已加大；`stack_size` 已 12 KiB 级，新增 handler 时仍避免在回调内做重活。

### 2.2 显示层（`common` / `cbb`）

- **BMP 子集**、全屏缩放至 **240×135**、条带推屏：已在 **`lcd_gallery.c`** 中实现（与 [`doc/sd_lcd_bitmap_plan.md`](sd_lcd_bitmap_plan.md) 一致）。
- **支持**：BI_RGB、无压缩、24/32 位；不支持 8bit 调色板、RLE、BI_BITFIELDS 等。
- **像素约定**：RGB565 大端、`lcd_show_picture` 参数 **`length`=宽、`width`=高**；与 ST7789 MADCTL/BGR 处理路径与现有图库一致。

### 2.3 生命周期（`project/main/start.c`）

- `BoardInit()` 成功后，若 **`CONFIG_WEB_CTRL_AUTO_START`**，则合并 NVS 并 **`web_ctrl_start()`**。
- SD 与 LCD 初始化顺序以板级为准；**无 SD 卡**时若仍要上传，需另选存储（见 §3.2）。

---

## 3. 方案概要

### 3.1 协议与 UI（推荐）

| 项 | 建议 |
|------|------|
| **页面** | 在 `web_server.c` 内嵌 HTML 增加一块 **「图片上传」** 卡片：`input type=file accept=".bmp"`、`checkbox`「上传后立即在 LCD 显示」、提交按钮。 |
| **提交方式** | **`multipart/form-data`**（标准文件上传），字段示例：`file`（二进制）、`display`（`on`/`off` 或 `1`/`0`）。 |
| **备选** | 纯 **`application/octet-stream`** + 查询参数 `?display=1`：实现简单，但浏览器原生表单支持弱，需 `fetch` 手写；可列为阶段 B。 |

### 3.2 存储路径

| 策略 | 适用 | 说明 |
|------|------|------|
| **A. 固定写 SD（推荐与图库一致）** | 已挂载 `BOARD_SDCARD_MOUNT_POINT` | 固件写入 **`/sdcard/wupload.bmp`**（≤8.3 短名，避免无 LFN 时 EINVAL）；与 `lcd_gallery` 扫描路径可分离或一并枚举；可选上传后 `lcd_gallery_refresh`。 |
| **B. 无 SD 时** | 演示板无卡 | **SPIFFS/LittleFS** 分区存单文件（需 Kconfig 限制最大体积 ≤ 分区容量）；或首版直接返回 **503 + 说明需插卡**。 |

**建议**：首版实现 **策略 A + 无卡明确报错**；策略 B 作为可选迭代项在 Kconfig 中开关。

### 3.3 执行路径（禁止在 HTTP 回调里长时间刷屏）

与 [`doc/web_control_http_server_plan.md`](web_control_http_server_plan.md) 一致：

1. **Handler**：解析 multipart 头与边界 → **边收边写文件**（`fopen`/`fwrite` 或 VFS），校验 **BMP 文件头魔数与基本字段**；可选快速拒绝超大 `content_len`（见 §4）。
2. **上屏**：若用户勾选显示，则向已有 **`web_ctrl_cmd` 队列** 投递一条**内部命令**，或新增**轻量消息队列**由 **`app_mod` / 专用显示任务** 调用与 `lcd_gallery` 同源的 **`show_bmp_file(lcd, w, h, path)`**（可将该路径抽为 `common` 内小 API，避免 HTTP 任务直接持 LCD）。
3. **互斥**：与 **GPIO0 切图**、**倾角 UI** 等共享 LCD 时，需统一 **mutex** 或「单一显示所有者」状态机，避免花屏（与现有 `main` 循环协调，具体以 `main.c` 为准）。

---

## 4. 分阶段 WBS

### 阶段 0：需求冻结与验收标准

- [ ] 最大上传体积（例如 **1～2 MiB** 上限，远大于 240×135×4 的 BMP，留余量给对齐行）。
- [ ] 固定落盘文件名或命名规则（是否覆盖同一文件）。
- [ ] 无 SD、格式错误、LCD 未初始化时的 JSON/HTML 错误文案。

### 阶段 1：HTTP 接收与落盘

- [ ] 新增 **`POST /api/upload/bmp`**（或 `/api/lcd/bmp`）注册于 `web_server_start`；评估 **`max_uri_handlers`** 是否需再加。
- [ ] 实现 **multipart 解析**（可自写边界扫描 + 首段 `Content-Disposition` 提取 filename，或引入轻量依赖；避免整包进 RAM）。
- [ ] **流式写入**：循环 `httpd_req_recv` → 写入文件；超时与连接断开时删除半成品文件。
- [ ] 响应 **`application/json`**：`{"ok":true,"path":"/sdcard/...","bytes":N}` 或错误码 + `error` 字段。

### 阶段 2：BMP 校验

- [ ] 写完后 **`fopen` 读文件头**：`BM`、偏移、`biCompression == BI_RGB`、`biBitCount ∈ {24,32}` 等，与 **`lcd_gallery.c`** 中 BMP 路径保持一致（必要时抽 **共用校验函数** 到 `common`，避免两套规则分叉）。
- [ ] 校验失败：删文件或保留诊断文件（产品决定），并返回明确 `error`。

### 阶段 3：可选 LCD 显示

- [ ] 在 **`common`** 或 **`web_ctrl`** 暴露 **`lcd_web_show_bmp_path(const char *path)`** 一类接口：内部仅 **入队**，由 **`web_ctrl_cmd` 任务** 或 **`main`** 注册的同一互斥路径执行 `show_bmp_*`。
- [ ] 网页勾选「立即显示」时，落盘成功后调用上述接口；返回中附带 **`"display":"queued"|"skipped"|"error"`**。
- [ ] 与 **图库模式** 关系：文档说明「网页指定文件」与「SD 根目录多图」优先级（例如网页显示为**一次性覆盖**，不修改图库索引；或显示后 `lcd_gallery_refresh`）。

### 阶段 4：前端与联调

- [ ] 扩展 `GET /` 内嵌页：上传区 + 进度（`fetch` + `XMLHttpRequest.upload.onprogress` 可选）+ 结果提示。
- [ ] SoftAP 下手机浏览器实测；STA 下大文件与 Wi‑Fi 稳定性测试。
- [ ] **栈与堆**：上传 handler 栈深度、SD 写入缓冲区大小；必要时将解析缓冲放 **PSRAM**（若工程已启用）。

### 阶段 5：硬化（可选）

- [ ] Kconfig：`WEB_CTRL_BMP_UPLOAD_MAX_BYTES`、`WEB_CTRL_BMP_UPLOAD_PATH`。
- [ ] 简单 **速率限制**（每 IP 间隔）或 Token（与厂测策略对齐）。
- [ ] 更新 [`components/web_ctrl/README.md`](../components/web_ctrl/README.md) 操作说明。

---

## 5. 风险与对策

| 风险 | 对策 |
|------|------|
| RAM 不足（整包读 body） | **禁止**复用命令 POST 的小缓冲模式；必须 **流式写盘**。 |
| Watchdog | 解码+推屏在 **非 httpd 任务**；长传 SD 可分片 `yield`。 |
| 与图库/按键争用 LCD | **统一互斥** + 文档化优先级。 |
| FAT 长文件名 | 与 [`doc/sd_lcd_bitmap_plan.md`](sd_lcd_bitmap_plan.md) 一致，优先 **短固定名** 或 8.3 安全名。 |

---

## 6. 文档与代码索引（实施时对照）

| 主题 | 路径 |
|------|------|
| HTTP 服务与内嵌页 | `components/web_ctrl/src/web_server.c` |
| 命令队列模式 | `components/web_ctrl/src/web_ctrl_cmd.c` |
| Wi‑Fi API 参考（POST 读 body） | `components/web_ctrl/src/web_ctrl_wifi_api.c` |
| BMP 显示实现 | `common/src/lcd_gallery.c`（`show_bmp_file` 等） |
| LCD 块写 | `cbb/lcd.c` — `lcd_show_picture` |
| Web 启动 | `project/main/start.c`（`CONFIG_WEB_CTRL_AUTO_START`） |

---

## 7. 验收检查清单（建议）

- [ ] 合法 **24/32-bit BI_RGB BMP** 上传成功，JSON `ok==true`，SD 上文件大小与上传一致。
- [ ] 勾选「立即显示」后 LCD 显示内容与 PC 预览一致（无色偏、无花屏）。
- [ ] 不勾选时仅落盘，LCD 保持当前画面。
- [ ] 非法格式、超大文件、无 SD：稳定返回错误，无泄漏句柄，无复位。
- [ ] 与 **GPIO0 切图** 交替操作无死锁。

---

**本文档为开发计划**；具体 API 路径与字段名可在实现阶段微调，但应保持在 **`web_ctrl` 组件** 内聚合，并与 **`lcd_gallery` / 板级初始化** 职责边界一致。
