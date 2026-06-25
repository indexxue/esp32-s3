# ESP32-S3 BLE 演示工程（ble_demo）

面向零基础读者的说明文档：本工程在 ESP32-S3 上运行 **BLE（低功耗蓝牙）**，让手机能扫描、连接、读写数据，并控制板载 RGB 灯效。

---

## 一、这个 Demo 能做什么？

| 能力 | 说明 |
|------|------|
| 被手机发现 | 广播名称 `ESP32-S3-BLE-Demo` |
| 建立连接 | 手机作为「中心」，ESP32 作为「外设」 |
| 读写数据 | 通过 GATT 特征值 `0xFFF1` 读/写最多 20 字节 |
| 主动推送 | 手机订阅 Notify 后，设备每秒推送 4 字节计数 |
| RGB 反馈 | WS2812 灯珠（GPIO48，16 颗）随连接状态与写入命令变化 |

**重要：** ESP32-S3 **只支持 BLE**，不支持经典蓝牙（例如普通蓝牙耳机用的 BR/EDR 协议）。

---

## 二、应用领域（BLE 一般用在哪？）

BLE 功耗低、连接简单，适合「手机 / 平板 ↔ 小型设备」短距离通信。常见场景包括：

### 1. 消费电子

- 智能手环、手表（步数、心率上传）
- 蓝牙温度计、湿度计、体重秤
- TWS 耳机的配置与电量查询（走 BLE 控制通道）

### 2. 智能家居

- 智能灯泡、插座、门锁（手机 App 近场控制）
- 温湿度传感器上报
- Beacon：设备只广播小数据，不建立长连接（室内定位、到店提醒）

### 3. 工业与物联网

- 设备近场配网（如 ESP32 蓝牙配 Wi-Fi）
- 传感器数据采集（振动、压力、气体）
- 资产标签、巡检点（低功耗、电池供电数年）

### 4. 医疗与健康

- 血压计、血糖仪、血氧仪
- 可穿戴生命体征监测

### 5. 本 Demo 对应的「最小原型」

本工程是上述场景的一个 **最小可运行模板**：

- ESP32 = 被控制的「外设」（传感器 / 执行器的替身）
- 手机 = 「中心设备 / App」
- 特征值读写 = App 下发命令或读取状态
- Notify = 设备主动上报（类似传感器定时采样）
- RGB 灯 = 执行器反馈（类似继电器、电机、指示灯）

学会本 Demo 后，你可以把「写 `'2'` 切黄灯」改成「写 3 字节 RGB 控制真实硬件」，或把 Notify 的 4 字节计数改成温湿度数据。

---

## 三、蓝牙通信入门（小白向）

### 3.1 两种蓝牙

| 类型 | 名称 | 特点 | 例子 |
|------|------|------|------|
| 经典蓝牙 | BR/EDR | 带宽较高、功耗较大 | 蓝牙耳机、音箱 |
| 低功耗蓝牙 | BLE | 功耗极低、间歇通信 | 手环、Beacon、本 Demo |

ESP32-S3 芯片内置的是 **BLE**，请用手机上的 BLE 调试 App（如 nRF Connect），而不是去连「音频蓝牙」。

### 3.2 两个角色

```
  ┌─────────────┐         BLE 无线          ┌──────────────────┐
  │   手机      │  ◄──────────────────────► │  ESP32-S3        │
  │  Central    │      扫描 / 连接 / 读写    │  Peripheral      │
  │  （中心）    │                            │  （外设 / 从机）  │
  └─────────────┘                            └──────────────────┘
```

- **Central（中心）**：主动扫描、发起连接，一般是手机或 USB 蓝牙 dongle。
- **Peripheral（外设）**：广播自己的名字，等待被连，本 Demo 的 ESP32 就是外设。

### 3.3 从「扫描」到「传数据」的流程

```
1. 广播（Advertising）
   ESP32 间歇发送小 packet：「我叫 ESP32-S3-BLE-Demo，欢迎来连我」

2. 扫描（Scan）
   手机听到广播，在列表里显示设备名

3. 连接（Connect）
   手机发起连接，建立一条逻辑链路

4. 服务发现（Service Discovery）
   手机查询：「你有哪些服务？每个服务里有哪些特征值？」

5. 读写 / 通知（Read / Write / Notify）
   手机与特征值交换数据
```

### 3.4 GATT：数据的「文件夹结构」

BLE 传业务数据时，最常用的是 **GATT**（Generic Attribute Profile）。可以把它想成：

```
设备
 └── Service（服务）        ← 一类功能，例如「自定义控制服务」
      └── Characteristic（特征值）  ← 具体一条数据，例如「命令 / 状态」
           └── Descriptor（描述符）   ← 特征值的附加说明，例如「是否允许 Notify」
```

