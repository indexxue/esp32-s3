# 固件发布目录（`firmware/`）



本目录存放 **`idf.py release <版本号>`** 生成的量产发布包。每个版本一个目录 **`firmware/<x.y.z>/`**，同一版本下可并存多个工程产物。



支持的 ESP-IDF 工程：`project`、`ble_demo`、`factory`、`ballot_guard`。release 扩展位于 `scripts/idf_py_actions/release_ext.py`，通过仓库根目录 **`idf.cmd`** / **`idf.ps1`** 或 **`scripts/release.ps1`** 自动加载。



## 为什么不是一堆 ESP-IDF 原始文件名？



`idf.py build` 产物（`project.bin`、`bootloader.bin`、`project.map` 等）面向**编译与烧录工具**，混在一起时：



- 拷到 U 盘或发给现场时**看不出产品、版本、用途**

- OTA 只需 **app 镜像**，却把 elf/map/flash_args 全打包，**体积大且易误用**



因此 release 采用**统一命名**，并按场景**默认只打 OTA 必需文件**。



**版本号**（与 `esp_app_desc.version`、NVS `appver` 同源）：

| 场景 | `PROJECT_VER` 来源 |
|------|-------------------|
| 普通 `idf build` | 当前 commit 上的 **git semver tag**（精确匹配）；否则 **最近祖先 tag**（`git describe --tags --abbrev=0`）；再否则 CMakeLists 默认值 |
| `idf release <ver>` | 命令行 `-DPROJECT_VER=<ver>`；若 `<ver>` **低于** 仓库最高 semver tag，则**抬升到 tag 版本** |

Tag 格式：`v1.0.3` 或 `1.0.3`（三段 semver）。打 tag 后 build 会自动带上该版本：

```powershell
git tag -a v1.0.3 -m "release 1.0.3"
idf -Project project build          # PROJECT_VER = 1.0.3（在 tag 所在 commit 上）

idf -Project project release 1.0.2  # 若已有 v1.0.3，实际 release 为 1.0.3
```



---



## 命名规范（强制）

```text
{product}_{version}_{build_date}.{ext}
```

| 字段 | 说明 | 示例 |
|------|------|------|
| `product` | CMake `project(...)` 名 | `project` / `ble_demo` |
| `version` | 三段 semver | `1.0.3` |
| `build_date` | 编译日期 UTC，`YYYYMMDD` | `20260625` |
| `ext` | `bin` / `hex` 等 | `bin` |

**示例：** `project_1.0.3_20260625.bin`、`project_1.0.3_20260625.hex`、`ble_demo_1.0.3_20260625.bin`

可选 `--flash-bundle` / `--debug` 产物在日期后加 role：`{product}_{version}_{build_date}_{role}.{ext}`



### role 定义



| role | 含义 | 默认 release | 可选 |

|------|------|:------------:|:----:|

| `app` | 应用镜像（`.bin` OTA 上传；`.hex` Intel HEX，偏移见 `flash_*_args`） | ✅ bin + hex | |

| `bootloader` | 2nd-stage bootloader | | `--flash-bundle` |

| `partition` | 分区表 | | `--flash-bundle` |

| `otadata` | OTA 启动元数据初值 | | `--flash-bundle` |

| `flash_full` | 整片烧录 esptool 参数（自 `flash_args` 复制并重命名） | | `--flash-bundle` |

| `flash_app` | 仅 app 槽烧录参数 | | `--flash-bundle` |

| `debug` | 调试符号（`.elf` / `.map` 共用 role，靠扩展名区分） | | `--debug` |



### 目录内固定元数据（不参与 `{product}_v…` pattern）



| 文件 | 说明 |

|------|------|

| `manifest.json` | 版本、各工程 artifacts 清单（schema 2，`products` 字段） |

| `README.txt` | 给人看的简短说明 |



---



## 用法



在 **cmd.exe** 里请用 **`idf.cmd`**（或 `idf`），不要直接运行 `idf.ps1`——Windows 会把 `.ps1` 用编辑器打开而不是执行。

若 release 报 `Ninja ()` / CMake 0.0s 配置失败，说明 `project/build` 处于损坏的半配置状态。在 **cmd** 中清理：

```bat
clean_build.cmd
idf -Project project release 1.0.4
```

（PowerShell 可用 `Remove-Item -Recurse -Force .\project\build`。release 脚本也会自动检测并重置此类损坏缓存。）



在 **PowerShell** 里可以用 `.\idf.ps1`。



```powershell

# 同一版本、多工程 — 逐个追加（不同 -Project，相同版本号）
idf -Project project release 1.0.3
idf -Project ble_demo release 1.0.3

# 一次 release 全部工程到 firmware/1.0.3/
idf -Project project release-all 1.0.3
powershell -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Version 1.0.3 -AllProjects

# 再次 release 同一工程会自动覆盖该工程旧产物，不影响其他工程
idf -Project project release 1.0.3

```



## 典型目录结构



**同一版本、多工程（推荐）：**



```text
firmware/1.0.3/
  project_1.0.3_20260625.bin      ← 现场 /ota 上传（仅 project）
  project_1.0.3_20260625.hex
  ble_demo_1.0.3_20260625.bin
  ble_demo_1.0.3_20260625.hex
  manifest.json
  README.txt
```



**同一工程**再次 release 时会**自动覆盖**该工程的 `{product}_v{version}_*` 文件；**其他工程**产物保留不动。



## OTA（仅 `project` 工程）



上传 **`project_{version}_{build_date}.bin`**（见 `manifest.json` → `products.project.ota.image`）。版本须高于设备 `run_ver`。  
**阶段 2 云端拉包**：将 `manifest.json` 托管到 HTTPS 可访问路径，设备 STA 联网后 `POST /api/ota/pull` 或 `/ota` 页填写 manifest URL。manifest 中 app 镜像须含 **`sha256`**（`idf release` 自动生成）。详见 [`doc/ota_development_plan.md`](../doc/ota_development_plan.md)。



## Git



[`firmware/.gitignore`](.gitignore) 忽略 `*.bin` / `*.hex` / `*.elf` / `*.map`；可提交 `manifest.json` / `README.txt`。

