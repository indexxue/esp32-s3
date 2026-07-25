# ESP32 Camera ↔ TM4C SPI 联调现状说明（致 MCU）

**日期**：2026-07-25  
**读者**：小车 TM4C 固件 / 硬件联调  
**ESP 工程**：`camera`（ESP32-S3）  
**协议正文**：[`camera_spi_host_protocol_plan.md`](camera_spi_host_protocol_plan.md)  
**评审基线**：[`camera-spi-host-protocol-review.md`](camera-spi-host-protocol-review.md)

本文描述 **ESP 侧当前已实现内容、实测现象、与 MCU 日志的对照结论，以及 MCU 需配合的事项**。可单独转发，无需访问 ESP 仓库源码。

---

## 1. ESP 侧当前状态（摘要）

| 项 | 状态 |
|----|------|
| 角色 | SPI **Master** |
| 总线 | 硬件 SPI3（与 LCD 的 SPI2 隔离） |
| 参数 | Mode0、MSB、1 MHz、每拍全双工 **32 字节**、约 **20 ms** 一轮 |
| 首版业务 | **仅 HEARTBEAT**（`msg_id=0x01`，`role=1`） |
| 固件模块 | `camera_spi_host` + `spi_link`（CRC-16/CCITT-FALSE） |
| CRC 自检 | 上电通过：`"123456789"→0x29B1` |
| 软件对调 MOSI/MISO | **已固化**：因 TM4C 丝印标反，逻辑 MOSI=GPIO45、MISO=GPIO47（见 §2） |

**ESP 认为：协议发送侧已按评审稿实现；当前卡在物理链路 / 对端收发通路，不是组帧错误。**

---

## 2. 接线（锁定）

| 信号 | ESP32-S3（逻辑） | TM4C123 芯片脚 | 说明 |
|------|------------------|----------------|------|
| SCK | GPIO **21** | PA2 `SSI0CLK` | |
| CS（低有效） | GPIO **14** | PA3 `SSI0FSS` | |
| MOSI（Master→Slave） | GPIO **45** | PA4 `SSI0RX` | |
| MISO（Slave→Master） | GPIO **47** | PA5 `SSI0TX` | |
| GND | 共地 | 共地 | |

**丝印说明（2026-07-25 确认）**：TM4C 侧排线/丝印上的「MOSI」「MISO」文字标反。  
ESP 已在 `board.h` 把逻辑 MOSI/MISO 的 GPIO 对调为上表，**按芯片脚同名功能对接**（ESP 逻辑 MOSI→PA4，逻辑 MISO→PA5），不要再按错误丝印交叉。

板无 CS 外上拉，**勿热插拔**。
---

## 3. ESP 正在发什么（请 MCU 对照 `bad=`）

每 20 ms 一帧合法 HEARTBEAT，线上前 8 字节形如：

```text
5A A5 01 xx 01 00 08 00
```

| 偏移 | 值 | 含义 |
|------|-----|------|
| 0..1 | `5A A5` | magic `0xA55A` 小端 |
| 2 | `01` | ver |
| 3 | `xx` | seq（自增） |
| 4 | `01` | HEARTBEAT |
| 5 | `00` | flags |
| 6 | `08` | payload len=8 |
| 7 | `00` | rsv |
| 8..15 | uptime_ms LE + `role=1` + `proto_ver=1` + err_flags | |
| 30..31 | CRC16 LE | 覆盖字节 0..29 |

ESP 串口约每 **2 s** 打印一次（烧录含 TX/RX 日志的固件后）：

```text
spi1: TX 5A A5 01 .. 01 00 08 00
spi1: RX .. .. .. .. .. .. .. ..
spi1: link=... rx_ok=... magic_err=...
```

- **`TX`** = 本拍发出的 MOSI 头 8 字节（应始终接近上表）  
- **`RX`** = 本拍读到的 MISO 头 8 字节（MCU 正常时应为 `5A A5 01 ..` 且 `role=2`）

---

## 4. 双方已观测到的现象（2026-07-25）

### 4.1 ESP 侧

| 现象 | 含义 |
|------|------|
| `TX` / 组帧为 `5A A5 01 xx 01 00 08 00` | Master 发送缓冲正确 |
| `RX` 曾长期为 `00 00 00 00...` | GPIO45 上采不到有效 Slave 数据 |
| `xfer_err=0` | SPI 主机事务本身成功（有钟、有 CS、传满 32B） |
| `link=DOWN`，`magic_err` 涨 | 因 RX 无合法 magic |
| 软件对调 47/45 后 RX 仍全 0 | 当时线松；根因后确认为 **MCU 丝印 MOSI/MISO 标反**，ESP 已固化脚位对调 |