类比：

- **Service** = 一个 App 模块（如「灯效控制」）
- **Characteristic** = 模块里的一个字段（如「当前命令」）
- **Descriptor（CCCD）** = 「要不要给我推送更新？」的开关

### 3.5 特征值的四种常见操作

| 操作 | 方向 | 含义 | 本 Demo |
|------|------|------|---------|
| Read | 手机 ← ESP32 | 手机主动读一次 | 读到默认 `"hello ble"` 或上次写入的内容 |
| Write | 手机 → ESP32 | 手机写数据给设备 | 写 1 字节切换灯效，或写字符串存盘 |
| Notify | 手机 ← ESP32（订阅后） | 设备有变化就主动推 | 每秒推 4 字节递增计数 |
| Indicate | 同 Notify，但需确认 | 可靠性更高 | 本 Demo 未使用 |

**Notify 与 Read 的区别：**

- Read：你问一次，设备答一次。
- Notify：你订阅后，设备可以 **反复主动推送**，不用手机每次来读。

### 3.6 UUID 是什么？

UUID 是服务和特征值的 **唯一编号**（像门牌号）。

- **16 位 UUID**（如 `0xFFF0`）：自定义 demo 常用，简短。
- **128 位 UUID**：蓝牙联盟标准服务（如心率 `0x180D`）或产品级协议。

本 Demo 使用自定义 16 位 UUID，仅用于演示，与蓝牙联盟标准服务无冲突约定。

### 3.7 广播 vs 连接

| 阶段 | 是否连接 | 能做什么 |
|------|----------|----------|
| 仅广播 | 否 | 被扫描到、看到设备名 |
| 已连接 | 是 | 发现服务、读写特征值、Notify |

---

## 四、本工程的通信协议说明

### 4.1 设备标识

| 项目 | 值 |
|------|-----|
| 广播名称 | `ESP32-S3-BLE-Demo` |
| 服务 UUID | `0xFFF0`（16 位） |
| 特征值 UUID | `0xFFF1`（16 位） |
| 特征值属性 | Read、Write、Notify |
| 最大写入长度 | 20 字节 |

### 4.2 特征值 `0xFFF1` 数据格式

#### A. 读取（Read）

- 返回当前缓冲区内容，**原始字节**，无额外帧头。
- 出厂默认：ASCII 字符串 `hello ble`（9 字节）。
- 若曾 Write 过，则返回最后一次写入的内容（最多 20 字节）。

#### B. 写入（Write）— 控制 RGB 灯效

写入 **1 字节 ASCII 字符** 时，切换预设灯效：

| 写入字符 | HEX | 灯效 |
|----------|-----|------|
| `0` | `0x30` | 上电黄呼吸（BOOTUP） |
| `1` | `0x31` | 蓝青律动，等待连接（PAIRING） |
| `2` | `0x32` | 黄灯闪 5 次（TRIGGER） |
| `3` | `0x33` | 绿色常亮约 5 s（SUCCESS） |
| `4` | `0x34` | 彩虹渐变（WORKING） |
| `5` | `0x35` | 红色快闪（ERROR） |

- 写入 **2 字节及以上** 任意内容：触发黄灯闪 5 次（TRIGGER）。
- 同时，写入内容会保存到特征值缓冲区（Read 可读回）。

**nRF Connect 建议：** Write 类型选 **Text**，输入单个字符如 `4`；或选 **BYTE ARRAY** 发送 `34`（即 `'4'` 的 ASCII）。

#### C. 通知（Notify）

1. 手机对 `0xFFF1` 开启 Notify（App 会自动写 CCCD，用户无需手填）。
2. 设备 **每 1 秒** 推送 **4 字节**，小端序无符号 32 位整数，从 0 递增：

```
字节序:  [0] [1] [2] [3]
含义:    计数低字节 → 高字节（Little-Endian）

示例:
  00 00 00 00  → 0
  01 00 00 00  → 1
  02 00 00 00  → 2
  ...
```

### 4.3 连接状态与灯效（自动，无需写入）

| 状态 | RGB 灯效 |
|------|----------|
| 上电约 2.5 s | 黄色呼吸 |
| 广播中、等待连接 | 蓝青律动 |
| 手机已连接 | 彩虹渐变 |
| 断开连接 | 回到蓝青律动 |

> 连接成功后会自动进入彩虹灯效。若要用 Write 测试其他灯效，请在连接状态下向 `0xFFF1` 写入 `'0'`～`'5'`。

### 4.4 协议示意图

