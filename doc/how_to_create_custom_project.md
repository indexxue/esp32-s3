# ESP32-S3 自建工程代码落地指南与计划

## 1. 目标

本文档用于指导用户从 0 创建自己的 ESP32-S3 工程，并明确：

- 代码层面必须完成哪些内容
- 目录和模块如何组织
- 按阶段如何推进并验收

---

## 2. 一个可运行工程的最小代码要求

如果只追求“能编译、能下载、能运行”，至少需要以下内容：

1. 入口函数
- 在 `project/main` 提供 `app_main(void)`。
- 入口里至少要有串口输出，用于确认程序已运行。

2. 组件构建脚本
- 工程根、`project`、`project/main` 需要有正确的 `CMakeLists.txt`。
- 在 `idf_component_register(...)` 中声明源文件与头文件目录。

3. 基础头文件与 API 调用
- 包含 `freertos/FreeRTOS.h`、`freertos/task.h`、`esp_system.h` 等基础头。
- 至少调用一个可观察行为（例如 `printf`、LED 翻转、周期日志）。

4. 可验证的运行逻辑
- 使用循环或任务保持程序持续运行。
- 增加 `vTaskDelay(...)`，避免空转占满 CPU。

5. 可构建可烧录
- 能通过 `idf.py set-target esp32s3`、`idf.py build`、`idf.py flash monitor` 完整流程。

---

## 3. 推荐工程结构（结合当前仓库）

建议保持三层结构，便于维护和扩展：

```text
ESP32-S3/
├─ driver/            # MCU 外设驱动层（GPIO/UART/I2C/SPI/ADC/PWM/Timer）
│  ├─ inc/
│  ├─ src/
│  └─ CMakeLists.txt
├─ common/            # 通用功能组件层（button/led_scene/log/error/config）
│  ├─ inc/
│  ├─ src/
│  ├─ components/
│  └─ CMakeLists.txt
├─ project/           # 业务入口层（app_main + 应用逻辑）
│  ├─ main/
│  └─ CMakeLists.txt
└─ doc/
```

依赖关系必须约束为：

- 允许：`project -> common -> driver`
- 禁止：`driver -> common`
- 尽量避免 `project` 直接依赖过多 `driver` 细节

---

## 4. 自建工程时“代码必须做什么”

## 4.1 启动与入口代码

- 在 `project/main/*.c` 中实现 `app_main()`。
- 完成系统初始化调用顺序，例如：
  - `driver_init()`
  - `common_init()`
  - `app_init()`

建议把初始化失败路径统一处理，避免静默失败。

## 4.2 Driver 层代码

每个外设模块建议具备：

- `xxx_init(...)`
- `xxx_deinit(...)`
- `xxx_read(...) / xxx_write(...)` 或 `xxx_start(...) / xxx_stop(...)`
- 返回统一错误码（建议自定义错误体系，不直接暴露底层细节）

第一批优先实现：

- `gpio`（输入输出 + 中断）
- `uart`（日志通道 + 基础收发）
- `timer`（单次 + 周期）

## 4.3 Common 层代码

Common 不直接处理寄存器，只做“语义封装”：

- `button`：消抖、短按、长按、连按事件
- `led_scene`：常亮、闪烁、呼吸、场景切换
- `log`：统一日志格式
- `error`：统一错误码与错误文本
- `config`：统一参数入口

## 4.4 应用层代码（project）

- 通过 common 接口完成功能，不在 `app_main` 堆叠底层细节。
- 将业务拆成任务或状态机，避免单函数过大。
- 提供最小演示流程：上电 -> 初始化 -> 事件触发 -> 行为反馈 -> 日志输出。

## 4.5 工程质量代码要求

- 编译无新增告警（建议 `-Wall -Wextra`）。
- 所有对外函数都要有返回值和失败处理。
- 日志至少包含模块名、错误码、关键参数。
- 每个模块至少一个最小示例或自测函数。

---

## 5. 分阶段实施计划（4 周模板）

## Phase 0（0.5 周）：工程骨架搭建

任务：

- 创建并打通 `driver/common/project` 三层目录
- 完成三层 `CMakeLists.txt` 注册
- 写最小 `app_main` 并输出启动日志

验收：

- 可稳定 `build + flash + monitor`
- 启动日志连续输出正常

## Phase 1（1 周）：最小可调试系统

任务：

- 实现 `driver/gpio`、`driver/uart`、`driver/timer`
- 用 timer + gpio 跑通 LED 闪烁
- 用 gpio 中断输出按键日志

验收：

- 连续运行 2 小时无异常重启
- 日志可定位中断和定时事件

## Phase 2（1~1.5 周）：通信总线能力

任务：

- 实现 `driver/i2c`、`driver/spi`
- 增加超时、重试、错误日志
- 完成一组传感器或外设读写验证

验收：

- I2C 读设备 ID 成功率 > 99%
- SPI 压测无异常返回

## Phase 3（1 周）：采集与控制能力

任务：

- 实现 `driver/adc`、`driver/pwm`
- 打通 ADC 采样与 PWM 控制闭环演示

验收：

- ADC 数据波动在预期范围
- PWM 控制效果稳定可见

## Phase 4（1 周）：Common 组件可复用

任务：

- 完成 `button` 与 `led_scene` 组件
- 业务层改用 common 调用
- 补齐文档和示例

验收：

- `project/main` 不直接调用底层外设细节也能跑通演示
- 组件接口可被其他工程复用

---

## 6. 推荐最小 app_main 示例（结构示意）

```c
void app_main(void)
{
    if (driver_init() != APP_OK) {
        // log error
        return;
    }

    if (common_init() != APP_OK) {
        // log error
        return;
    }

    while (1) {
        // app loop / state machine
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```



