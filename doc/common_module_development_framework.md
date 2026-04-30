# ESP32-S3 基础框架开发计划（Driver 与 Common 分层版）

## 1. 目标与边界

本计划以“先打通 MCU 基础外设，再建设通用组件”为原则，明确拆分：

- `driver`：只放单片机基础外设驱动（GPIO/UART/I2C/SPI/ADC/PWM/Timer 等）
- `common`：放通用功能组件（`button`、`led_scene`、状态管理、事件封装等）

当前阶段只覆盖基础外设与最小公共框架，不包含网络协议栈、复杂业务逻辑和 UI 体系。

---

## 2. 开发总策略（先 Driver，后 Common）

- 第一优先级：确保板级外设可独立验证（Bring-up 成功）。
- 第二优先级：将验证过的能力沉淀到 `driver` 目录，形成稳定硬件抽象接口。
- 第三优先级：在 `common` 基于 `driver` 实现 `button`、`led_scene` 等组件。
- 第四优先级：再补齐 `common` 生命周期、日志、错误码、配置等公共能力。

一句话：先跑通硬件，再做抽象；先可用，再优雅。

---

## 3. 推荐目录演进（与当前仓库兼容）

```text
ESP32-S3/
├─ driver/
│  ├─ inc/
│  │  ├─ gpio.h
│  │  ├─ uart.h
│  │  ├─ i2c.h
│  │  ├─ spi.h
│  │  ├─ adc.h
│  │  ├─ pwm.h
│  │  └─ timer.h
│  ├─ src/
│  │  ├─ gpio.c
│  │  ├─ uart.c
│  │  ├─ i2c.c
│  │  ├─ spi.c
│  │  ├─ adc.c
│  │  ├─ pwm.c
│  │  └─ timer.c
│  └─ CMakeLists.txt
│
├─ common/
│  ├─ inc/
│  │  ├─ common.h
│  │  ├─ error.h
│  │  ├─ log.h
│  │  ├─ button.h
│  │  ├─ led_scene.h
│  │  └─ config.h
│  ├─ src/
│  │  ├─ common.c
│  │  ├─ error.c
│  │  ├─ log.c
│  │  ├─ button.c
│  │  ├─ led_scene.c
│  │  └─ config.c
│  ├─ components/
│  │  ├─ button/
│  │  └─ led_scene/
│  └─ CMakeLists.txt
├─ project/
└─ doc/
   └─ common_module_development_framework.md
```

---

## 4. 开发阶段计划（先基础外设）

## Phase 0：硬件基线确认（0.5 周）

目标：确认开发环境和板级资源真实可用，减少后续定位成本。

- 输出内容：
  - 引脚资源表（UART/I2C/SPI/ADC/PWM 对应 IO、上拉、复用关系）
  - 时钟与供电约束说明（尤其 ADC 参考和高频外设时钟）
  - `board bring-up checklist`
- 验收标准：
  - 工程可稳定编译下载
  - 串口日志可持续输出（无随机乱码/重启）

## Phase 1：Driver 首批落地（GPIO + UART + Timer，1 周）

目标：建立最小可调试系统和事件触发能力。

- `driver/gpio`
  - 输入/输出基础接口
  - 中断注册与回调
  - 电平读写与中断分离（IRQ 不做业务逻辑）
- `driver/uart`
  - 日志通道固定化（统一 baudrate、TAG 策略）
  - 基础收发（轮询或中断二选一，先保证稳定）
- `driver/timer`
  - 单次定时器
  - 周期定时器
- 验收标准：
  - LED 周期闪烁（Timer + GPIO）
  - 按键触发中断并输出日志（GPIO IRQ + UART）
  - 连续运行 2 小时无异常重启

## Phase 2：Driver 通信总线（I2C + SPI，1~1.5 周）

目标：打通最常见外设总线，形成可复用通信底座。

- `driver/i2c`
  - 主机模式初始化
  - 读写接口（支持寄存器读写）
  - 超时与重试机制（至少 1 次重试）
- `driver/spi`
  - 主机模式初始化
  - 半双工/全双工基础传输
  - 片选管理与频率配置
