# ESP32-S3 分区切换（双槽启动）开发计划

**版本**：1.0  
**依据**：`tmp/boot_slot.c` / `tmp/boot_slot.h`（STM32 切换思路）、`flash_partition/partitions_16m_n16r8.md`（当前 Flash 布局与 CSV）、ESP-IDF 双槽 OTA 启动机制。  
**目标**：在 ESP32-S3 上实现与「厂测槽 / 正式槽」切换等价的工程能力，并给出可落地的分阶段任务与验收标准。

---

## 1. 背景与目标

### 1.1 业务目标

- 设备可在 **正式应用（App_A，`ota_0`）** 与 **厂测 / 备用应用（App_B，`ota_1`）** 之间切换下次上电启动分区。
- 行为上对齐你在 STM32 侧的直觉：**请求某槽 → 持久化意图 → 复位 → Bootloader 按意图启动对应镜像**。
- 与现有分区表一致：`app_a` / `app_b` 等容、`otadata` 已预留（见 `flash_partition/partitions_16m_n16r8.md`）。

### 1.2 非目标（首版可明确排除）

- 不讨论「自定义 Bootloader 读私有 Flash 魔数」替代 `otadata`（维护成本高，与 IDF 工具链耦合差）；首版以 **ESP-IDF 官方 OTA 启动路径** 为准。
- 完整 OTA 下载、HTTPS、签名校验可作为后续迭代；本文聚焦 **启动槽选择与切换**。

---

## 2. STM32 参考实现摘要（`tmp/boot_slot`）

| 能力 | 实现要点 |
|------|----------|
| 固定标志地址 | `BOOT_SLOT_FLAG_ADDR`（单字对齐地址），魔数 `BOOT_SLOT_FLAG_FACTORY` 表示请求厂测类启动。 |
| 请求厂测 | 若已为魔数则直接成功；否则 `HAL_FLASH_Program` 写入，失败则擦除所在页后重试。 |
| 请求 App_A | 擦除包含标志的整页，使标志回到「非厂测」默认语义。 |
| 生效方式 | `boot_slot_system_reset()` → `NVIC_SystemReset()`。 |

**映射到 ESP32-S3**：不在应用区外再维护一套「私有魔数字」作为 Bootloader 唯一依据，而是使用 **`otadata` 分区** 中由 ESP-IDF 维护的元数据，通过 **`esp_ota_set_boot_partition()`**（及配套查询 API）表达「下次从哪个 `ota_x` 分区启动」。应用层可提供与 `boot_slot_request_*` 同语义的薄封装，便于代码迁移与评审对齐。

---

## 3. ESP32-S3 分区与启动链（现状）

以下与仓库内 `partitions_16m_n16r8` 文档一致，仅列与切换相关的行：

- **Bootloader**（片头）→ 读 **分区表** → 读 **`otadata`** → 选择 **`ota_0` / `ota_1`** 对应物理分区上的应用镜像。
- **`app_a`**：`subtype ota_0`，对应文档中的 **App_A（量产主应用）**。
- **`app_b`**：`subtype ota_1`，对应 **厂测 / 对侧槽**。
- **`otadata`**：必须为 `data, ota`，大小当前为 8 KiB；**不要**用手工擦写替代 IDF 提供的更新流程，除非你愿意承担与 esptool/OTA 工具不一致的风险。

详细地址与 CSV 片段以 `flash_partition/partitions_16m_n16r8.md` 为准；应用内逻辑地址请优先 **`esp_partition_*`**，避免硬编码 SPI Flash 线性偏移与 MMIO 映射混淆。

---

## 4. 技术对齐：STM32「标志位」与 ESP-IDF「启动分区」

| 维度 | STM32（参考代码） | ESP32-S3（推荐做法） |
|------|-------------------|----------------------|
| 持久意图存哪 | 固定 Flash 地址上的魔数 | **`otadata`**（由 Bootloader + `esp_ota_*` 协同维护） |
| 谁决定启动镜像 | 自研 Boot / 启动汇编分支 | **2nd stage bootloader** 读 `otadata` |
| 应用如何请求切换 | 写 Flash + 复位 | **`esp_ota_set_boot_partition()`** + **`esp_restart()`** |
| 清除「厂测请求」 | 擦除标志所在页 | 将下次启动分区设为 **`ota_0`**（或你定义的默认槽） |
| 与 OTA 写入关系 | 无（独立标志） | **向非运行槽写入** 与 **切换启动分区** 解耦；切换前须保证目标分区镜像合法 |

