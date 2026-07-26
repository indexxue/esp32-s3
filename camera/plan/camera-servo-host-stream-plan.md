# 舵机控制 × 上位机图传接入计划

**版本**：0.1  
**日期**：2026-07-26  
**路径**：`camera/plan/camera-servo-host-stream-plan.md`  
**关联协议**：`camera/plan/camera-spi-protocol.md`（v1.7+，双方公共 SPI 约定）  
**文档性质**：产品分工与接口落地计划（**非**改写既有 SPI 帧格式的定稿；定稿后须回写协议文档并升版本）

---

## 0. 一句话结论

| 通道 | 传什么 | 不传什么 |
|------|--------|----------|
| **SPI**（Camera Master ↔ TM4C Slave） | 舵机控制 / 检测框 / 云台遥测 / **图传入口元数据（IP、端口、路径枚举）** | **不传 JPEG / MJPEG / 任何图像字节** |
| **WiFi HTTP**（Camera ↔ 小车上位机） | MJPEG 图传、JPEG 快照、网页/REST | 不作为车控主通道 |

**上位机拿图传**：先经车（TM4C）从 SPI 拿到相机 **IP（+端口）**，再按约定 URL 用 HTTP 拉流。SPI 只做「指路」与「控舵机」。

---

## 1. 系统分工

```text
┌─────────────┐  SPI 32B @~20ms   ┌──────────────┐
│  TM4C123    │◄─────────────────►│ ESP32-S3     │
│  小车主控   │  控制 / 遥测 / IP  │ Camera       │
└──────┬──────┘                   └──────┬───────┘
       │ UART/USB/既有链路                │ WiFi STA/SoftAP
       ▼                                  ▼
┌─────────────┐   HTTP MJPEG/JPEG  ┌──────────────┐
│ 上位机工具  │◄───────────────────│ 图传服务      │
│ （小车侧）  │   不经 SPI         │ :80 等        │
└─────────────┘                   └──────────────┘
```

### 1.1 职责

| 角色 | 职责 |
|------|------|
| TM4C | 决策云台动作；经 SPI 下发 `CTRL_CMD`；缓存 `SERVO_TELEMETRY` / 检测结果；向车侧上位机转发 **相机 IP / 流地址** |
| ESP Camera | 执行舵机；上报遥测；提供 HTTP 图传；经 SPI 回答「我的 IP 是多少」 |
| 上位机工具 | 用 IP 打开 MJPEG；**不**指望 SPI 出图像；可选经车侧协议间接发舵机命令 |

### 1.2 强制边界（与现协议一致）

- 协议 N4：**不传 JPEG/图像**（SPI）
- 首版产品：云台 + 检测 + 图传入口元数据；**不进底盘运动闭环**
- `SET_STREAM_MODE`：可继续 `UNSUPPORTED`；图传开关由 WiFi/HTTP 侧决定，不必经 SPI 推流

---

## 2. SPI 舵机控制接口（已约定，首版可用）

> 权威字段表见 `camera-spi-protocol.md` §5.6。下文只摘产品用法。

### 2.1 帧与流程

- MCU→Camera：`CTRL_CMD (0x30)` + MISO `flags.SLAVE_HAS_CMD`
- Camera→MCU：后续拍 `CTRL_ACK (0x31)`，按 `req_id` 对账
- Camera 侧 ESP 已实现并驱动真实舵机（见 `camera_spi_host.c`）

### 2.2 舵机 sub_cmd

| sub_cmd | 名 | 参数 | 说明 |
|---------|-----|------|------|
| `0x10` | `SERVO_SET_ANGLE` | `u8 ch` + `i16 deg_x100` | 绝对角；`ch`：0=Pan，1=Tilt |
| `0x11` | `SERVO_NUDGE` | `u8 ch` + `i16 delta_x100` | 相对步进 |
| `0x12` | `SERVO_CENTER` | 无 | 双轴回中 |
| `0x13` | `SERVO_SET_LIMITS` | 4×`i16`（×100） | 软限位 |
| `0x14` | `SERVO_RESET_LIMITS` | 无 | 恢复默认限位 |

**单位**：`deg_x100`（180.00° → `18000`）。ACK：`result` 0=OK / 1=BAD_PARAM / 2=BUSY / 3=UNSUPPORTED / 4=FAILED。

### 2.3 遥测（Camera→MCU，只读）

- `SERVO_TELEMETRY (0x20)`：约 10～20 Hz，角度/脉宽/限位  
- 控完后 ESP 会尽快插发一帧遥测，避免上位机/MCU 仍显示旧角

### 2.4 MCU 厂测命令（联调）

