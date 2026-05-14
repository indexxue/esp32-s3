# ESP32-S3 项目

本仓库仅跟踪源代码。本地的 ESP-IDF 工具链与构建产物不会提交到版本库。

## 环境要求

- Windows 10/11
- Git for Windows（零安装引导流程需要）

## 首次配置

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
```

配置脚本支持「零安装」流程：

- 若未检测到 ESP-IDF，会自动克隆 `v5.5.4` 到 `.\Espressif\frameworks\esp-idf-v5.5.4`
- 随后运行 `install.bat` 安装或更新工具链依赖

然后**新开**一个终端，加载 ESP-IDF 环境：

```cmd
cmd /k ".\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
```

## 构建

建议在仓库根目录一键构建：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

可选操作：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action reconfigure
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action clean
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action fullclean
```

传统 ESP-IDF 命令：

```cmd
idf.py -C project build
```

若当前工作目录已是 `project`，可使用：

```cmd
idf.py build
```

## 烧录固件（双 OTA 槽位）

本仓库使用**自定义分区表**（`flash_partition/partitions_16m_n16r8.csv`）：**`app_a` / `ota_0` = 槽位 A**，**`app_b` / `ota_1` = 槽位 B**，另有 `otadata` 用于启动选择。详见 `flash_partition/partitions_16m_n16r8.md` 与 `doc/partition_switch_development_plan.md`。

### 默认 `idf.py flash` 始终写入槽位 **A**（`app_a`）

无论是量产应用（`project/`）还是工厂应用（`factory/`），常规 ESP-IDF 烧录都会写入分区表中的**第一个**应用分区，即 **`app_a`（槽位 A）**。

| 命令 | 应用镜像写入位置 |
|--------|----------------------------------|
| `idf.py -C project -p PORT flash` | **槽位 A**（`project/build/project.bin` → `app_a`） |
| `idf.py -C factory -p PORT flash` | **仍是槽位 A**（`factory/build/factory.bin` → `app_a`） |

**注意：**`idf.py -C factory flash` **不会**烧录到槽位 B，它会覆盖与量产工程相同的**槽位 A** 镜像。若你只想更新**槽位 B**，请使用下文脚本。

### 烧录槽位 **A**（量产应用）

1. 构建：`.\scripts\build.ps1`
2. 烧录：`idf.py -C project -p PORT flash`（可选：末尾加 `monitor` 监视串口）

### 烧录槽位 **B**（工厂应用，不改动 A）

1. 构建工厂镜像：`powershell -ExecutionPolicy Bypass -File .\scripts\build_factory.ps1`
2. **仅**将 `app_b` 烧录到偏移 `0x00810000`：

```powershell
powershell -ExecutionPolicy Bypass -File .\factory\flash_app_b.example.ps1 -Port PORT
```

将 `PORT` 替换为你的 COM 口（例如 `COM13`）。脚本使用仓库内的 IDF Python 环境（`python -m esptool`）；若缺少该 Python，请先运行 `.\scripts\setup_env.ps1`。

### 一步烧录：bootloader + 分区表 + 槽位 A + 槽位 B

在**同时**构建好 `project` 与 `factory` 之后：

```powershell
powershell -ExecutionPolicy Bypass -File .\factory\flash_dual_slot.example.ps1 -Port PORT
```

对**已整片擦除**的芯片，建议先执行一次 `idf.py -C project -p PORT flash`（使 bootloader、分区表、`otadata` 等一致），再用 `flash_app_b.example.ps1` 写入槽位 B。更多说明见 `doc/factory_project.md`、`factory/README.txt` 与 `doc/compile_flash_erase_monitor.md`。

### 运行时：在 A / B 之间切换下次启动

固件中若已启用 USB 串口命令行，可使用 **`boot_a`**、**`boot_b`**、**`boot_q`**（见 `common/src/cmd.c`）。实现基于 `esp_ota_set_boot_partition()`（`common/src/boot_slot.c`）。

## 校验环境脚本

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
```

可选完整检查：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1 -RunInstall -RequireIdfInPath -RunBuildTest
```

## 说明

- `Espressif/` 在 Git 中刻意忽略，各开发者本地自行安装工具。
- 若使用其他 ESP-IDF 版本或自定义安装路径，请同步修改 `scripts/setup_env.ps1`。
- 可复用的自定义组件放在 `libraries/`（例如 `libraries/my_component/...`）或顶层组件目录（例如 `common/...`）。
