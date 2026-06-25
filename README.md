# ESP32-S3 项目

本仓库为 ESP32-S3（N16R8）固件 monorepo，包含量产应用、工厂镜像与示例工程。仅跟踪源代码；本地 ESP-IDF 工具链（`Espressif/`）与构建产物默认不提交。

| 工程 | 目录 | 说明 |
|------|------|------|
| `project` | `project/` | 量产主应用（SoftAP、Web 维护、OTA） |
| `factory` | `factory/` | 工厂/维护镜像（可烧录槽位 B） |
| `ble_demo` | `ble_demo/` | BLE GATT 示例 |
| `ballot_guard` | `ballot_guard/` | 选票监管应用 |

共享代码：`common/`（业务逻辑）、`bsp_driver/`（板级）、`cbb/`（器件驱动，**Git 子模块**）、`components/`（如 `web_ctrl`）。

## 环境要求

- Windows 10/11
- Git for Windows

## 首次配置

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
git submodule update --init --recursive
```

配置脚本会克隆 ESP-IDF **v5.5.4** 到 `.\Espressif\frameworks\esp-idf-v5.5.4` 并运行 `install.bat`。

也可手动加载环境（传统方式）：

```cmd
cmd /k ".\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
```

## 推荐命令入口：`idf.cmd` / `idf.ps1`

仓库根目录提供封装脚本，**自动配置 ESP-IDF、Ninja/CMake 路径**，并加载 `release` / `release-all` 扩展（`scripts/idf_py_actions/`）。

| 终端 | 用法 |
|------|------|
| **cmd.exe** | `idf build`、`idf -Project ble_demo build`（用 `idf.cmd`，勿直接双击 `idf.ps1`） |
| **PowerShell** | `.\idf.ps1 build` |

等价的 PowerShell 脚本：`scripts/build.ps1`、`scripts/release.ps1`。

支持的 `-Project`：`project`（默认）、`ble_demo`、`factory`、`ballot_guard`。

## 构建

```powershell
# 默认构建 project
idf build

# 其他工程
idf -Project ble_demo build
idf -Project factory build

# 或通过 build.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Project project
```

维护操作：

```powershell
idf reconfigure
idf -Project project fullclean
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Action clean
```

传统方式（须先 `export.bat`，且手动设置 `IDF_EXTRA_ACTIONS_PATH=scripts/idf_py_actions` 才能用 `release`）：

```cmd
idf.py -C project build
```

## 版本号（`PROJECT_VER`）

版本在 **`esp_app_desc.version`**、**NVS `appver`**、**release 产物文件名** 之间保持同源（见 `common/cmake/git_project_version.cmake`）。

| 场景 | `PROJECT_VER` 来源 |
|------|-------------------|
| `idf build` | 当前 commit 上的 **git semver tag**（`v1.0.4` / `1.0.4`）；否则 **最近祖先 tag**；再否则 CMakeLists 默认值 |
| `idf release <ver>` | 命令行指定；若 **低于** 仓库最高 semver tag，则**抬升到 tag 版本**并覆盖 `firmware/<tag>/` 同名产物 |

示例：

```powershell
git tag -a v1.0.4 -m "release 1.0.4"
git push origin v1.0.4          # tag 须单独推送
idf build                       # 在 tag 所在 commit 上 → PROJECT_VER = 1.0.4

idf -Project project release 1.0.5   # 高于 tag → 正常发布 1.0.5
idf -Project project release 1.0.2   # 低于 tag → 实际为 1.0.4，覆盖旧产物
```

设备启动后 `nvs_init()` 会将 NVS 中的 `appver` / `fver` 与固件内 `PROJECT_VER` 同步。

## 发布版本（release）

```powershell
# 单工程
idf -Project project release 1.0.4

# 多工程同一版本（逐个追加到 firmware/1.0.4/）
idf -Project project release 1.0.4
idf -Project ble_demo release 1.0.4

