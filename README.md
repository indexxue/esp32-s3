# ESP32-S3 项目

ESP32-S3（N16R8）固件 monorepo。本地 ESP-IDF 工具链（`Espressif/`）与构建产物默认不提交。

| 工程 | 目录 | 说明 |
|------|------|------|
| `project` | `project/` | 量产主应用（SoftAP、Web 维护、OTA） |
| `factory` | `factory/` | 工厂/维护镜像（槽位 B） |
| `ble_demo` | `ble_demo/` | BLE GATT 示例 |
| `ballot_guard` | `ballot_guard/` | 选票监管应用（SoftAP/STA OTA，见 [`ballot_guard/README.txt`](ballot_guard/README.txt)） |

共享代码：`common/`、`bsp_driver/`、`cbb/`（**Git 子模块**）、`components/`（如 `web_ctrl`）。

## 命令入口（`idf`）

仓库根目录 **`idf.cmd`** / **`idf.ps1`** 为唯一推荐入口：自动加载本仓库 ESP-IDF 5.5.4、设置 `IDF_EXTRA_ACTIONS_PATH`（`release`、`signing-profile` 等扩展子命令），并将参数转发给 `idf.py -C <工程目录>`。

| 终端 | 用法 |
|------|------|
| **cmd**（推荐，Cursor 默认「ESP-IDF 5.5」） | `idf build`、`idf -Project project release 1.0.10` |
| **PowerShell** | 终端选 **「ESP-IDF PowerShell」** 后直接 `idf ...`；否则 **`.\idf.cmd ...`**（不要裸打 `idf`） |

**`-Project <名>`**（可选）：选择工程目录，默认 `project`。可选值：`project`、`ble_demo`、`factory`、`ballot_guard`。例：`idf -Project factory build` 等价于在 `factory/` 下执行 `idf.py build`。

### 一次性环境脚本

与日常 `idf` 子命令无关，仅首次克隆或换机时执行：

| 命令 | 功能 |
|------|------|
| `scripts\setup_env.ps1` | 下载并安装 ESP-IDF **v5.5.4** 到 `.\Espressif\frameworks\esp-idf-v5.5.4` 及配套工具链 |
| `git submodule update --init --recursive` | 拉取 `cbb/` 等子模块 |
| `scripts\verify_env.ps1` | 检查 IDF 路径、Python、工程目录是否就绪（不要求裸 `idf.py` 已在 PATH，用 `idf.cmd` 即可） |

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
git submodule update --init --recursive
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
```

## 构建与烧录（未签名，默认）

日常开发默认**不启用**镜像签名。若曾切到 `signed_ota`，先恢复（见下表 `signing-profile none`）。

| 命令 | 功能 |
|------|------|
| `idf build` | 编译当前 `-Project` 工程（cmake + ninja）；`PROJECT_VER` 取自 git semver tag |
| `idf -Project ble_demo build` | 编译指定工程（示例：BLE 演示） |
| `idf signing-profile none` | 恢复未签名 `sdkconfig.defaults`；**会删除** `sdkconfig` 与 `build/`，之后需重新 `build` |
| `idf -p COM13 flash` | 经 esptool 将固件写入 Flash（`COM13` 换实际端口） |
| `idf -p COM13 monitor` | 打开串口日志监视 |
| `idf -p COM13 flash monitor` | 先烧录再监视；`idf.ps1` 在烧录后等待端口重连再开 monitor |
| `idf reconfigure` | 强制重新运行 CMake（增删源文件或改 `-D` 缓存项后使用） |
| `idf fullclean` | 清空 `build/` 目录（不删 `sdkconfig`） |
| `clean_build.cmd` | 仓库提供的清理脚本，用于修复 release 时 `Ninja ()` 等异常 |

```powershell
idf signing-profile none
idf build
idf -Project ble_demo build
idf -p COM13 flash monitor          # COM13 换成实际端口
idf reconfigure
idf -Project project fullclean
```

release 编译异常（`Ninja ()`）时：`idf -Project project fullclean` 后重试，或运行 `clean_build.cmd`。

**串口：** 日志静默时按板子 RESET。若报 `COMx is busy`，同终端 **Ctrl+]** 退出 monitor，或关闭 Cursor 串口监视。

## 版本与发布

`PROJECT_VER` 在固件描述、NVS `appver`、release 文件名间同源（`common/cmake/git_project_version.cmake`）。

| 命令 | 功能 | `PROJECT_VER` 来源 |
|------|------|-------------------|
| `idf build` | 仅编译，不打包 | 当前 commit 的 git semver tag，否则最近祖先 tag |
| `idf -Project project release <ver>` | 按版本编译**单个**工程并归档到 `firmware/<ver>/` | 命令行 `<ver>`；低于仓库最高 tag 时抬升到 tag |
| `idf -Project project release-all <ver>` | 对 `project`、`ble_demo`、`factory`、`ballot_guard` 依次 release 到同一 `firmware/<ver>/` | 同上 |

**release 常用选项：**

| 选项 | 功能 |
|------|------|
| `--flash-bundle` | 额外打包 bootloader、分区表、otadata 及 esptool `flash_*.txt`（产线首烧） |
| `--debug` | 额外打包 `.elf` / `.map`（勿用于现场 OTA） |
| `--signing-profile signed_ota` | 合并签名 sdkconfig 并产出已签名镜像（见下节） |
| `--signing-key-id dev` | 在 `manifest.json` 记录密钥标识 |

```powershell
git tag -a v1.0.4 -m "release 1.0.4"
git push origin v1.0.4
idf -Project project release 1.0.4              # 未签名 release（默认）
idf -Project project release-all 1.0.4          # 全部工程同一版本
idf -Project project release 1.0.4 --flash-bundle   # 产线首烧：bootloader + 分区表 + otadata
```

产物目录：`firmware/<版本>/`（`manifest.json`、`project_<ver>_<date>.bin` 等）。详见 [`firmware/README.md`](firmware/README.md)。

## OTA 镜像签名（阶段 3）

RSA-3072 / Secure Boot v2 通过 **`idf` 子命令**切换；默认 `sdkconfig.defaults` **不含**签名，与阶段 1/2 兼容。各命令详细说明与副作用见 [`keys/README.md`](keys/README.md)。

| 命令 | 功能 |
|------|------|
| `idf signing-key-gen` | 在 `keys/dev/` 生成 RSA-3072 开发私钥（首次一次） |
| `idf signing-profile signed_ota --build` | 启用 OTA 验签配置并编译（**不**烧 eFuse） |
| `idf signing-profile secure_boot --build` | 启用硬件 Secure Boot v2 配置并编译（eFuse **不可逆**） |
| `idf signing-profile none` | 恢复未签名日常开发 |
| `idf release <ver> --signing-profile signed_ota --signing-key-id dev` | 打已签名 release 包；版本号与选项顺序任意 |

私钥：`keys/dev/secure_boot_signing_key.pem`（不进 Git）。量产见 [`keys/README.md`](keys/README.md)。

| 类型 | 约略大小 | 设备 signed_ota 下 SoftAP OTA |
|------|----------|-------------------------------|
| 未签名 | ~1.34 MB | 拒绝（`ESP_ERR_OTA_VALIDATE_FAILED`） |
| 已签名 | ~1.38 MB | 允许（版本须高于 `run_ver`） |

典型联调（`project`）：

```powershell
idf signing-key-gen
idf signing-profile signed_ota --build
idf -p COM13 flash monitor
idf -Project project release 1.0.10 --signing-profile signed_ota --signing-key-id dev
# 设备 http://192.168.4.1/ota 上传 firmware/1.0.10/project_*.bin → Apply
```

验收与量产首烧：[`doc/secure_boot_production.md`](doc/secure_boot_production.md)；总体规划：[`doc/ota_development_plan.md`](doc/ota_development_plan.md)。

## 双 OTA 槽位烧录

分区表：`flash_partition/partitions_16m_n16r8.csv` — **`app_a`/`ota_0` = 槽 A**，**`app_b`/`ota_1` = 槽 B**。详见 `flash_partition/partitions_16m_n16r8.md`。

| 场景 | 步骤 |
|------|------|
| 槽 A（量产） | `idf build` → `idf -p PORT flash` |
| 槽 B（工厂，不动 A） | `idf -Project factory build` → `.\factory\flash_app_b.example.ps1 -Port PORT` |
| A + B 整片 | `idf -Project project build` + `idf -Project factory build` → `.\factory\flash_dual_slot.example.ps1 -Port PORT` |

`flash_app_b` / `flash_dual_slot` 为示例脚本，内部调用 esptool 写入指定槽位（需接板子，未在 CI 中自动验证）。

运行时切换：串口 `boot_a` / `boot_b` / `boot_q`（`common/src/cmd.c`）。

## SoftAP / STA OTA

连接 SoftAP 后 **`http://192.168.4.1/ota`** 上传 release 镜像，或使用 `/api/ota/*`。