### 4.2 MCU 侧（你们提供的日志）

```text
camera_spi: link=DOWN rx_ok=0 crc_err=0 magic_err=... 
  bad=00 00 00 00 00 00 00 00 tx=A5 A5 A5 A5
  isr=... rx_b=... frm=... rxor=0
```

| 字段 | ESP 解读 |
|------|----------|
| `isr` / `rx_b` / `frm` 增加，`rxor=0` | 有时钟、能收满帧、无 RX 溢出 → **时序骨架基本 OK** |
| `bad=00...` | PA4 采到的是全 0，**没有收到** ESP 的 `5A A5...` |
| `tx=A5 A5 A5 A5` | 你们在做 TX 填充测试；这不是协议 HEARTBEAT |
| `rx_ok=0` / `magic_err≈frm` | 与 `bad=00` 一致 |

### 4.3 综合结论

1. **协议层（帧格式/CRC/周期）**：ESP 已按评审实现；MCU 也有 Slave + 统计。  
2. **物理/通路**：至少 **ESP GPIO47 → TM4C PA4（MOSI）** 当前不通或一直为低（线松、接触不良、接错针、对地等）。  
3. **回程**：MCU 发 `A5` 时，若 ESP `RX` 仍全 0，则 **PA5 → GPIO45** 同样可疑。  
4. 现场曾怀疑线松，与上述「有钟、收满、内容全 0」高度吻合 → **请优先复测排线通断并插紧**。

---

## 5. 请 MCU 侧配合的事项

### 5.1 硬件（优先）

1. 万用表：ESP47↔PA4、ESP45↔PA5 通断；晃线看是否时通时断。  
2. 确认共地；47/45 不与 GND 短接。  
3. 插紧排线后两边同时看日志。

### 5.2 日志对照（通路通后）

| 期望 | MCU | ESP |
|------|-----|-----|
| MOSI 通 | `bad=5A A5 01 xx 01 00 08 00` | `TX` 同左 |
| MISO 通且发正式心跳 | — | `RX=5A A5 01 xx 01 ..`，`peer_role=2` |
| 链路 OK | `link=OK`，`rx_ok` 增 | `link=OK`，`rx_ok` 增 |

### 5.3 软件注意

1. **`tx=A5` 仅用于 TX 通路测试**。通路确认后请改回正式 HEARTBEAT（`magic=5A A5`，`role=2`，CRC 正确），否则 ESP 即使收到 `A5` 也会 `magic_err`。  
2. CS 拉低前预装下一拍 TX；本拍 MISO = 上一拍已准备好的帧。  
3. 暂勿发 DETECT/SERVO/CTRL；先稳定 HEARTBEAT ≥1 min（评审阶梯 A～D）。

### 5.4 回传请附

- 插紧排线后 MCU 一条完整 `camera_spi:`（含 `bad=` / `tx=`）  
- ESP 同时段 `spi1: TX` / `spi1: RX` / `spi1: link=...`  
- 通断测量结果（47–PA4、45–PA5）

---

## 6. ESP 日志说明（方便你们读串口）

| 日志 | 含义 |
|------|------|
| `spi1 init ok SCK=21 MOSI=45 MISO=47 CS=14 1000000Hz` | Master 已起来（MOSI/MISO 已按 MCU 丝印对调） |
| `spi1: TX ...` | 发出头 8 字节（每 2 s） |
| `spi1: RX ...` | 读回头 8 字节（每 2 s） |
| `spi1: link=OK` | 近 1 s 内收到合法 magic+CRC |
| `spi1: link=DOWN` | 连续多帧无合法应答 |
| `spi1: CRC selftest failed` | 勿上总线，固件异常 |

---

## 7. 通过标准（L1）

- MCU：`link=OK`，`rx_ok` 递增，`magic_err` 不再每帧涨；`bad` 为 `5A A5...`  
- ESP：`link=OK`，`RX` 为 `5A A5...`，`peer_role=2`，`rx_ok` 递增  
- 稳定运行 ≥1 分钟后再做 DETECT/SERVO（P2）

---

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-07-25 | 初版：汇总 ESP 已实现、双方日志对照、线松/通路结论与 MCU 待办 |
| 2026-07-25 | 确认 TM4C 丝印 MOSI/MISO 标反；ESP 逻辑脚改为 MOSI=45、MISO=47 |
