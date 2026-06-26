# ESP32-S3 项目

ESP32-S3（N16R8）固件 monorepo。本地 ESP-IDF 工具链（`Espressif/`）与构建产物默认不提交。

| 工程 | 目录 | 说明 |
|------|------|------|
| `project` | `project/` | 量产主应用（SoftAP、Web 维护、OTA） |
| `factory` | `factory/` | 工厂/维护镜像（槽位 B） |
| `ble_demo` | `ble_demo/` | BLE GATT 示例 |
| `ballot_guard` | `ballot_guard/` | 选票监管应用 |

共享代码：`common/`、`bsp_driver/`、`cbb/`（**Git 子模块**）、`components/`（如 `web_ctrl`）。

## 首次配置

**要求：** Windows 10/11、Git for Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
git submodule update --init --recursive
```

脚本会安装 ESP-IDF **v5.5.4** 到 `.\Espressif\frameworks\esp-idf-v5.5.4`。校验环境：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
```

## 构建与烧录

根目录提供 **`idf.cmd` / `idf.ps1`**，自动配置工具链并加载 `release` 扩展。根目录**没有** `CMakeLists.txt`，勿直接运行裸 `idf.py`；须用 `idf` 或 `idf.py -C <工程>`。

| 终端 | 示例 |
|------|------|
| cmd | `idf build`、`idf -p COM13 flash monitor` |
| PowerShell | `.\idf.ps1 build`、`.\idf.ps1 -p COM13 flash monitor` |

`-Project` 可选：`project`（默认）、`ble_demo`、`factory`、`ballot_guard`。

```powershell
# 构建
idf build
idf -Project ble_demo build

# 烧录 + 监视（将 COM13 换成实际端口）
idf -p COM13 flash monitor

# 维护
idf reconfigure
idf -Project project fullclean
```

等价脚本：`scripts/build.ps1`、`scripts/release.ps1`。传统方式须先 `export.bat`，且 `idf.py -C project build`。

**串口监视：** 推荐 cmd 跑 `idf.cmd -p COM13 flash monitor`；日志静默时按板子 RESET。若报 `COMx is busy`，先在同终端按 **Ctrl+]** 退出 monitor，或关闭 Cursor 串口监视后再试。

## 版本与发布

`PROJECT_VER` 在固件描述、NVS `appver`、release 文件名间同源（`common/cmake/git_project_version.cmake`）。

- **`idf build`**：优先当前 commit 的 git semver tag，否则最近祖先 tag
- **`idf -Project project release <ver>`**：命令行指定；低于最高 tag 时抬升到 tag 并覆盖旧产物

```powershell
git tag -a v1.0.4 -m "release 1.0.4"
git push origin v1.0.4          # tag 须单独推送
idf -Project project release 1.0.4
idf -Project project release-all 1.0.4   # 全部工程
```

产物：`firmware/<版本>/`（`manifest.json`、`README.txt` 等）。详见 [`firmware/README.md`](firmware/README.md)。release 编译异常（`Ninja ()`）时先跑 `clean_build.cmd`。

## 双 OTA 槽位烧录

分区表：`flash_partition/partitions_16m_n16r8.csv` — **`app_a`/`ota_0` = 槽位 A**，**`app_b`/`ota_1` = 槽位 B**。详见 `flash_partition/partitions_16m_n16r8.md`。

| 场景 | 步骤 |
|------|------|
| 槽位 A（量产） | `idf build` → `idf -p PORT flash` |
| 槽位 B（工厂，不动 A） | `idf -Project factory build` → `.\factory\flash_app_b.example.ps1 -Port PORT` |
| A + B 整片 | 构建 project + factory → `.\factory\flash_dual_slot.example.ps1 -Port PORT` |

运行时切换：串口命令 `boot_a` / `boot_b` / `boot_q`（`common/src/cmd.c`）。

## SoftAP OTA

连接设备 SoftAP 后访问 **`http://192.168.4.1/ota`** 上传 `project_*.bin`；或调用 `/api/ota/*`。新固件版本须**严格高于**当前版本，写入对侧 OTA 槽。设计见 [`doc/ota_development_plan.md`](doc/ota_development_plan.md)。

## 参考

- 编码规范：`doc/embedded_coding_standard.md`
- `cbb/` 子模块：`git@github.com:indexxue/cbb.git`
- 自定义 IDF 路径/版本：修改 `scripts/setup_env.ps1`、`scripts/IdfEnv.ps1`
