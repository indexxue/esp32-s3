# ESP32-S3 开发规范与工程说明

**适用范围**：本仓库在 **ESP32-S3** 上使用 **ESP-IDF v5.5.4** 的固件开发。  
**应用工程目录**：`project/`（CMake 根目录）。  
**烧录、整片擦除、串口监视**：见 [`compile_flash_erase_monitor.md`](compile_flash_erase_monitor.md)。

本文档结构：**开发规范** → **编码规范** → **文件与目录创建规范** → **编译与工程说明**。

---

# 第一部分：开发规范

## 1.1 分层与依赖方向

本仓库以 ESP-IDF **组件**为边界组织代码，约定依赖关系如下（箭头表示「允许依赖」）：

- **`project/main`**：应用入口与业务编排（`app_main`、板级初始化调用顺序等）。  
  允许依赖：`common`、`bsp_driver`、`cbb` 以及 ESP-IDF 官方组件。

- **`common`**：与具体电路板无关的通用能力（如按键语义、灯效、统一日志封装等）。  
  允许依赖：`bsp_driver`（及 IDF 组件）；**不得**被 `bsp_driver` 依赖。

- **`bsp_driver`**：板级外设封装（GPIO、UART、I2C、Timer 等与 ESP 外设驱动相关的薄封装）。  
  不依赖 `common`；尽量不依赖 `cbb`，避免底层反向依赖器件层。

- **`cbb`**（circuit / chip building blocks）：具体器件或功能块驱动（如显示屏控制器、传感器芯片等）。  
  可依赖 `bsp_driver` 提供的总线/GPIO 抽象或 IDF 驱动；**不得**依赖 `common`。

**禁止**：`bsp_driver` → `common`，`cbb` → `common`；尽量避免环状组件依赖。新增模块前先确定所属层级，再决定放在哪个目录。

## 1.2 初始化与错误处理

1. **启动顺序**：建议显式分层初始化（例如板级硬件 → 通用组件 → 应用逻辑），顺序写在 `app_main` 或独立 `board_init` / `app_init` 中并加注释。  
2. **失败路径**：初始化失败应记录日志并进入安全状态（重试、停机或复位），**禁止**静默跳过导致后续空指针或未定义行为。  
3. **返回值**：对 ESP-IDF API 与团队自研 API 的 `esp_err_t` 或统一错误码进行检查；见下文「编码规范」中的错误处理约定。

## 1.3 FreeRTOS 与并发

1. **耗时工作**放在任务中；**ISR** 内只做最短工作（清标志、通知任务/队列），不打印大段日志、不等待互斥量。  
2. **栈深度**：创建任务时按最坏调用链预留；修改栈单位或宏时与 `xTaskCreate` / `xTaskCreateStatic` 的文档一致。  
3. **同步**：跨任务共享数据用互斥量、队列、事件组等，并控制持锁时间；注意优先级反转与死锁。

## 1.4 日志、配置与版本管理

1. 调试阶段使用 **`ESP_LOGx`**，为每个模块设稳定 **TAG**，便于过滤。  
2. 与编译相关的全局选项以 **`sdkconfig` / `menuconfig`** 为准；提交前确认未误提交本机私密路径或无关差异。  
3. **提交前**：应能通过本仓库推荐方式 **零新增告警** 完成编译（见第四部分）；提交说明用完整句子写清动机与影响范围。  
4. 涉及引脚、时钟、分区表变更时，在提交说明或独立 `doc` 中注明，便于他人复现。

## 1.5 与专项文档的关系

- 工程从零搭建与阶段验收：[`how_to_create_custom_project.md`](how_to_create_custom_project.md)  
- 分层演进与计划：[`common_module_development_framework.md`](common_module_development_framework.md)

---

# 第二部分：编码规范

## 2.1 总体原则

1. **可读性优于炫技**：复杂条件拆分，层次清晰。  
2. **单一职责**：每个 `.c` 文件围绕一个外设、器件或清晰子功能；函数长度建议控制在约 50～80 行以内，过长则拆分。  
3. **防御性编程**：校验指针、长度、数值范围；通信与传感器数据需校验范围与状态机合法性。  
4. **避免魔法数字**：有业务含义的常量用 `enum`、`static const` 或命名宏，并注明单位（ms、Hz、raw 等）。  
5. **可移植与宽度**：协议与外设位宽优先使用 `stdint.h`（`uint8_t`、`uint32_t` 等）；网络与持久化结构体与内存布局需在注释或文档中说明对齐与字节序。

## 2.2 命名约定

