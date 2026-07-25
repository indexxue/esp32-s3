# Camera ↔ MCU SPI 协议 — 评审结论与 ESP32 对接说明

**对应草案**：[`camera_spi_host_protocol_plan.md`](camera_spi_host_protocol_plan.md) v1.1  
**本文版本**：2.0  
**日期**：2026-07-25  
**读者**：ESP32-S3 Camera 固件 / 小车 MCU 联调双方  
**结论**：协议 **通过**；MCU **P0+P1 已落地**，可与 ESP 做 **1 MHz / 20 ms HEARTBEAT** 联调。MCU 尚未实现 DETECT/SERVO 深解析与 CTRL 闭环（P2/P3）。

| 侧 | 角色 | 当前状态 |
|----|------|----------|
| Camera（ESP32-S3） | **SPI Master** | **P0+P1 已落地**：SPI3 / 1 MHz / 20 ms HEARTBEAT（`camera_spi_host`）；可联调 L1 |
| 小车 TM4C123 | **SPI Slave** | **已就绪（L1）**：应答 HEARTBEAT；校验 magic/CRC；UART7 打链路日志 |

完整帧格式 / CRC / 命令表仍以草案为准；**本文是 MCU 评审回传 + ESP 实施清单**，冲突时以双方已确认参数为准（见 §3）。

---

## 1. 当前完成情况（MCU）

### 1.1 已完成

| 阶段 | 内容 | 仓库位置 |
|------|------|----------|
| P0 板级 | SSI0 → Slave，名 `Camera`；CLK/FSS 为输入 | `projects/{car-4wd,car-2wd,factory}/.syscfg/modules/ssi.json` |
| P0 BSP | `SSI_MODE_SLAVE`；32B ISR 补 FIFO；`start/load_tx/take_rx` | `bsp_driver/{inc,src}/bsp_spi.{h,c}` |
| P1 链路 | CRC-16/CCITT-FALSE；magic；HEARTBEAT TX；链路状态 | `Common/{inc,src}/camera_spi.{h,c}` |
| P1 接入 | 量产/厂测启动 init；20 ms `camera_spi_poll` | `projects/*/main/app.c` |

MCU 上电后行为（摘要）：

1. `Board_Periph_Init` → `bsp_spi_init(SLAVE)`  
2. `camera_spi_init` → CRC 自检（`0x29B1` / `0x6EB9`）→ 预装 HEARTBEAT → `bsp_spi_slave_start`  
3. 每 20 ms：若收齐 32B → 校验 → 统计 → 挂起下一拍 HEARTBEAT  
4. UART7 约每 2 s：`camera_spi: link=OK|DEGRADED|DOWN rx_ok=... crc_err=... magic_err=... last_msg=0x..`

### 1.2 未完成（ESP 可先按草案发，MCU 会校验但暂不业务消费）

| 阶段 | 内容 |
|------|------|
| P2 | 结构化解析 `0x10` DETECT、`0x20` SERVO 并缓存/日志关键字段 |
| P3 | MCU 发 `0x30` CTRL_CMD（`SLAVE_HAS_CMD`）+ 处理 `0x31` CTRL_ACK |

首版产品边界（已确认）：**仅云台 + 检测**；不进底盘运动闭环。

### 1.3 MCU 联调里程碑对照

| 里程碑 | MCU | 说明 |
|--------|-----|------|
| L0/L1 | **已具备** | 合法 HEARTBEAT 应答；CRC/线序可验 |
| L2 | 未做 | 等 ESP 发 DETECT/SERVO 后 MCU 补解析 |
| L3 | 未做 | 控制闭环后半段 |

---

## 2. 接线（已锁定，按信号名对接）

| 协议信号 | ESP32-S3（Master） | TM4C123（Slave） | 备注 |
|----------|--------------------|------------------|------|
| SCK | **GPIO 21** | **PA2** `SSI0CLK` | Mode0，空闲低 |
| CS（低有效） | **GPIO 14** | **PA3** `SSI0FSS` | 硬件 FSS；**板无外上拉** |
| MOSI | **GPIO 47** | **PA4** `SSI0RX` | Master → Slave |
| MISO | **GPIO 45** | **PA5** `SSI0TX` | Slave → Master |
| GND | 共地 | 共地 | **必须先接** |

- 排线已预留，可随时对接；**勿热插拔**（无 CS 上拉）。  
- 勿按「两边同号 GPIO」接线；以本表信号名为准。  
- 电平 **3.3 V**，无需电平转换。

---

## 3. 双方已确认的物理层 / 协议参数

