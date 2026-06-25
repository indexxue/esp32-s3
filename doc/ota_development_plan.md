# ESP32-S3 OTA 开发计划与决策记录

**版本**：1.0  
**依据**：`flash_partition/partitions_16m_n16r8.md`、`doc/partition_switch_development_plan.md`、`common/src/boot_slot.c`  
**状态**：阶段 1（SoftAP/STA 本地 OTA）已在 `project/` **实机验证通过**（2026-06-25）；阶段 2（STA HTTPS）未开始。

---

## 已确认决策

| # | 议题 | 决策 | 日期 | 备注 |
|---|------|------|------|------|
| D1 | OTA 首要使用场景 | **E：分阶段组合** | 2026-06-25 | **阶段 1**：SoftAP 本地上传。**阶段 2**：STA + HTTP(S) 云端拉包。 |
| D2 | OTA v1 覆盖工程 | **A：仅 `project/`** | 2026-06-25 | v1 在量产工程打通全链路；`ballot_guard/` 待 v1.1 复用 `common/ota`。 |
| D3 | Bootloader rollback | **A：v1 即启用** | 2026-06-25 | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；启动成功后 `esp_ota_mark_app_valid_cancel_rollback()`。 |
| D4 | 镜像签名校验 / Secure Boot | **D：分阶段** | 2026-06-25 | 阶段 1：仅 `esp_ota_end` 合法性检查。阶段 2：至少 SHA256/manifest。 |
| D5 | 阶段 1 OTA 触发入口 | **E：Web 页 + HTTP API** | 2026-06-25 | `/ota` 维护页 + 独立 REST（见 D8）；v1 不含串口 OTA 命令。 |
| D6 | 版本号与 anti-rollback | **D：硬拒绝降级 + 统一版本源** | 2026-06-25 | 新包版本须严格大于当前；`PROJECT_VER` → `esp_app_desc` + `NVS_APP_VERSION_STRING`。 |
| D7 | 升级失败与断电行为 | **B：旧槽保底 + abort 清理** | 2026-06-25 | 见「边界场景」；断连 `esp_ota_abort`；写完未切仍旧槽；崩溃 rollback。 |
| D8 | HTTP API 形态 | **A：独立 `/api/ota/*`** | 2026-06-25 | 不走 `/api/cmd`；与 `/api/wifi/*` 同风格，支持大文件与长会话。 |

---

## 边界场景（D7）

| 场景 | 期望行为 |
|------|----------|
| **S1** 写入过程中断电/断连 | 继续从**旧槽**运行；**`esp_ota_abort`**；对侧槽半成品**不可切换**，下次覆盖重写 |
| **S2** `esp_ota_end` 成功、尚未切换 | 仍从**旧槽**启动；用户 **`POST /api/ota/apply`** 后再切换 + 复位 |
| **S3** 已切换但新固件反复崩溃 | Bootloader **rollback** 回旧槽（D3） |
| **S4** 上传版本 ≤ 当前版本 | **`ota_begin` 拒绝**（D6） |

---

## 阶段 1 HTTP API（D8 草案）

与 `web_ctrl` 现有 REST 风格一致；SoftAP 下写操作可限制在 `192.168.4.0/24`（对齐 `/api/wifi/save`）。

| 方法 | 路径 | 作用 |
|------|------|------|
| `GET` | `/ota` | 维护页（选文件、进度、确认重启） |
| `GET` | `/api/ota/status` | JSON：当前/对侧槽、运行版本、会话状态、已写字节 |
| `POST` | `/api/ota/upload` | 二进制 body（`Content-Length` 必填）；内部 `ota_begin` → 分块 `ota_write` → `ota_end` |
| `POST` | `/api/ota/abort` | 放弃当前会话（S1） |
| `POST` | `/api/ota/apply` | 校验通过后 `esp_ota_set_boot_partition` + `esp_restart()`（S2） |

**并发**：全局仅允许一个 OTA 会话；进行中返回 **503**。

---

## 背景（仓库现状）

| 已有 | 待做 |
|------|------|
| 双槽分区表、`boot_slot_*`、双槽烧录脚本 | `common/ota`（begin/write/end/abort/版本比较） |
| `web_ctrl` SoftAP + HTTP + `/api/wifi/*` 先例 | `/api/ota/*` + `/ota` 页 |
| 串口 `boot_a`/`boot_b`/`boot_q` | rollback menuconfig + 启动后 `mark_app_valid` |
| `NVS_APP_VERSION_STRING`（当前硬编码 `"1.0.0"`） | 与 `PROJECT_VER` 统一 |

---

## 实现 WBS（阶段 1 — v1）

### W1：配置与版本（0.5 天）