```
手机 (Central)                          ESP32 (Peripheral)
      │                                        │
      │──── Scan / Connect ───────────────────►│
      │                                        │ 灯: 彩虹 (WORKING)
      │◄─── 发现 Service 0xFFF0 ─────────────│
      │◄─── 发现 Char 0xFFF1 ────────────────│
      │                                        │
      │──── Read 0xFFF1 ──────────────────────►│
      │◄─── "hello ble" (9 bytes) ────────────│
      │                                        │
      │──── Write 0xFFF1: '2' ────────────────►│
      │                                        │ 灯: 黄闪 (TRIGGER)
      │                                        │
      │──── Enable Notify (CCCD) ─────────────►│
      │◄─── 01 00 00 00 (每秒) ───────────────│
      │◄─── 02 00 00 00 ──────────────────────│
      │                                        │
      │──── Disconnect ───────────────────────►│
      │                                        │ 灯: 蓝青律动 (PAIRING)
```

---

## 五、构建、烧录与测试

### 5.1 编译

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_ble_demo.ps1
```

首次会自动 `set-target esp32s3`。

### 5.2 烧录与串口监视

```powershell
idf.py -C ble_demo -p COMx flash monitor
```

将 `COMx` 换成实际端口。正常日志示例：

```
I ble_led: bootup scene started (GPIO48 WS2812 x16)
I ble_gatt: Advertising started
I ble_demo: BLE ready — scan for device name "ESP32-S3-BLE-Demo"
I ble_gatt: Connected, conn_id=..., remote xx:xx:xx:xx:xx:xx
I ble_led: working scene (rainbow, connected)
```

### 5.3 手机测试（nRF Connect）

1. 安装 **nRF Connect**（Nordic Semiconductor，Android / iOS）。
2. **SCAN** → 点 **ESP32-S3-BLE-Demo** → **CONNECT**。
3. 展开 **Unknown Service** 或 UUID **0xFFF0**。
4. 对 **0xFFF1**：
   - **Read**：查看当前字符串。
   - **Write (Text)**：输入 `2` 测黄闪。
   - 点 **Notify** 图标（三条横线）：观察每秒递增的 4 字节 HEX。

---

## 六、目录结构

```
ble_demo/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md              ← 本文档
├── README.txt             ← 简要速查
└── main/
    ├── main.c             # 入口：NVS、RGB、BLE 启动
    ├── ble_gatt_server.c  # GATT 服务、广播、Notify
    ├── ble_gatt_server.h
    ├── ble_led.c          # RGB 灯效（复用 common/led_scene）
    └── ble_led.h
```

---

## 七、名词速查

| 名词 | 一句话解释 |
|------|------------|
| BLE | 低功耗蓝牙，适合传感器、遥控、配网 |
| GATT | BLE 上读写数据的「服务 / 特征值」模型 |
| Service | 一组相关功能的容器 |
| Characteristic | 实际读写的数据点 |
| UUID | 服务/特征值的编号 |
| Advertising | 外设对外「喊名字」，供扫描 |
| Central / Peripheral | 中心（手机）/ 外设（ESP32） |
| CCCD | Notify/Indicate 订阅开关 |
| MTU | 单次传输最大 payload，本工程设为 517 |
| Notify | 设备主动推送，无需手机反复 Read |

---

## 八、延伸学习建议

1. 先在本 Demo 上练熟：Scan → Connect → Read → Write → Notify。
2. 阅读 `ble_gatt_server.c` 中的 `ESP_GATTS_*` 事件，理解「何时创建服务、何时开始广播」。
3. 尝试增加第二个特征值（例如只读「固件版本」）。
4. 将 Notify 的 4 字节改成真实传感器数据（温度、ADC 等）。
5. 官方示例路径（本仓库 ESP-IDF 内）：
   `Espressif/frameworks/esp-idf-v5.5.4/examples/bluetooth/ble_get_started/bluedroid/Bluedroid_GATT_Server`

---

## 九、常见问题

**Q：手机搜不到设备？**  
A：确认固件已烧录、串口有 `Advertising started`；检查手机蓝牙和定位权限（Android 扫描 BLE 常需定位权限）。

**Q：连上后找不到 0xFFF0？**  
A：等待 GATT 服务发现完成；在 nRF Connect 里下拉刷新。

**Q：Write 了但灯没变？**  
A：确认写的是 **1 字节** `'0'`～`'5'`；连接后默认彩虹会覆盖，Write `'2'` 等可强制切换。

**Q：Notify 没有数据？**  
A：必须点击特征值旁的 **Notify 开关**；未订阅时设备不会推送。

**Q：能否用微信小程序 / 自己 App 连？**  
A：可以。微信小程序使用 `wx.createBLEConnection` 等 API，UUID 填 `FFF0` / `FFF1` 即可（需按平台要求格式化为 128 位 UUID 字符串）。