- 验收标准：
  - I2C: 读取 1 个已知器件 ID 成功率 > 99%
  - SPI: 连续 10,000 次传输无错误返回
  - 总线异常时日志包含错误码与总线编号

## Phase 3：Driver 采集控制（ADC + PWM，1 周）

目标：补齐采集与控制能力，满足基础传感和执行器场景。

- `driver/adc`
  - 单次采样与多次均值
  - 原始值到工程量的转换接口（预留标定参数）
- `driver/pwm`
  - 频率和占空比可调
  - 运行时动态更新无明显毛刺
- 验收标准：
  - ADC 采样波动在预期范围（定义测试条件）
  - PWM 驱动 LED 呼吸效果稳定

## Phase 4：Common 组件层建设（1 周）

目标：基于稳定 `driver`，建设 `common` 通用组件。

- `button`
  - 基于 `gpio + timer` 实现消抖、短按、长按、连按事件
- `led_scene`
  - 基于 `gpio/pwm + timer` 实现常亮、闪烁、呼吸、场景切换
- common 公共基础
  - `common_init()` 初始化编排
  - 统一错误码与日志接口
- `config` 参数管理
- 验收标准：
  - `project/main` 仅通过 `button/led_scene` 可完成演示
  - `driver` 不依赖 `common`，`common` 只依赖 `driver`（单向依赖）

---

## 5. 模块边界定义（重点）

### 5.1 driver 层职责（硬件抽象）

- 只处理硬件外设访问，不含业务语义
- 输出稳定、通用的 MCU 外设 API
- 典型命名：`gpio_*`、`uart_*`、`i2c_*`

### 5.2 common 层职责（通用组件）

- 对输入输出行为进行“语义化封装”
- 组件示例：`button`、`led_scene`
- 可直接被应用调用，减少业务重复代码

### 5.3 依赖关系约束

- 允许：`project -> common -> driver`
- 禁止：`driver -> common`
- 禁止：`project` 直接耦合过多 `driver` 细节（仅调试阶段可例外）

---

## 6. common 最小接口建议（围绕组件）

- 生命周期
  - `common_init(void)`
  - `common_start(void)`
- button
  - `button_init(...)`
  - `button_register_cb(...)`
- led_scene
  - `led_scene_init(...)`
  - `led_scene_set(...)`
- 基础能力
  - `log_xxx(...)`
  - `err_to_name(...)`

---

## 7. 工程化要求（每阶段都执行）

- 编译要求：`-Wall -Wextra` 无新增告警（`driver` 与 `common`）
- 接口要求：所有对外函数返回统一错误码，不直接泄漏底层 `esp_err_t`
- 日志要求：所有失败路径必须打印模块名、错误码、关键参数
- 文档要求：每个 `driver` 模块与每个 `common` 组件至少 1 个示例
- 回归要求：每个阶段结束执行一次 smoke test（上电 -> 初始化 -> 读写/采样 -> 日志输出）

---

## 8. 里程碑定义

### M1（第 1~2 周）：Driver 基础可用

- `gpio/uart/timer` 可稳定工作
- 具备最小调试与中断处理能力

### M2（第 3 周）：Driver 完整基础外设可复用

- `i2c/spi/adc/pwm` 稳定并通过压力验证
- `driver` 层接口冻结到 v1.0

### M3（第 4 周）：Common 组件可交付

- `button`、`led_scene` 可直接复用
- 文档与示例满足团队复用

---

## 9. 从当前仓库立刻可执行的动作（先看计划，再落地代码）

1. 新建 `driver` 目录与 `inc/src/CMakeLists.txt`。
2. 首批落地 `gpio`、`uart`、`timer`。
3. 调整 `common` 结构，预留 `button` 与 `led_scene`。
4. `project/main` 先通过 `driver` 做自检，再切换到 `common` 调用。
5. 阶段性评审通过后再开始实现 `button`、`led_scene`。

> 说明：该方案确保基础外设统一沉淀在 `driver`，`common` 聚焦可复用组件语义，便于后续多人协作与长期维护。