**结论**：ESP 侧「分区切换」的工程实质是 **合法地改写启动元数据并复位**，而不是在 `app_a`/`app_b` 之外再刻一个与 Bootloader 无契约的私有标志（若强需求「可读审计标志」，可额外用 **NVS 键** 记录「用户上次请求的槽」，但 **真正启动仍以 `otadata` 为准**，避免双源不一致）。

---

## 5. 方案设计（推荐）

### 5.1 启动策略（需在 `menuconfig` 中定稿）

与产品策略二选一或组合（需书面确认默认行为）：

1. **默认量产**：上电默认从 **`ota_0`（App_A）** 启动；厂测或维护命令切到 **`ota_1`**。  
   - 对应关注：`CONFIG_BOOTLOADER_APP_ROLLBACK`、是否启用 **anti-rollback** 等（若后续 OTA）。
2. **试产/开发**：允许从上次运行槽继续，或固定从 `ota_0` 试起（便于现场恢复）。

具体选项以 ESP-IDF 版本文档为准，关键词：**`CONFIG_BOOTLOADER_APP_*`**、**`FACTORY`**（若将来增加 `factory` 子类型分区，与当前「双 `ota`」模型不同，需另开文档）。

### 5.2 应用层模块建议

新增小型模块（命名示例，可与 STM32 对齐）：

- **`boot_slot_request_factory()`** → 解析分区表找到 **`ota_1`**（或你命名的 `app_b`），校验子类型后 **`esp_ota_set_boot_partition()`**，返回码转布尔或 `esp_err_t`。
- **`boot_slot_request_app_a()`** → 对 **`ota_0`** 执行同上。
- **`boot_slot_system_reset()`** → **`esp_restart()`**（注意：与 FreeRTOS 任务上下文、日志 flush、外设去初始化策略一致）。

内部实现应：

- 使用 **`esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_x, NULL)`** 或 **`esp_ota_get_running_partition()`** + **`esp_ota_get_next_update_partition()`** 辅助理解当前/对侧槽，避免硬编码偏移。
- **禁止**在应用里直接 `spi_flash_erase` / 写 `otadata` 原始扇区（除非走 IDF 未覆盖的极端定制）。

### 5.3 与串口命令 / 产测的衔接

若产线或现场通过串口切换（可参考 `tmp/serial_cmd.c` 的思路），建议：

- 命令层只调上述 **`boot_slot_*` 封装**，统一日志与错误码。
- 切换前可选：**校验目标分区是否有有效镜像**（例如读取镜像头 magic、或调用 **`esp_ota_get_partition_description`** / 项目内已有校验逻辑），避免指到空槽导致反复重启失败。

---

## 6. 开发阶段与交付物（WBS）

### 阶段 0：需求与策略冻结（0.5～1 天）

**交付物**：一页纸「启动默认槽 / 厂测槽定义 / 是否允许现场切回 / 失败时行为」。

- 确认：`app_a` = 量产，`app_b` = 厂测（与 `partitions_16m_n16r8.md` 角色表一致）。
- 确认：两槽镜像 **是否同签名校验策略**、版本号规则。

### 阶段 1：构建与 Bootloader 配置（0.5～1 天）

**交付物**：可编译烧录的 `sdkconfig`（或 `sdkconfig.defaults`）片段 + 变更说明。

- 启用 **`CONFIG_PARTITION_TABLE_CUSTOM`**，指向 **`../flash_partition/partitions_16m_n16r8.csv`**（路径以工程根为准）。
- 核对 **Bootloader 大小** 与分区表首项偏移（文档已给片头 36 KiB 量级，改 Bootloader 偏移须全链路重算）。
- 按产品选择配置 **`CONFIG_BOOTLOADER_APP_*`** 相关项，并记录默认值对「首次烧录仅烧一槽」行为的影响。

### 阶段 2：封装库实现（1～2 天）

**交付物**：`common`（或独立组件）下的 `boot_slot.c` / `boot_slot.h`（或 `slot_switch.*`），单元测试或最小桩测试。

- 实现 **查询当前运行分区**、**设置下次启动分区**、**安全复位**。
- 错误处理：`ESP_ERR_OTA_VALIDATE_FAILED`、分区未找到、子类型不匹配等统一映射为日志 + 返回值。

### 阶段 3：集成入口（0.5～1 天）

**交付物**：串口 / 按键 / 产测协议中至少一条可触发切换的路径。

