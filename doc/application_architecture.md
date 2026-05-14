# 应用架构与后续功能开发约定

本文描述本工程在 ESP-IDF / FreeRTOS 下的**分层思路**与**扩展新功能时的推荐做法**，与 `project/main/main.c`、`project/main/start.c` 的实现一致。

## 1. 目标

- **入口极薄**：`app_main` 只做启动与失败日志。
- **平台壳层与业务解耦**：上电顺序、日志初始化、板级外设、按键、灯效集中在壳层；存储、显示内容、传感器业务循环放在独立任务中。
- **可演进为事件驱动**：模块间优先通过队列 / 事件总线通信，避免在 ISR 或按键回调里做重活。

## 2. 文件职责

| 位置 | 职责 |
|------|------|
| `main.c` | `app_main`；`application_start_modules_task` 及 **LCD / SD / IMU 等重逻辑** 与其 FreeRTOS 任务体。 |
| `start.c` | `app_start` / `app_entry`：**log_init → BoardInit → 按键扫描任务 → 灯效**；`app_run` 中调用 `application_start_modules_task()` 后进入 `app_idle_default()`（不返回）。 |
| `start.h` | 生命周期 `app_lifecycle_t`、`app_start`、`app_entry`，以及 **`application_start_modules_task()`**（实现在 `main.c`，供 `start.c` 调用）；可被测试或替代启动路径复用。 |

## 3. 任务与优先级（当前约定）

- **按键扫描**（`start.c`）：任务名 `btn_scan`，优先级 **5**，周期由 `FLEX_BTN_SCAN_FREQ_HZ` 决定。
- **业务模块**（`main.c`）：任务名 `app_mod`，优先级 **4**，栈 `APP_MODULES_TASK_STACK_WORDS`（默认 4096）。略低于按键，减轻长 SPI / SD 操作对短周期人机逻辑的挤压。

新增长期任务时：**先写清与外设总线的关系**，再选优先级与栈大小，并在本文或代码注释中补一行说明。

## 4. 外设与“单一所有者”

LCD（SPI）、SDMMC、部分 I2C 设备易在**多任务同时访问**时出错。约定：

- **默认由业务任务**（`app_mod`）独占刷屏与 SD 烟测；若新增第二个会刷 LCD 的任务，应引入**显示服务任务**或 **互斥锁**，其它模块只投递“刷新请求”，不直接抢总线。
- 按键回调中只做**短逻辑**（当前为日志 + 灯效场景）；若后续加入文件写入、网络等，应改为 **Post 到队列**，由专用任务处理。

## 5. 新功能开发检查清单

1. **放哪一层**：属于“上电/人机壳层”还是“业务数据与 UI”？前者改 `start.c` 或板级 `board.*`，后者优先放进 `main.c` 的任务或拆出的 `app_*.c` 并由 `application_start_modules_task` 或队列触发。
2. **是否新建任务**：阻塞超过数毫秒的逻辑不要放在按键回调或高优先级不可抢占上下文；需要周期执行则用独立任务 + `vTaskDelay`，或 `esp_timer`。
3. **通信方式**：跨模块状态用 **FreeRTOS 队列** 或 **esp_event**；避免大量 `extern` 交叉调用深层实现。
4. **栈与水线**：新任务用 `uxTaskGetStackHighWaterMark`（调试期）核对栈余量；浮点、大局部缓冲、深层调用链要留余量。
5. **看门狗**：长时间 CPU 密集或关中断区间需避免饿死 IDLE；必要时拆分步骤并 `taskYIELD()` / 降优先级。

## 6. 生命周期语义

- `app_start`：先执行 `init`，再执行 `run`；`run` 为 `NULL` 时进入默认 idle。
- 默认工程中 **`run` 不返回**：先创建业务任务，再 `app_idle_default()`，从而 `app_entry` 长期阻塞，`app_main` 在成功路径上不会在启动后立即退出任务上下文。

若将来需要 **`run` 立即返回**（例如仅创建任务后交给 IDF 默认 `app_main` 退出策略），需同步调整 `app_main` 末尾行为，并确认组件对 `app_main` 结束的预期。

## 7. 变更记录（架构）

- 将 QMI / LCD 基准 / SD 烟测与主循环从 `start.c` 迁至 `main.c` 的 `app_mod` 任务；`start.c` 保留平台壳层与任务创建编排。