| 项目 | 说明 |
|------|------|
| 选文件 | `firmware/<ver>/manifest.json` → `products.project.ota.image` |
| 版本 | 须**严格高于**设备 `run_ver` |
| 签名 | 设备 **signed_ota** 时须 **signed** `.bin`（见上表） |
| 云端拉包 | STA 联网后 `/ota` 填 manifest URL，或 `POST /api/ota/pull`（SHA256 校验） |

## 环境验证（ESP-IDF 5.5.4）

以下命令已在仓库 ESP-IDF 环境中实测可用（2026-06-26，`.\idf.cmd`）：

| 命令 | 验证方式 | 结果 |
|------|----------|------|
| `verify_env.ps1` | 完整执行 | 通过 |
| `idf --help` | 列出子命令（含 `release`、`signing-profile` 等扩展） | 通过 |
| `idf build` | 启动 ninja 编译（未跑完全部目标） | 通过 |
| `idf -Project factory build` | 路由到 `factory/` 并启动编译 | 通过 |
| `idf -Project ble_demo build --help` | 工程切换 + 帮助 | 通过 |
| `idf build/reconfigure/fullclean/flash --help` | 帮助 | 通过 |
| `idf release / release-all --help` | 帮助 | 通过 |
| `idf signing-key-gen / signing-profile` | 见 [`keys/README.md`](keys/README.md) 验证表 | 通过 |

未接板子时未实测 `flash` / `monitor` 实机烧录；`--help` 已确认子命令可用。完整 `release` / `--build` 耗时较长，日常用 `--help` 或签名命令的无 `--build` 切换即可验证可行性。

## 参考

- 编码规范：`doc/embedded_coding_standard.md`
- OTA 设计：`doc/ota_development_plan.md`
- 签名产线：`doc/secure_boot_production.md`
- 签名密钥与命令详解：`keys/README.md`
- `cbb/` 子模块：`git@github.com:indexxue/cbb.git`
- 自定义 IDF 路径/版本：`scripts/setup_env.ps1`、`scripts/IdfEnv.ps1`