| 类别 | 规则 | 示例 |
|------|------|------|
| 函数 | 小写 + 下划线；模块前缀与 ESP-IDF 风格协调 | `board_init`、`i2c_bus_write` |
| 局部变量 | 小写 + 下划线 | `retry_count`、`line_level` |
| 文件作用域静态 | 前缀 `s_`（可选，与团队一致即可） | `s_is_inited` |
| 全局变量 | 尽量少用；若需要，前缀 `g_` | `g_log_level` |
| 指针 | 可选用前缀 `p_` | `p_buf` |
| 类型 | `typedef struct` 用 `_t` 后缀 | `imu_sample_t` |
| `enum` | 标签与 typedef 清晰；取值加前缀避免污染 | `DRV_OK`、`DRV_ERR_TIMEOUT` |
| 宏常量 | 全大写 + 下划线 | `MAX_RETRY` |

对外 API 命名建议：**模块动词** 或 **模块动词对象**（如 `timer_start_periodic`）。布尔语义可用 `is_` / `has_` 前缀。

## 2.3 错误与返回值

1. 调用 ESP-IDF API 后应检查 **`esp_err_t`**；业务层可封装为统一错误码，但**禁止**忽略错误返回值。  
2. **`ESP_ERROR_CHECK`**：仅适用于「失败即不应继续」的调试路径；量产路径应返回错误、记录日志或降级处理。  
3. 自定义模块建议约定：`ESP_OK` / `ESP_ERR_*` 或项目内 `typedef enum` 错误域，文档中说明各错误语义。

## 2.4 日志

1. 使用 **`ESP_LOGI/W/E/D`**，每个 `.c` 文件或模块使用固定 **`TAG`**（字符串字面量）。  
2. 热路径避免高频 `ESP_LOG`；中断内避免大量日志。  
3. 日志级别与控制台输出以 `menuconfig` 为准。

## 2.5 头文件与包含顺序

1. **对应 `.c` 的第一行**包含本模块 `.h`，保证头文件自完备。  
2. 其次为 **ESP-IDF / FreeRTOS** 头文件，再为其他工程组件头文件，最后 **标准库**。  
3. 头文件保护：使用 `#pragma once` 或 `#ifndef` 守卫，团队内统一一种。  
4. **最小可见性**：对外 API 放入 `.h`；仅本编译单元使用的 `static` 函数与内部宏放在 `.c`。  
5. 避免头文件中的非 `inline` 对象定义与大段内联逻辑堆积。

## 2.6 宏与安全用法

1. 带参宏对参数加括号；多语句宏使用 `do { ... } while (0)`。  
2. 与寄存器或硬件位相关的宏在注释中标明数据手册章节或符号名（若封装在底层）。  
3. 避免用复杂宏替代函数导致无法单步调试。

## 2.7 中断、缓存与 DMA（ESP32-S3）

1. **ISR 尽量短**；需要与任务协作时使用队列、任务通知、`xSemaphoreGiveFromISR` 等 **FromISR** 接口，并视情况在退出前 `portYIELD_FROM_ISR()`。  
2. 在 **ISR 或禁用 cache 场景** 下访问的代码/常量，遵循 IDF 文档使用 **`IRAM_ATTR`** 等属性，避免执行路径在 flash 上导致异常。  
3. **DMA 缓冲区**：按外设要求对齐；若涉及 **cache**，使用 IDF 提供的 **DMA 安全内存 API** 或文档推荐的 `MALLOC_CAP_DMA` 等，并在所有权转移处注意同步。  
4. **双核**：若使用双核，注意跨核访问共享数据的同步。

## 2.8 内存与资源

1. 长期运行固件中，**动态分配**尽量集中在初始化阶段；运行中频繁 `malloc/free` 需谨慎并处理失败路径。  
2. 任务、队列、定时器等创建失败必须处理，避免后续空句柄。  
3. `deinit` 路径应释放句柄、删除任务、注销中断回调，与 `init` 对称。

## 2.9 断言与 `configASSERT`

开发阶段可依赖 **`assert` / `configASSERT`** 尽早暴露非法状态；发布策略由团队统一（保留、关闭或改为记录后复位），**禁止**依赖「断言被优化掉」掩盖逻辑错误。

## 2.10 编译告警

新增代码应在当前工程默认选项下 **不引入新告警**；若需对接第三方代码产生告警，应局部、有注释地处理并评审。

---

# 第三部分：文件与目录创建规范

## 3.1 应把新代码放在哪里

| 内容 | 放置位置 | 说明 |
|------|----------|------|
| 板级 GPIO / UART / I2C / Timer 等 ESP 外设封装 | `bsp_driver/inc`、`bsp_driver/src` | 更新同目录下 `CMakeLists.txt` 的 `SRCS` |
| 具体芯片、显示屏、电机驱动 IC 等 | `cbb/` | 当前仓库该目录头文件多与 `.c` 同级；保持组件内风格一致 |
| 按键消抖、灯效、与硬件无关的通用逻辑 | `common/inc`、`common/src` | 通过 `REQUIRES` 声明对 `bsp_driver` 等的依赖 |
| 产品业务、状态机、主流程 | `project/main/` | 保持 `app_main` 清晰，复杂逻辑可拆多文件 |