| 命令 | 期望 |
|------|------|
| `cam center` | ACK=OK，云台回中 |
| `cam angle 0 18000` | Pan→180° |
| `cam nudge 1 -500` | Tilt −5° |
| `cam servo` | 看到最新 `0x20` |

### 2.5 与网页 REST 对照（同源硬件，不同通道）

| 能力 | SPI | HTTP（本机调试） |
|------|-----|------------------|
| 回中 | `SERVO_CENTER` | `POST /api/servo` `{"center":1}` |
| 绝对角 | `SERVO_SET_ANGLE` | `{"pan":180}` / `{"tilt":...}` |
| 步进 | `SERVO_NUDGE` | `{"pan_delta":5}` |
| 状态 | `SERVO_TELEMETRY` | `GET /api/servo/status` |

**量产车控路径以 SPI 为准**；HTTP 舵机接口保留给调试页，不是小车主链路。

---

## 3. 上位机如何获取图传（不经 SPI 传图）

### 3.1 现成 HTTP 端点（Camera 已有）

默认 HTTP 端口：NVS `web_ctrl.http_port`，出厂多为 **80**。

| 用途 | 方法 | 路径 |
|------|------|------|
| 能力/状态 JSON | GET | `/api/camera/status`（含 `"stream":"/api/camera/stream.mjpg"`） |
| **MJPEG 图传** | GET | `/api/camera/stream.mjpg` |
| 单帧 JPEG | GET | `/api/camera/camera.jpg` |
| 暂停/恢复 | GET | `/api/camera/camera_pause` · `/api/camera/camera_resume` |

完整 URL 示例：

```text
http://<camera_ipv4>:<port>/api/camera/stream.mjpg
http://<camera_ipv4>:<port>/api/camera/camera.jpg
```

上位机接入步骤（推荐）：

1. 从车侧拿到 `<camera_ipv4>` 与 `<port>`（见 §4）  
2. （可选）`GET /api/camera/status` 确认 `camera:true`、读 `stream` 字段  
3. 打开 MJPEG：`multipart/x-mixed-replace`（浏览器 `<img src>` 或工具内 HTTP 客户端）  
4. 与相机 **同一局域网**（STA 同网段，或连相机 SoftAP）

### 3.2 网络前提

| 模式 | 上位机怎么连 |
|------|----------------|
| STA | 相机加入路由器；上位机与相机同网；IP 为 DHCP 地址 |
| SoftAP | 上位机（或中继）连相机 AP；IP 多为 AP 地址（常见 `192.168.4.1`） |

敏感写接口（配网/OTA）与媒体只读接口的 ACL 不同；**图传按现有 `web_local_peer` / 子网规则**，计划不改 ACL，只约定「如何发现 IP」。

### 3.3 明确不做

- SPI 上传输 JPEG/MJPEG  
- 用 SPI `SET_STREAM_MODE` 当图传总线（可长期 UNSUPPORTED）  
- 要求上位机直连 SPI  

---

## 4. SPI「图传入口」接口（待增补）

现状：协议有 `device_flags.WIFI_UP`，**没有**把 IPv4/端口装进 32B 帧的定稿消息。  
目标：MCU（再转上位机）能稳定拿到「连哪台、开哪条 URL」。

### 4.1 建议方案（定稿前双方确认）

新增应用消息（草案，**未写入协议正文前勿当已冻结**）：

| msg_id | 名 | 方向 | 说明 |
|--------|-----|------|------|
| `0x03` | `NET_INFO` | Camera→MCU | 主动或应询上报网络入口 |

或走控制对账（与舵机同一套路）：

| sub_cmd | 名 | 参数 | ACK |
|---------|-----|------|-----|
| `0x21` | `GET_NET_INFO` | 无 | `result=OK` 后由 Camera 下一优先拍发 `NET_INFO`；或 `detail` 仅带 IPv4（端口另约定默认 80） |

**推荐**：独立 `NET_INFO (0x03)`，避免把端口/模式塞进 4 字节 `detail`。

### 4.2 `NET_INFO` payload 草案（len≤22）

| 偏移 | 类型 | 字段 |
|------|------|------|
| 0 | u32 | `ipv4`：四段小数点地址按 **小端字节序** 打包，`a.b.c.d` → `a \| b<<8 \| c<<16 \| d<<24`；无效时 `0` |
| 4 | u16 | `http_port`（通常 80） |
| 6 | u8 | `wifi_mode`：0=OFF，1=STA，2=SoftAP |
| 7 | u8 | `flags`：bit0=`HAS_IP`，bit1=`HTTP_UP`，bit2=`STREAM_READY` |
| 8 | u8 | `stream_path_id`：0=`/api/camera/stream.mjpg`（路径用枚举，**不在 SPI 传字符串**） |
| 9 | u8 | `rsv` |
| 10 | u16 | `rsv2` |