- 与 `cmd.c` 或产测命令表注册命令（例如 `BOOT_A` / `BOOT_B`）。
- 可选：上电打印当前 **`esp_ota_get_running_partition()`** 标签，便于现场判读。

### 阶段 4：双镜像烧录与产线脚本（1 天）

**交付物**：`esptool.py` 或 `idf.py flash` 的分步说明 / 脚本：分区表 + `bootloader` + `app_a` + `app_b`（按需）+ `nvs`/`phy_init` 初始化策略。

- 明确：**仅烧 `ota_0`** 时设备能否按预期启动（取决于 Bootloader 默认槽配置）。
- 明确：**厂测镜像** 首次写入 `ota_1` 的地址与 **合并 bin** 时的偏移。

### 阶段 5：测试与验收（1～2 天）

**交付物**：测试记录表（见第 8 节矩阵）+ 已知问题列表。

---

## 7. Bootloader / 配置检查清单（实施时逐项打勾）

- [ ] 自定义分区表已编入固件且与 **`partitions_16m_n16r8.md`** 一致。  
- [ ] **`otadata`** 存在于 CSV，且未被其它用途覆盖。  
- [ ] **`app_a` / `app_b`** 子类型分别为 **`ota_0` / `ota_1`**，大小相等（当前为 `0x7E0000`，受 IDF `app` 偏移 64KiB 对齐与 16MiB 上限约束，见 `partitions_16m_n16r8.md`）。  
- [ ] 应用 `CMakeLists.txt` 已依赖 **`app_update`**（或等价组件），链接 **`esp_ota_*`** 符号。  
- [ ] 切换后执行 **`esp_restart()`** 前，关键外设（LCD 总线、SD 写缓存等）是否需 **deinit**（按板级策略）。  
- [ ] 日志：切换命令与 **`esp_ota_set_boot_partition`** 返回值必须可追踪。

---

## 8. 测试用例矩阵（建议）

| 编号 | 前置条件 | 操作 | 期望 |
|------|----------|------|------|
| T1 | 仅 `ota_0` 有合法镜像 | 上电 | 正常进入 App_A |
| T2 | 两槽均有合法镜像，当前跑 A | 请求启动 B + 复位 | 进入 App_B |
| T3 | 当前跑 B | 请求启动 A + 复位 | 进入 App_A |
| T4 | `ota_1` 为空或损坏 | 请求启动 B + 复位 | Bootloader 重试 / 回退策略符合 `menuconfig`（记录实际行为） |
| T5 | 连续快速发送切换命令 | 不崩溃、`otadata` 仍一致 | 以最后一次有效请求为准 |
| T6 | OTA 写入对侧过程中断电（若后续做 OTA） | 上电 | 不砖；行为符合是否启用 rollback |

---

## 9. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 目标槽无有效镜像导致无法启动应用 | 切换前校验镜像头；Bootloader 配置回退；量产保证两槽烧录策略 |
| `otadata` 与人为 NVS「意图」不一致 | 单一真相源：**`otadata`**；NVS 仅作显示/审计 |
| 分区表变更后地址常量漂移 | 仅用 **`esp_partition_*`**；文档常量仅用于脚本对账 |
| FreeRTOS 上下文中复位 | 切换放在任务中并避免持锁复位；必要时延迟到 idle |

---

## 10. 与 STM32 代码的刻意对齐点（便于评审）

- 保留 **`boot_slot_request_factory` / `boot_slot_request_app_a` / `boot_slot_system_reset`** 三个入口语义，实现改为 **`esp_ota_*` + `esp_restart()`**。  
- STM32 的 **`BOOT_SLOT_FLAG_ADDR` 魔数** 不在 ESP 首版复刻；若强需求「与产测机协议兼容的魔数」，建议放在 **NVS** 或 **产测专用 data 分区**，并文档声明 **不参与 Bootloader 决策**。

---

## 11. 参考资料（仓库内）

- `flash_partition/partitions_16m_n16r8.md` — 分区地址、CSV、`sdkconfig` 片段。  
- `tmp/boot_slot.c`、`tmp/boot_slot.h` — STM32 侧参考语义。  
- `factory/README.txt` — 厂测工程 **`factory/`** 构建与烧录到 **`app_b`** 的步骤。  
- ESP-IDF 编程指南：**Over-The-Air Updates**、**Partition Tables**、**Bootloader**（以当前 IDF 版本为准）。

---

## 12. 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-05-13 | 1.0 | 初版：基于 `tmp/boot_slot` 与 `partitions_16m_n16r8` 的输出开发计划。 |
