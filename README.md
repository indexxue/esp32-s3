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

克隆后需初始化 **Git 子模块**（`cbb` 器件驱动库）：

```powershell
git submodule update --init --recursive
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
# cmd.exe（推荐 idf.cmd，勿直接运行 idf.ps1）
idf build
idf -Project ble_demo build

# PowerShell
.\idf.ps1 build
.\idf.ps1 -Project ble_demo build

powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Project ble_demo
```

可选操作：

```powershell
.\idf.ps1 reconfigure
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action reconfigure
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action clean
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action fullclean
```

`idf.ps1` / `scripts/build.ps1` 会自动配置 ESP-IDF 环境，并通过 `IDF_EXTRA_ACTIONS_PATH` 加载 `release` 等扩展（无需各工程 `idf_ext.py`）。

传统 ESP-IDF 命令（须先 `export.ps1`，且手动设置 `IDF_EXTRA_ACTIONS_PATH=scripts/idf_py_actions` 才能用 `release`）：

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

对**已整片擦除**的芯片，建议先执行一次 `idf.py -C project -p PORT flash`（使 bootloader、分区表、`otadata` 等一致），再用 `flash_app_b.example.ps1` 写入槽位 B。更多说明见 `factory/README.txt` 与 `doc/partition_switch_development_plan.md`。

### 运行时：在 A / B 之间切换下次启动

固件中若已启用 USB 串口命令行，可使用 **`boot_a`**、**`boot_b`**、**`boot_q`**（见 `common/src/cmd.c`）。实现基于 `esp_ota_set_boot_partition()`（`common/src/boot_slot.c`）。

### SoftAP 本地 OTA（维护页）

量产工程 `project/` 已启用 **`CONFIG_WEB_CTRL_OTA`**（见 `doc/ota_development_plan.md`）。连接设备 SoftAP 后：

1. 浏览器打开 **`http://192.168.4.1/ota`** — 选择 `project.bin` 上传，完成后点 **Apply & Reboot**
2. 或使用 API：`GET /api/ota/status`、`POST /api/ota/upload`（二进制 body + `Content-Length`）、`POST /api/ota/apply`

**注意：** 新固件版本须 **高于** 当前运行版本（`PROJECT_VER` / `esp_app_desc`）；上传写入对侧槽，确认前仍从旧槽启动。Bootloader rollback 已启用，新固件启动成功后自动 `mark_app_valid`。

测试 OTA 时：打 **git tag**（如 `v1.0.3`）后 `idf build` 自动使用该版本；`idf -Project project release x.y.z` 不会低于最高 tag（详见 [`firmware/README.md`](firmware/README.md)）。

### 发布版本（release）

```powershell
# 多工程同一版本：逐个追加
idf -Project project release 1.2.3
idf -Project ble_demo release 1.2.3

# 或一次 release 全部
idf -Project project release-all 1.2.3
powershell -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Version 1.2.3 -AllProjects
```

产物：`firmware/<版本>/{product}_{版本}_{编译日期}.bin` + `.hex` + `manifest.json`（见 [`firmware/README.md`](firmware/README.md)）。

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
- **`cbb/`** 为 Git 子模块（`git@github.com:indexxue/cbb.git`），克隆主仓库后执行 `git submodule update --init --recursive`。
- 若使用其他 ESP-IDF 版本或自定义安装路径，请同步修改 `scripts/setup_env.ps1`。
- 可复用的自定义组件放在 `libraries/`（例如 `libraries/my_component/...`）或顶层组件目录（例如 `common/...`）。