| 参数 | 值 |
|------|-----|
| 角色 | Camera=Master，MCU=Slave |
| 线数 | 仅 4 线，**无 IRQ** |
| Mode | **0**（CPOL=0，CPHA=0） |
| 位序 | MSB first |
| 字节序 | 小端 |
| 帧长 | **固定 32 字节**全双工 |
| magic | `0xA55A`（线上首两字节 `5A A5`） |
| CRC | CRC-16/CCITT-FALSE；覆盖字节 `[0..29]` |
| 联调时钟 | **1 MHz**（升钟 ≤4 MHz 且须误码验收；不默认承诺 8 MHz） |
| 轮询周期 | Master **约 20 ms** 发起一次 |
| CS 时序 | CS↓→首钟 ≥1 µs；末位→CS↑ ≥1 µs；CS 高间隔 ≥10 µs |

CRC 自检（上总线前双方必须通过）：

- ASCII `"123456789"` → **`0x29B1`**  
- 空 HEARTBEAT 头 30 字节（草案 5.4）→ **`0x6EB9`**（帧末小端 `B9 6E`）

---

## 4. ESP32 侧要做什么（实施清单）

> MCU 已按 Slave 等待；**联调能否打通取决于 ESP 是否按下列实现 Master**。

### 4.1 硬件 / SPI 驱动（阻塞）

1. 使用 **独立于 LCD 的 SPI 主机**（草案：产品总线约 SPI3；勿与 LCD 抢同一总线事务）。  
2. 配置：**Mode0、MSB、8 bit、1 MHz**。  
3. 每次事务：拉低 CS → 全双工交换 **恰好 32 字节** → 拉高 CS。  
4. 遵守 §3 CS 时序；推挽驱动 CS（板侧无上拉）。  
5. 共地后上电；先短线联调。

### 4.2 链路层（阻塞，先于业务）

每拍 TX（MOSI）必须是合法 32B 帧：

| 偏移 | 字段 | ESP 首版建议 |
|------|------|----------------|
| 0..1 | magic | `5A A5` |
| 2 | ver | `0x01` |
| 3 | seq | 本端自增 |
| 4 | msg_id | 先发 **`0x01` HEARTBEAT** |
| 5 | flags | 0 |
| 6 | len | HEARTBEAT=`8` |
| 7 | rsv | 0 |
| 8..29 | payload | 见草案 6.2（`uptime_ms`、`role=1`、`proto_ver`、`err_flags`） |
| 30..31 | crc16 | 小端，覆盖 0..29 |

每拍同时读 MISO 32B，并：

1. 检查 magic / CRC；失败计数，**不执行**其中命令。  
2. 合法时解析 `msg_id`：当前 MCU 主要回 **`0x01` HEARTBEAT**（`role=2`，`uptime_ms` 递增）。  
3. 链路判定与草案 4.3 对齐（近 1 s 有合法帧 → OK；连续坏 magic → DOWN 等）。

**联调顺序（强制）**：只开 HEARTBEAT → 稳定 1 min → 再开 DETECT/SERVO。

### 4.3 Master 发送优先级（草案 5.5，业务阶段）

每 20 ms 最多一帧，从高到低：

1. 上拍 MISO 带 `SLAVE_HAS_CMD` → 本拍发 HEARTBEAT，让 MCU 吐 `CTRL_CMD`（**MCU P3 未做前可忽略**）  
2. 有新检测 → `0x10` DETECT_RESULT  
3. 舵机遥测到期 → `0x20` SERVO_TELEMETRY  
4. 否则 → `0x01` HEARTBEAT  

### 4.4 业务帧（MCU P2 就绪后生效；ESP 可先实现发送）

| msg_id | 方向 | ESP 职责 |
|--------|------|----------|
| `0x10` DETECT | ESP→MCU | 最多 2 框；坐标原点左上；优先高分；`best_index` 正确 |
| `0x20` SERVO | ESP→MCU | 角度 `deg_x100`；脉宽 µs；限位字段按草案 |
| `0x30` CTRL | MCU→ESP | **暂无**；预留：见 `SLAVE_HAS_CMD` 后拉命令 |
| `0x31` CTRL_ACK | ESP→MCU | 对 `req_id` 回 `result`（含 `UNSUPPORTED`） |

首版 CTRL 子命令期望（MCU 将来会发）：

- 需支持：`DETECT_ENABLE`、`SERVO_CENTER`、`SERVO_SET_ANGLE` / `NUDGE`  
- 可先 `UNSUPPORTED`：`FOLLOW_ENABLE`、`SET_STREAM_MODE`

### 4.5 ESP 侧联调自检（建议日志）

