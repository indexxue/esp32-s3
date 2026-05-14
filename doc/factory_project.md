# 厂测工程 `factory` 说明

**版本**：1.0  
**日期**：2026-05-13  

本文描述仓库内 **厂测独立 ESP-IDF 工程** `factory/` 的定位、与量产 `project/` 的关系、源码与产物命名、构建烧录步骤，以及与 **双槽分区 / 启动切换** 文档的衔接。

---

## 1. 目标与边界

| 项目 | 说明 |
|------|------|
| **工程路径** | 仓库根目录下 **`factory/`**（CMake `project(factory)`）。 |
| **固件产物** | `factory/build/factory.bin`（与 CMake `project(factory)` 名称一致）。 |
| **与量产关系** | 与 **`project/`** 共用 **`common`**、**`cbb`**、**`bsp_driver`** 及 **`flash_partition/partitions_16m_n16r8.csv`**；厂测专用逻辑写在 **`factory/main/main.c`**（或同目录新增 `.c` 并在 `CMakeLists.txt` 登记）。 |
| **不包含** | 量产 `project/main/main.c` 中的 LCD / SD / IMU 等业务任务；厂测壳层只做板级、按键、灯效、USB 厂测命令等「产线/维护基座」。 |

更完整的双槽策略、STM32 语义对齐、验收矩阵见 **`doc/partition_switch_development_plan.md`**；分区地址与 CSV 见 **`flash_partition/partitions_16m_n16r8.md`**。

---

## 2. 源码布局（`factory/main`）

| 文件 | 作用 |
|------|------|
| **`main.c`** | `app_main` → 调用 **`factory_entry()`**；在此追加厂测任务或 FreeRTOS 任务创建。 |
| **`factory.c`** | 厂测壳层实现（原 **`factory_start.c`**，已重命名为与工程一致的 **`factory.c`**）。 |
| **`factory.h`** | 生命周期类型与 **`factory_entry`** / **`factory_lifecycle_start`** 声明（原 **`factory_start.h`** → **`factory.h`**）。 |

头文件保护宏：**`FACTORY_MAIN_FACTORY_H`**（避免与组件或其它 `factory_*` 符号冲突）。

---

## 3. 命名与近期变更摘要

便于评审与 Git 历史对齐，建议记住以下演进：

1. **目录**：`project_factory` → **`factory`**。  
2. **CMake 工程名**：`factory_app` → **`factory`**，产物 **`factory.bin`**（原为 `factory_app.bin`）。  
3. **壳层源文件**：`factory_start.c` / `factory_start.h` → **`factory.c` / `factory.h`**（与工程名统一，便于在 IDE 中识别）。  
4. **API**：生命周期入口函数仍为 **`factory_entry()`**；表驱动启动函数为 **`factory_lifecycle_start()`**（与量产侧 `app_start` / `app_entry` 命名区分）。

---

## 4. 构建

前提：与本仓库 **`scripts/build.ps1`** 相同，已安装 **ESP-IDF 5.5.x** 及工具链（脚本内会 dot-source **`$IDF_PATH/export.ps1`** 以加入交叉编译器 `PATH`）。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_factory.ps1
```

- 工程目录：**`-C factory`**（脚本内写死为仓库根下的 `factory`）。  
- 若 **`factory/sdkconfig`** 不存在，脚本会先执行 **`idf.py set-target esp32s3`**。  
- 其它动作：`build_factory.ps1 -Action reconfigure|clean|fullclean` 与量产脚本语义相同。

---

## 5. 烧录与双槽约定

### 5.1 默认 `idf.py flash` 写到哪里？

对当前 **`partitions_16m_n16r8.csv`**，`idf.py -C factory flash` 会把应用镜像写到 **第一个应用分区 `app_a`（`ota_0`，偏移约 `0x30000`）**，与 **`idf.py -C project flash`** 的默认行为一致。  

**量产主应用应始终在 `app_a`。** 若把厂测固件仅用于 **B 槽（`app_b` / `ota_1`）**，不要用默认 flash 覆盖量产槽，除非你有意替换 A 槽镜像。

### 5.2 仅烧录厂测镜像到 B 槽（`app_b`）

B 槽起始地址以 **`flash_partition/partitions_16m_n16r8.md`** 为准（当前为 **`0x00810000`**）。仓库提供示例脚本：

```powershell
powershell -ExecutionPolicy Bypass -File factory/flash_app_b.example.ps1 -Port COM3
```

依赖：已成功构建并得到 **`factory/build/factory.bin`**。

### 5.3 量产 + 厂测两槽一次写入

见 **`factory/flash_dual_slot.example.ps1`**：从 **`project/build`** 取 bootloader、分区表与 **`project.bin`**（A 槽），从 **`factory/build/factory.bin`** 作为 B 槽（可按脚本说明调整路径与串口）。

### 5.4 运行时槽查询与切换

厂测与量产共用 **`common`** 中的 **`boot_slot_*`** 与 **`cmd`** 注册的命令（如 **`boot_q`**、**`boot_a`**、**`boot_b`**）。上电后可通过 USB 串口发 **`boot_q`** 查看 `run=` / `next=` 分区标签；详见 **`doc/partition_switch_development_plan.md`**。

---

## 6. 扩展厂测功能的建议

1. **在 `main.c` 的 `app_main` 中**：在 **`factory_entry()`** 成功返回后（或改为在 `factory.c` 的 `app_run` 阶段由你注册钩子——需自行改 `factory.c`），创建厂测专用任务。  
2. **新增 `.c/.h`**：放在 **`factory/main/`**，修改 **`factory/main/CMakeLists.txt`** 的 **`SRCS`**。  
3. **与量产共享算法**：抽到 **`common`** 或 **`cbb`**，避免在 `factory` 与 `project` 之间复制大段代码。  
4. **仅 B 槽运行的识别**：可在厂测固件中调用 **`boot_slot_format_status`** 或 **`esp_ota_get_running_partition()`**，若标签不是 **`app_b`** 则打印告警或拒绝执行危险项（按产品策略定）。

---

## 7. 相关文档与路径索引

| 资源 | 路径 |
|------|------|
| 双槽开发计划 | `doc/partition_switch_development_plan.md` |
| 分区表 CSV / 地址说明 | `flash_partition/partitions_16m_n16r8.csv`、`flash_partition/partitions_16m_n16r8.md` |
| 厂测工程 README（简版） | `factory/README.txt` |
| 厂测构建脚本 | `scripts/build_factory.ps1` |

---

## 8. 变更记录

| 日期 | 说明 |
|------|------|
| 2026-05-13 | 初版：`factory` 工程说明；收录 `factory_start` → `factory` 源文件重命名与构建/烧录指导。 |