MCU / 上位机组 URL：

```text
http://{a}.{b}.{c}.{d}:{http_port}{path_from_id}
```

### 4.3 发送策略（建议）

1. MCU 需要图传时发 `GET_NET_INFO`（或周期性读缓存）  
2. Camera：WiFi IP 变化或 `GET_NET_INFO` 后，按 §4.5 优先级插发 `NET_INFO`（低于 `CTRL_ACK` / `SLAVE_HAS_CMD` yield，可高于或并列于 DETECT——双方定一条即可）  
3. IP 未就绪：`HAS_IP=0`，`ipv4=0`，ACK 可用 `BUSY` 或仍发 `NET_INFO` 空地址  

### 4.4 ESP 实现锚点（落地时）

- IP：`net_wifi_format_ipv4_for_display()` / `esp_netif_get_ip_info`  
- 端口：NVS `web_ctrl.http_port`  
- 模式：`net_wifi_get_mode()`  
- 模块：`camera_spi_host.c` + `spi_link.*`；改完同步 `camera-spi-protocol.md` 升版本  

### 4.5 车 → 上位机

SPI 止于 TM4C。上位机侧沿用小车既有通道（UART 文本 / 二进制帧 / 厂测命令），建议最小字段：

```text
cam_ip=192.168.1.23
cam_port=80
cam_stream=/api/camera/stream.mjpg
```

或一条完整 URL。**本仓库只约定 SPI 与 HTTP；车↔上位机帧格式由 TM4C 工程自定**，但语义应对齐上表。

---

## 5. 端到端用例

### 5.1 上位机开图传

```text
上位机 → 车：请求相机流地址
车 → ESP：SPI GET_NET_INFO（或读最近 NET_INFO）
ESP → 车：NET_INFO（IP/端口/path_id）
车 → 上位机：URL
上位机 → ESP：GET http://IP:port/api/camera/stream.mjpg
```

### 5.2 上位机/车控云台（图传同时开着）

```text
决策方 → 车：云台意图（或车本地决策）
车 → ESP：CTRL_CMD SERVO_*
ESP → 车：CTRL_ACK + 后续 SERVO_TELEMETRY
图传通道不变（仍走 WiFi）
```

### 5.3 验收抽检

| # | 操作 | 期望 |
|---|------|------|
| S1 | `cam center` / angle / nudge | ACK=OK，角度变，图传可同时在线 |
| S2 | WiFi 已获 IP 后查 NET_INFO | `HAS_IP=1`，地址与串口/LCD 显示一致 |
| S3 | 上位机打开组装 URL | MJPEG 出图；SPI `rx_ok` 不因拉流崩溃 |
| S4 | 断 WiFi | `HAS_IP=0`；SPI 舵机仍可用 |
| S5 | 故意要求 SPI 出图 | **拒绝**；无此类 msg |

---

## 6. 里程碑

| ID | 内容 | 状态 |
|----|------|------|
| M0 | SPI 舵机 CTRL + 遥测（协议 L3） | **ESP 已通；继续与 MCU 抽检** |
| M1 | 双方确认 §4 `NET_INFO` / `GET_NET_INFO` 草案 | **已定稿（协议 v1.9）** |
| M2 | 回写 `camera-spi-protocol.md` 升版本；两端头文件同步 | **ESP 已完成** |
| M3 | ESP：组 `NET_INFO`；响应查询；IP 变化触发 | **已完成** |
| M4 | TM4C：缓存 NET_INFO；厂测/转发上位机 | 待联调 |
| M5 | 上位机：用车给的 URL 拉 `/api/camera/stream.mjpg` | 待做（车侧工具） |
| M6 | 联调：控舵机 + 图传同开；断网恢复 | 待做 |

---

## 7. 文档与同步

| 文档 | 用途 |
|------|------|
| `camera-spi-protocol.md` | SPI 帧 / msg_id / CRC **唯一定稿**；本计划中的 `0x03`/`0x21` 确认后并入 |
| **本文** | 产品分工：舵机走 SPI、图传走 WiFi、IP 经 SPI 指路 |
| TM4C 仓库同名 plan（建议） | 与 ESP 同步；车↔上位机转发格式可附附录 |

改 SPI 布局或 msg_id：**升协议版本 + 变更记录 + 两端同步**。

---

## 变更记录

| 日期 | 版本 | 说明 |
|------|------|------|
| 2026-07-26 | 0.1 | 初稿：舵机 SPI 复用既有 CTRL；图传走 HTTP；SPI 增补 NET_INFO/IP 草案 |
