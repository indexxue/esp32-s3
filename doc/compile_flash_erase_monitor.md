# 编译、烧录、擦除与串口监视操作说明

本文档梳理本仓库（ESP32-S3 + ESP-IDF v5.5.4）下：**编译**、**烧录**、**擦除 Flash**、**串口监视（运行输出检测）** 的常用命令与预期现象。与根目录 `README.md` 中的环境说明一致。

---

## 1. 前置条件

1. 已按仓库说明完成首次环境安装（见根目录 `README.md` 的 **First-time setup**）。
2. 终端中已加载 ESP-IDF 环境，任选其一：
   - **CMD**：`cmd /k ".\Espressif\frameworks\esp-idf-v5.5.4\export.bat"`（路径按你本机仓库根目录调整）
   - **VS Code / Cursor**：使用已配置的 **ESP-IDF 5.5** 终端配置文件，会自动 `cd` 到 `project` 目录（见 `.vscode/settings.json`）

后续命令若未特别说明，均在已加载 IDF 环境的终端中执行。

---

## 2. 编译（Build）

### 2.1 推荐方式（仓库脚本，无需手动 export）

在仓库**根目录** PowerShell 执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

等价于对 `project` 目录执行 `idf.py build`。

### 2.2 脚本可选动作

| 动作 | 命令 | 说明 |
|------|------|------|
| 默认编译 | `.\scripts\build.ps1` 或 `.\scripts\build.ps1 -Action build` | 增量编译 |
| 重新配置 | `.\scripts\build.ps1 -Action reconfigure` | 重新跑 CMake 配置 |
| 清理构建 | `.\scripts\build.ps1 -Action clean` | Ninja clean |
| 深度清理 | `.\scripts\build.ps1 -Action fullclean` | 删除 `project/build` 等，下次全量重编 |

### 2.3 直接使用 idf.py

在任意目录（指定工程路径 `-C project`）：

```cmd
idf.py -C project build
```

若当前目录已是 `project`：

```cmd
idf.py build
```

### 2.4 典型成功输出（摘要）

- 末尾出现 **Project build complete**，并给出生成的 `project.bin` / 合并镜像路径。
- 失败时会有 **FAILED**、具体 `.c` 报错或链接错误，需根据行号回到源码修改。

### 2.5 目标芯片（首次或换芯片时）

若尚未为工程设置目标，需在 `project` 下执行一次：

```cmd
cd project
idf.py set-target esp32s3
```

---

## 3. 烧录（Flash）

### 3.1 前提

- USB 连接开发板，Windows 上确认串口（设备管理器中 **端口 (COM 和 LPT)**），例如 `COM5`。
- 已编译成功（或至少 bootloader/partition/app 镜像已生成）。

### 3.2 指定串口烧录

在 `project` 目录（或 `idf.py -C project`）：

```cmd
idf.py -p COM5 flash
```

将 `COM5` 换成你的实际端口号。

### 3.3 典型成功输出（摘要）

- 日志中出现 **Connecting...**、**Writing at ...** 各分区写入进度。
- 结尾 **Hash of data verified**、**Hard resetting via RTS pin...** 表示烧录完成并复位。

### 3.4 一键：烧录后立即监视串口

```cmd
idf.py -p COM5 flash monitor
```

烧录结束后会进入与 `monitor` 相同的串口监视；**退出方式见下文 §5.2**（推荐 **Ctrl+]**）。

---

## 4. 擦除（Erase Flash）

### 4.1 整片 Flash 擦除（常用）

会清除应用程序、NVS、可能的其他用户数据，**慎用**：

```cmd
idf.py -p COM5 erase-flash
```

成功后一般提示擦除完成；之后需要重新 **flash** 才能运行固件。

### 4.2 与「清理编译」的区别

| 操作 | 作用对象 | 典型命令 |
|------|----------|----------|
| 擦除芯片 Flash | 开发板片上 Flash 内容 | `idf.py erase-flash` |
| 清理本地构建目录 | 本机 `project/build` | `build.ps1 -Action clean` / `fullclean` |

---

## 5. 检测与监视（串口输出 / Monitor）

此处「检测」指：**观察设备运行日志与断言**，通过 UART 或 USB-Serial-JTAG 等已配置的默认控制台。

### 5.1 仅监视（不重烧录）

```cmd
idf.py -p COM5 monitor
```

### 5.2 如何退出监视（关闭串口检测）

串口监视是前台进程，**关掉监视 = 退出 `idf.py monitor` / `flash monitor` 里的 monitor 阶段**，不会自动关电脑或关板子电源。

| 方式 | 说明 |
|------|------|
| **Ctrl+]** | ESP-IDF 自带监视器默认快捷键：先按住 **Ctrl**，再按 **]**（右方括号，美式键盘在 Enter 键上方）。松手后监视应结束并回到命令提示符。 |
| **Ctrl+C** | 多数情况下会直接结束整个 Python 进程（监视随之结束）。若串口偶发占用，可再拔插 USB 或重开终端。 |
| **关终端 / 关标签页** | 等价于杀掉该次监视进程；粗暴但有效。 |

若使用 **VS Code / Cursor 的 ESP-IDF 插件** 打开的监视窗口，一般用插件工具栏的 **停止监视（Stop）** 即可，效果与退出 `idf.py monitor` 相同。

**说明**：上述是「不再看串口日志」。若指固件里**少打或不打日志**（关掉应用侧“检测输出”），需在代码里降低日志级别或去掉 `printf` / `ESP_LOGx`，与 `menuconfig` 里日志配置有关，与退出监视器是两件事。

### 5.3 监视中常见现象

- 复位后打印 **boot** 日志、应用里 `printf` / `ESP_LOGI` 等输出。
- 若乱码：检查波特率是否与 `menuconfig` 中 **Component config → ESP System Settings → Channel for console output** 及对应 UART 波特率一致（常见 115200）。
- 无输出：检查 COM 号、线序、是否被其他软件占用串口。

### 5.4 环境自检（可选）

在仓库根目录验证本机工具链与（可选）试编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
```

更严格检查（耗时更长）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1 -RunInstall -RequireIdfInPath -RunBuildTest
```

---

## 6. 推荐日常流程（简表）

| 步骤 | 命令示例 |
|------|----------|
| 编译 | `.\scripts\build.ps1` 或 `idf.py -C project build` |
| 烧录 | `idf.py -p COMx flash` |
| 看日志 | `idf.py -p COMx monitor` 或 `flash monitor` |
| 恢复空片 / 排障 | `idf.py -p COMx erase-flash` 后再 `flash` |

---

## 7. 参考文件

- 根目录 `README.md`：环境安装、`export.bat`、`build.ps1` 说明  
- `scripts/build.ps1`：`build` / `clean` / `fullclean` / `reconfigure` 与 `idf.py` 的对应关系  
- `doc/how_to_create_custom_project.md`：工程结构与最小可运行要求  

本文档随 ESP-IDF 小版本差异，个别子命令若变更，以官方 `idf.py --help` 为准。