- MOSI 发出前打印：seq / msg_id / crc  
- MISO 收到后打印：magic 是否 `A55A`、crc ok、`msg_id`、`uptime_ms`、`role`  
- 统计：`rx_ok` / `crc_err` / `magic_err`；目标联调阶段 CRC 错率 **&lt; 0.1%**（1 MHz）

### 4.6 ESP 已知工程约束（自查）

- LCD 占用另一路 SPI：隔离事务，升钟时盯 LCD 是否花屏。  
- 无 IRQ：MCU→ESP 命令延迟受 20 ms 轮询限制（P3 后再验）。  
- 控制命令不保证 exactly-once：ACK + `req_id` + 幂等。

---

## 5. 双方联调验收阶梯

| 阶梯 | 通过标准 | MCU | ESP |
|------|----------|-----|-----|
| A. 参数一致 | 本文 §3 + 草案 CRC 向量双方算出 | 已做 | **须做** |
| B. 接线 | 按 §2；共地；不热插拔 | 就绪 | **须接** |
| C. L0/L1 | 1 MHz：ESP 读到合法 MCU HEARTBEAT；MCU UART7 `link=OK`、`rx_ok` 增 | **已具备** | **须发合法帧** |
| D. 稳定 | ≥1 min，CRC 错率低于约定 | 统计已有 | 对齐统计 |
| E. L2 业务 | MCU 正确识别 DETECT/SERVO 字段 | P2 待做 | 发业务帧 |
| F. L3 控制 | `SERVO_CENTER` / `DETECT_ENABLE` 得 ACK=OK | P3 待做 | 执行+ACK |
| G. 压力 | 50 Hz×10 min；拔线恢复（尽量不热插） | 后续 | 后续 |

**未完成 A～D，不要进业务联调。**

现场检查（上电前）：

- [ ] GND 已接，3.3 V  
- [ ] SCK/MOSI/MISO/CS 按 §2（无交叉）  
- [ ] Mode0、MSB、32B、同一 CRC、1 MHz、约 20 ms  
- [ ] ESP 已通过 `"123456789"→0x29B1`  
- [ ] MCU UART7 115200 可见 `camera_spi: ready` / 周期 link 日志  

---

## 6. MCU 侧实现细节（供 ESP 理解对方行为）

| 项 | 说明 |
|----|------|
| TX 策略 | 默认每拍准备 **HEARTBEAT**（`role=2`）；下一拍 TX 在帧完成后挂起换入 |
| 全双工语义 | 本拍 MISO = **上一拍已算好的应答**；勿期望同拍即时 ACK |
| FIFO | TM4C SSI FIFO 深 8，MCU 用 **ISR** 补仓；ESP 勿把帧拆成多次 CS |
| 日志口 | **UART7**（PE0/PE1 @ 115200）；UART0 是蓝牙协议，无关 SPI |
| 车型 | `car-4wd` / `car-2wd` / `factory` 均已 Slave + `camera_spi` |

---

## 7. 评审确认表（定稿）

| # | 项 | 接受 | 备注 |
|---|-----|------|------|
| 1 | Camera=Master，MCU=Slave | Y | |
| 2 | 仅 4 线，无 IRQ | Y | |
| 3–8 | Mode0 / MSB / 小端 / 32B / magic / CRC | Y | |
| 9–10 | 1 MHz 联调；升钟 ≤4 MHz 验收 | Y | |
| 11 | 20 ms 轮询 | Y | |
| 12–13 | DETECT≤2；`deg_x100` | Y | |
| 14 | CTRL 表 | Y* | Follow/Stream 可 UNSUPPORTED |
| 15–16 | `SLAVE_HAS_CMD`；保障/非保障 | Y | |
| 17 | 脚位 | Y | §2 |

**评审结论**：☑ **通过**（MCU P0+P1 已实施；ESP 按 §4 实现即可联调 L1）  

**MCU / 日期**：本仓库 / 2026-07-25  
**Camera / 日期**：________________  

---

## 8. 已关闭待决项

| # | 结论 |
|---|------|
| 排线 | 已预留，锁定 SSI0 PA2/3/4/5 |
| CS 上拉 | 无外上拉；短线 + Master 推挽；不热插拔 |
| 业务范围 | 首版仅云台 + 检测 |
| 工程范围 | 三工程同期 Slave |

---

## 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-07-25 | 1.0～1.4 | MCU 评审、待决收口、P0/P1 落地 |
| 2026-07-25 | **2.0** | 重写为「MCU 完成状态 + ESP32 实施清单 + 联调阶梯」；过时 Master 缺口描述清除 |
| 2026-07-25 | 2.1 | ESP P0+P1 Master 落地（双 SPI host + HEARTBEAT）；可进入阶梯 C/D 联调 |