# 一次 release 全部工程
idf -Project project release-all 1.0.4
powershell -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Version 1.0.4 -AllProjects
```

产物目录：`firmware/<版本>/`，含 `{product}_{版本}_{编译日期}.bin` / `.hex`、`manifest.json`、`README.txt`。详见 [`firmware/README.md`](firmware/README.md)。

可选参数：`--flash-bundle`（bootloader / 分区表 / flash 脚本）、`--debug`（elf / map）。

**release 编译失败（`Ninja ()`、CMake 0.0s）**：多为 `project/build` 缓存损坏，在 cmd 中执行：

```bat
clean_build.cmd
idf -Project project release 1.0.4
```

## 烧录固件（双 OTA 槽位）

自定义分区表：`flash_partition/partitions_16m_n16r8.csv` — **`app_a` / `ota_0` = 槽位 A**，**`app_b` / `ota_1` = 槽位 B**。详见 `flash_partition/partitions_16m_n16r8.md` 与 `doc/partition_switch_development_plan.md`。

### 默认 `flash` 写入槽位 A

| 命令 | 写入位置 |
|------|----------|
| `idf -Project project -p PORT flash` | 槽位 A（`project.bin` → `app_a`） |
| `idf -Project factory -p PORT flash` | 仍是槽位 A（覆盖同一槽） |

### 烧录槽位 A（量产）

1. `idf build`（或 `idf -Project project build`）
2. `idf -Project project -p PORT flash`（可加 `monitor`）

### 烧录槽位 B（工厂镜像，不改动 A）

1. `idf -Project factory build`
2. `powershell -ExecutionPolicy Bypass -File .\factory\flash_app_b.example.ps1 -Port PORT`

### 整片烧录（A + B）

构建好 `project` 与 `factory` 后：

```powershell
powershell -ExecutionPolicy Bypass -File .\factory\flash_dual_slot.example.ps1 -Port PORT
```

新芯片建议先 `idf -Project project -p PORT flash`，再写槽位 B。更多见 `factory/README.txt`。

### 运行时切换启动槽

串口命令 **`boot_a`**、**`boot_b`**、**`boot_q`**（`common/src/cmd.c`，基于 `esp_ota_set_boot_partition()`）。

## SoftAP 本地 OTA

量产工程 `project/` 已启用 **`CONFIG_WEB_CTRL_OTA`**（设计见 [`doc/ota_development_plan.md`](doc/ota_development_plan.md)）。连接设备 SoftAP 后：

1. 浏览器 **`http://192.168.4.1/ota`** — 上传 `project_*.bin`，点 **Apply & Reboot**
2. API：`GET /api/ota/status`、`POST /api/ota/upload`、`POST /api/ota/apply`

**规则：** 新固件版本须 **严格高于** 当前运行版本；镜像写入对侧 OTA 槽，Bootloader rollback 在新固件确认后 `mark_app_valid`。测试时可用 `idf -Project project release x.y.z` 生成 OTA 包。

## 校验环境

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1 -RunInstall -RequireIdfInPath -RunBuildTest
```

## 仓库说明

- `Espressif/` 在 Git 中忽略，各开发者本地通过 `setup_env.ps1` 安装。
- `cbb/` 子模块：`git@github.com:indexxue/cbb.git`，克隆后须 `git submodule update --init --recursive`。
- `firmware/` 提交 `manifest.json` / `README.txt`；`.bin` / `.hex` 等大文件见 `firmware/.gitignore`。
- Git **tag 不会随 `git push` 自动上传**，须 `git push origin v1.0.4`；GitHub **Releases** 页需在网页或 `gh release create` 单独发布。
- 自定义 ESP-IDF 路径或非 v5.5.4 版本时，请同步修改 `scripts/setup_env.ps1` / `scripts/IdfEnv.ps1`。
- 编码与分层规范见 `doc/embedded_coding_standard.md`；Cursor skill：`.cursor/skills/esp32-s3-coding-standard/`。