若新目录需被工程识别，必须已列入 `project/CMakeLists.txt` 的 **`EXTRA_COMPONENT_DIRS`**，或采用 IDF 支持的其它组件注册方式。

## 3.2 新建或扩展 ESP-IDF 组件时的步骤

1. 在目标目录创建或修改 **`CMakeLists.txt`**，使用 **`idf_component_register`**：  
   - **`SRCS`**：列出所有 `.c` / `.cpp` 源文件；  
   - **`INCLUDE_DIRS`**：对外头文件目录（如 `inc` 或 `.`）；  
   - **`REQUIRES` / `PRIV_REQUIRES`**：声明本组件直接使用的其它组件（如 `esp_driver_gpio`、`freertos`、`bsp_driver`），避免链接期或未声明依赖导致的头文件/符号错误。  
2. **头文件**：对外 API 放入 `INCLUDE_DIRS` 指定目录；命名与源文件一致，如 `timer.h` ↔ `timer.c`。  
3. **Kconfig**：若组件需在 `menuconfig` 中可配置选项，再增加 `Kconfig` 与 `CMakeLists.txt` 中的注册；无配置需求可不增加。  
4. 完成后在仓库根执行 **`build.ps1`** 或 `idf.py -C project build` 做全量验证。

## 3.3 文件命名

1. 源/头文件使用 **小写字母 + 下划线**，与模块名一致，例如 `usb_serial_jtag.c`、`usb_serial_jtag.h`。  
2. 避免单字母文件名或与标准库易混淆的名称。  
3. 同一模块 **`.c` / `.h` 成对出现**；不在公共头文件中堆积无关模块的声明。

## 3.4 禁止与不推荐

1. **禁止**在 `bsp_driver` 中引用 `common` 层头文件或符号。  
2. **不推荐**在 `project/main` 中直接复制粘贴大段寄存器操作；应下沉到 `bsp_driver` 或 `cbb` 并封装接口。  
3. **禁止**将 `project/build/`、本地 `sdkconfig` 中的机器相关绝对路径等提交策略不允许的内容误提交（以团队 Git 规范为准）。

---

# 第四部分：编译与工程说明

## 4.1 环境与首次准备

1. **操作系统**：以 Windows 10/11 为主；工具链位于本地 `Espressif/`（不入库）。  
2. **首次安装**（根目录）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
   ```

3. **加载 ESP-IDF 环境**：`export.bat`、`.\Espressif\idf_cmd_init.bat` 或 VS Code / Cursor 已配置终端（见 `.vscode/settings.json`）。  
4. **自检**（可选）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
   ```

## 4.2 工程与组件布局（摘要）

- CMake 根：`project/CMakeLists.txt`。  
- **`EXTRA_COMPONENT_DIRS`**：`../common`、`../bsp_driver`、`../cbb`。  
- 入口：`project/main/`。  
- 构建输出：`project/build/`（勿提交）。

## 4.3 目标芯片

```cmd
cd project
idf.py set-target esp32s3
```

## 4.4 编译命令

根目录推荐：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

动作：`build`（默认）、`reconfigure`、`clean`、`fullclean`。  
直接使用：`idf.py -C project build`（需在已 `export` 的环境中）。

## 4.5 配置

```cmd
idf.py menuconfig
```

结果写入 `sdkconfig` 等；与分区表、控制台、日志、频率、PSRAM 等相关，改后需重编。

## 4.6 诊断与常见问题

| 目的 | 命令示例 |
|------|----------|
| 镜像大小 | `idf.py size` / `size-components` / `size-files` |

常见问题：`IDF_PATH` 未设置、`set-target` 与 `sdkconfig` 不一致、组件未列入 `EXTRA_COMPONENT_DIRS`、`idf_component_register` 缺少 `REQUIRES`、头文件目录未加入 `INCLUDE_DIRS`。详细烧录与监视见 [`compile_flash_erase_monitor.md`](compile_flash_erase_monitor.md)。

---

# 附录：文档与仓库索引

| 主题 | 文档 |
|------|------|
| 开发 / 编码 / 文件 / 编译（本文） | `embedded_coding_standard.md` |
| 烧录、擦除、监视 | `compile_flash_erase_monitor.md` |
| 环境与安装 | 根目录 `README.md` |
| 自建工程与最小可运行要求 | `how_to_create_custom_project.md` |

---

# 修订记录

| 日期 | 说明 |
|------|------|
| 2026-05-08 | 初版：STM32 / CubeMX / FreeRTOS 编码与中断优先级等。 |
| 2026-05-10 | 改为 ESP32-S3 + ESP-IDF；增加开发规范、编码规范、文件与目录创建规范；保留编译与工程说明；布局对齐 `bsp_driver` / `cbb` / `common` / `project`。 |