- [x] `project/sdkconfig.defaults` / bootloader：启用 **`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`**
- [x] `project/CMakeLists.txt`：`PROJECT_VER` 单一来源；生成或同步 `NVS_APP_VERSION_STRING`
- [x] `project/main/start.c`（或合适 init 点）：自检通过后调用 **`esp_ota_mark_app_valid_cancel_rollback()`**

### W2：`common/ota` 核心（1～2 天）

- [x] `common/inc/ota.h`、`common/src/ota.c`
- [x] `esp_ota_get_next_update_partition()` 选对侧槽
- [x] `ota_begin`：版本比较（D6）、拒绝降级
- [x] `ota_write` / `ota_end` / `ota_abort`（D7）
- [x] `ota_apply`：封装 `boot_slot` + 切换前状态检查（半成品不可 apply）
- [x] `common/CMakeLists.txt` 注册源文件（已依赖 `app_update`）

### W3：`web_ctrl` 集成（1～2 天）

- [x] `web_ctrl` 注册 `/api/ota/*` + `GET /ota`
- [x] Kconfig：上传 body 上限（`WEB_CTRL_OTA_UPLOAD_MAX`）
- [x] `/ota` 页：进度轮询 `GET /api/ota/status`；完成后启用 apply
- [x] 仅 `project/` 启用 `CONFIG_WEB_CTRL_OTA`（`sdkconfig.defaults`）

### W4：测试与文档（1 天）

- [x] 测试矩阵 O1–O6 — **2026-06-25 实机验证通过**（上传 → apply → 对侧槽启动；`run_ver` 递增至 1.0.2）
- [x] `README.md` 增加 OTA 维护说明

### 阶段 2（后续，不在 v1 范围）

- STA + `esp_https_ota` 或等价客户端
- SHA256/manifest（D4）
- `ballot_guard/` 复用 `common/ota`（D2）

---

## 测试矩阵（阶段 1 验收）

| 编号 | 前置 | 操作 | 期望 |
|------|------|------|------|
| O1 | A 槽运行合法镜像 | 上传更高版本 `.bin` → apply | 重启进入 B 槽新固件；`mark_app_valid` 后 rollback 取消 |
| O2 | 同 O1 | 上传中途断连 | 仍 A 槽运行；status 为 idle/ aborted；不可 apply |
| O3 | 同 O1 | 上传完成不 apply 后断电 | 仍 A 槽运行 |
| O4 | 同 O1 | 上传低版本 | begin 拒绝（D6） |
| O5 | 新固件故意崩溃 | apply 后反复重启 | rollback 回 A（D3） |
| O6 | 两槽均有镜像 | `boot_q` / status API | 正确显示 run/next |

---

## 分阶段交付摘要

### 阶段 1 — SoftAP 本地 OTA（v1）

- **传输**：`/ota` + `/api/ota/*`（D5、D8）
- **核心**：`common/ota` + rollback（D3）+ 禁止降级（D6）+ abort（D7）
- **安全**：无签名校验（D4 阶段 1）
- **范围**：仅 `project/`（D2）

### 阶段 2 — STA + HTTP(S) 云端 OTA

- SHA256/manifest（D4）
- 断点/重试、版本检查 API
- 验收仍须符合 D3/D7

---

## Grill 队列（已全部完成）

- [x] D1 → E　[x] D2 → A　[x] D3 → A　[x] D4 → D  
- [x] D5 → E　[x] D6 → D　[x] D7 → B　[x] D8 → A  

---

## 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-06-25 | 0.1–0.7 | Grill D1–D7 逐项确认 |
| 2026-06-25 | 1.0 | Grill D8 确认（A）；决策冻结；补充 API 草案、WBS、测试矩阵 |
| 2026-06-25 | 1.1 | 实现阶段 1：common/ota、web_ctrl OTA HTTP、rollback、PROJECT_VER |
| 2026-06-25 | 1.2 | 实机验收通过（run=app_b, run_ver=1.0.2） |
| 2026-06-25 | 1.3 | 新增 `idf.py release` 与 `firmware/` 发布目录（见 `firmware/README.md`） |

---

## 发布工作流（release，与 OTA 配合）

| 步骤 | 命令 / 产物 |
|------|-------------|
| 打 release 包 | `idf.py -C project release 1.2.3`（`-DPROJECT_VER`，不改 CMakeLists；默认仅 OTA app；产线加 `--flash-bundle`） |
| 归档位置 | `firmware/1.2.3/project_1.2.3_YYYYMMDD.bin`、`.hex` 等（见 `firmware/README.md`） |
| 现场 OTA | 上传 **`project_1.2.3_YYYYMMDD.bin`**（版本须高于设备 `run_ver`） |

详见 [`firmware/README.md`](../firmware/README.md)。
