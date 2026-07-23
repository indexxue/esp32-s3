# camera：钢珠检测 + 云台粗跟 开发计划

状态：图传方案已确认（A+B + 客户端叠框）；其余项待评审落地  
范围：`camera` 工程（ESP32-S3 N16R8）  
日期：2026-07-23

---

## 1. 结论摘要

| 议题 | 决定 |
|------|------|
| PC 先做再迁片上 | **可行**（见 §2）；PC 验证算法与数据，片上用 ESP-DL，**不**整库移植 OpenCV |
| 识别目标 | 钢珠 / 球形物品；类别暂定 **10** |
| 云台轴 | PWM1(GPIO46)=偏航 Pan；PWM2(GPIO3)=俯仰 Tilt（见 §4） |
| 控制 | 检测框中心 → 画面中心偏差 → 粗跟（死区 + 限速） |
| 图传 | **已确认**：SoftAP + **A（JPEG 轮询）+ B（`/api/detect/latest`）+ 客户端叠框**；本迭代不上 MJPEG/服务端画框（见 §6.4） |
| 输出 | 串口/LCD/网页图传；同一结果结构体预留为后续 SPI/其它设备接口 |
| SPI1 | 仅占脚，本阶段不开发协议 |
| 供电 | 已解决（外供稳定电源，ESP 与舵机共地） |

---

## 2. PC → ESP32 路径是否可行

### 2.1 结论

**可行，且推荐作为主路径**，但角色要分清：

| 阶段 | 跑在哪 | 做什么 | 不做什么 |
|------|--------|--------|----------|
| A. PC 原型 | PC + 摄像头/离线图 | 采数、标定、试检、调跟踪律、定坐标约定 | 不假设 OpenCV API 能原样上板 |
| B. 模型迁移 | PC 训练 → 导出 | 得到可上板的检测模型（ESP-DL / ESPDet 类） | 不把完整 OpenCV 链搬进固件 |
| C. 片上集成 | ESP32-S3 `camera` | 推理 + 坐标输出 + 双舵机粗跟 + Web 图传 | SPI1 业务协议延后 |

原因简述：

1. **算法可迁移**：检测框 → `(cx, cy)` → 误差 → 舵机增量，控制律与平台无关，PC 上调好死区/增益即可搬到固件。
2. **视觉栈不可原样移植**：ESP32-S3 实时路径应走现有 **ESP-DL**（工程内已有 `camera_model` / CatDetect），不是 OpenCV DNN。
3. **钢珠反光强**：PC 上可先用经典法（Hough / 颜色）摸边界，量产更稳的是 **监督学习检测**（10 类）；PC 采数与标注是关键路径。

### 2.2 推荐工作流

```text
PC 采图/标注 (10 类钢珠)
    → PC OpenCV/YOLO 等验证召回与坐标约定
    → 导出/量化为 ESP-DL 可部署模型
    → 替换 camera_model 中的模型与类别表
    → 片上：detect → track → servo + 图传
```

### 2.3 PC 阶段交付物（建议）

- 数据集目录约定（类别 id 0..9、命名、分辨率）
- 坐标约定文档：帧尺寸、原点、`xywh` / `cxcy`（与片上一致）
- 跟踪参数表：死区、最大步进角/帧、中位角、软限位
- （可选）PC 用 USB 串口读板端 `printf`，或板端固定假目标测云台
- （可选）PC 浏览器连 SoftAP，用图传核对检测框与云台动作

---

## 3. 产品目标（本迭代）

1. **检测**：对画面中钢珠给出类别 + 框 + 中心坐标（最多多目标；跟踪默认跟「主目标」）。
2. **云台粗跟**：主目标中心拉向画面中心；有死区，避免抖动。
3. **图传（已确认）**：SoftAP 网页 JPEG 轮询预览 + `/api/detect/latest`；**客户端叠框**（不在板端画进 JPEG）。
4. **人读输出**：串口文本 + LCD 画框/状态行 + 网页图传。
5. **接口预留**：稳定 C 结构体 / getter，供后续 SPI1 或其它模块调用。
6. **非目标**：SPI1 协议、精跟 PID 调优、RTSP/WebRTC、SD 存图（除非后续单独立项）。

---

## 4. 硬件与引脚

来源：`camera/README.txt`；落地时写入 `common/inc/board.h` 的 `BOARD_PROFILE_CAMERA`。

| 功能 | 引脚 | 说明 |
|------|------|------|
| 舵机 PWM1（Pan 偏航） | GPIO46 | 水平扫视；行程通常更大 |
| 舵机 PWM2（Tilt 俯仰） | GPIO3 | 俯仰；注意机械防撞 |
| SPI1 预留 | SCK=21, MOSI=47, MISO=45, CS=14 | 仅宏定义，不 init |
| ST7789 | SPI2 既有 | 预览 + 框 |
| OV2640 | DVP + I2C 既有 | 检测输入帧 |
| WS2812 | GPIO48 | 可选状态灯 |
| 按键 | GPIO0 | 可选：锁定跟踪 / 回中 |

### 4.1 舵机（MG996R）

- 信号：LEDC，约 **50 Hz**；脉宽约 **500–2500 µs** 映射角度（以实测标定为准）。
- 驱动：复用 `bsp_driver` 的 `pwm`（LEDC），上层 `servo_*` 做角度/脉宽换算。
- 上电：先中位（约 1500 µs），再允许跟踪输出。
- 软限位：在 `board`/配置中写死 `PAN/TILT_MIN_DEG` / `MAX_DEG`，防止撞结构。

### 4.2 轴定义（本计划固定）

```text
画面坐标：原点左上，x 向右，y 向下（与现有 camera_detection 一致）
误差：    err_x = cx - frame_w/2
          err_y = cy - frame_h/2

PWM1 / GPIO46 / Pan ：err_x > 0 → 云台向右转（使目标回到中心；若实物反向则改符号宏）
PWM2 / GPIO3  / Tilt：err_y > 0 → 云台向下俯（同上，用 SERVO_TILT_SIGN 可翻转）
```

符号用宏 `SERVO_PAN_SIGN` / `SERVO_TILT_SIGN`（±1），上板后一次标定即可，不必改算法。

---

## 5. 软件架构

```text
camera/main
├── start.c                 生命周期；挂检测+跟踪任务；拉起 web_ctrl
├── source/
│   ├── camera_sensor.*     已有：采帧/LCD 预览/JPEG 编码
│   ├── camera_model.*      演进：钢珠模型（现猫模型作通路）
│   ├── camera_ui.*         已有：LCD 画框/状态
│   ├── web_pages.*         已有/演进：图传 HTTP API + 网页
│   ├── detect_out.*        新增：人读输出 + get_latest / callback
│   ├── gimbal_track.*      新增：中心粗跟（读检测 → 写舵机）
│   └── servo_ctrl.*        新增：双路 MG996R 接口
bsp_driver/pwm              已有 LEDC
common/inc/board.h          补舵机/SPI1 宏
web_ctrl / SoftAP           已有：OTA + 图传承载
SPI1                        本阶段不实现
```

```text
OV2640 → snapshot(RGB565)
           ├─→ LCD 预览 + 画框
           ├─→ camera_model → detect_out → gimbal_track → servo
           └─→ JPEG 编码 → HTTP 图传（网页 / 客户端）
                              └─→ /api/detect/latest（JSON，可选）
```

依赖方向：`main` → `servo_ctrl` → `pwm`；`gimbal_track` → `detect_out` + `servo_ctrl`；`web_pages` → `camera_sensor` + `detect_out`（只读）；不反向依赖。

---

## 6. 接口约定（草案）

### 6.1 检测结果（人读 + 后续设备共用）

在现有 `camera_detection_t` 上扩展或并行提供中心点（实现时二选一，优先少破坏）：

```c
typedef struct {
    uint16_t x, y, w, h;   /* 框，相对摄像头帧 */
    uint16_t cx, cy;       /* 中心 = (x+w/2, y+h/2) */
    uint8_t  class_id;     /* 0..9，钢珠类别 */
    float    confidence;
} camera_detection_t;

typedef struct {
    camera_detection_t boxes[CAMERA_MODEL_MAX_BOXES];
    uint8_t  count;
    int8_t   primary_idx;  /* 粗跟主目标下标；无则 -1 */
    uint32_t frame_id;
    uint32_t elapsed_ms;
} camera_detection_result_t;

const camera_detection_result_t *camera_detect_get_latest(void);
status_t camera_detect_set_callback(camera_detect_cb_t cb, void *ctx);
```

串口人读示例：

```text
[DETECT] frame=12 boxes=2 time=85ms primary=0
  #0: cls=3 conf=0.91 xywh=(40,50,30,30) cxy=(55,65)
  #1: cls=1 conf=0.72 xywh=(180,90,28,28) cxy=(194,104)
[GIMBAL] err=(-12,8) pan=93.0 tilt=88.5
```

### 6.2 舵机

```c
typedef enum { SERVO_CH_PAN = 0, SERVO_CH_TILT = 1 } servo_ch_t;

status_t servo_init(void);
status_t servo_set_angle(servo_ch_t ch, float deg);
status_t servo_set_pulse_us(servo_ch_t ch, uint16_t us);
status_t servo_get_angle(servo_ch_t ch, float *deg);
status_t servo_center_all(void);
```

### 6.3 粗跟

```c
typedef struct {
    uint16_t deadzone_px;     /* 中心死区，建议 8~20 */
    float    k_pan_deg_per_px;
    float    k_tilt_deg_per_px;
    float    max_step_deg;    /* 每控制周期最大转角 */
    float    pan_min_deg, pan_max_deg;
    float    tilt_min_deg, tilt_max_deg;
    bool_t   enable;
} gimbal_track_cfg_t;

status_t gimbal_track_init(const gimbal_track_cfg_t *cfg);
void     gimbal_track_set_enable(bool_t on);
void     gimbal_track_on_detection(const camera_detection_result_t *res);
```

主目标选择（初版）：`confidence` 最高且 `class_id` 在允许表内；后续可改为「最靠近中心」或指定类别。

控制律（初版，比例 + 限幅，非精 PID）：

```text
if |err| < deadzone: 不动作
else: delta = clamp(k * err * SIGN, ±max_step)
      angle = clamp(angle + delta, soft_limit)
```

### 6.4 图传（SoftAP + HTTP）

#### 现状（工程已有，作为基线）

| 项 | 现状 |
|----|------|
| 承载 | `web_ctrl` SoftAP + `httpd` |
| 单帧 JPEG | `GET /api/camera/camera.jpg`（`camera_sensor_snapshot_jpeg`） |
| 网页轮询预览 | `source/www/index.html` 定时 `fetch` JPEG |
| 状态 | `GET /api/camera/status` |
| 本地屏 | ST7789 同步预览（与图传同源 snapshot） |

README 旧描述「无 web streaming」已过时；本计划以 **网页图传** 为正式能力。

#### 本迭代目标

1. **给人看**：浏览器打开设备 AP 页即可看画面（沿用/加强现有轮询预览）。
2. **可核对检测**：网页或旁路 API 能看到框 / 主目标（便于调阈值与粗跟，无需只盯串口）。
3. **资源可控**：图传不得拖垮检测与舵机控制；可降帧、降 JPEG 质量、关流。

#### 方案分档（按实现成本递增）

| 档 | 内容 | 本迭代 |
|----|------|--------|
| **A** | 稳定 JPEG 轮询预览 + 开关 | **必做** |
| **B** | `GET /api/detect/latest` JSON + 网页/PC **客户端叠框** | **必做** |
| **C** | multipart MJPEG `GET /api/camera/stream` | **不做**（A/B 不够用再单独立项） |
| **D** | RTSP / WebRTC / 原始 RGB565 UDP | **不做** |

落地顺序：**只做 A → B**。

#### 叠框策略（已确认）

| 策略 | 本迭代 |
|------|--------|
| **客户端叠框（JSON + 原图 JPEG）** | **采用**：板端轻；框样式在网页/PC 改；坐标与 JPEG 分辨率须一致（同源 snapshot） |
| 服务端叠框再 JPEG | **不做** |

实现要点：网页定时并行拉取 `camera.jpg` 与 `detect/latest`，用 canvas 按 `xywh`/`cxcy` 画框；主目标可加粗/变色。

#### API 草案（B 档）

```http
GET /api/camera/camera.jpg     → image/jpeg
GET /api/camera/status         → { w, h, streaming, ... }
GET /api/detect/latest         → application/json
POST /api/camera/stream_enable → { enable: true|false }   # 可选：停图传省算力
```

JSON 示例（与串口字段对齐，便于后续 SPI 复用语义）：

```json
{
  "frame_id": 12,
  "elapsed_ms": 85,
  "w": 240,
  "h": 240,
  "primary_idx": 0,
  "boxes": [
    { "cls": 3, "conf": 0.91, "x": 40, "y": 50, "w": 30, "h": 30, "cx": 55, "cy": 65 }
  ],
  "gimbal": { "pan_deg": 93.0, "tilt_deg": 88.5, "err_x": -12, "err_y": 8 }
}
```

#### 与检测/云台的调度约定

- JPEG 编码与 `camera_model_detect` **勿长时间互斥同一大锁**；snapshot 已有拷贝路径则复用。
- 图传目标帧率：**≤ 检测帧率**（例如检测 5–10 Hz 时，网页 2–5 fps 足够联调）。
- SoftAP 仅 1～2 个客户端；不做多路高清。
- 关闭图传（网页按钮 / API）后应停止 JPEG 编码，把 CPU 留给推理与粗跟。

---

## 7. 检测与 10 类钢珠

### 7.1 短期（打通链路）

- 继续用现有 ESPDet 猫模型验证：预览 → 推理 → 串口坐标 →（可用假 `cx,cy`）测云台。
- PC 同步开始钢珠采数与标注。

### 7.2 中期（替换模型）

- 类别：`0..9`，名称表放固件常量（如 `steel_ball_cls_name[]`），串口打印名字。
- 输入尺寸尽量贴近现有 **224×224** ESPDet 管线，减少改预处理。
- 反光/高光：训练需覆盖多角度光照；必要时 PC 上做简单增广验证。

### 7.3 PC OpenCV 可先试的内容

- 圆形检测基线（评估反光失败率）
- ROI / 曝光建议，反馈给板端摄像头配置
- 跟踪律仿真（用录屏坐标驱动虚拟云台角）

---

## 8. 分阶段落地

### Phase 0 — 板级与文档对齐（0.5d）

- [ ] `board.h`：`SERVO_PAN/TILT`、SPI1 预留宏
- [ ] `README.txt` 与本文轴定义一致
- [ ] SPI1：**不**调用 `spi_bus_initialize`

### Phase 1 — 舵机接口 + 手动联调（1–2d）

- [ ] `servo_ctrl` + LEDC 50 Hz 双通道
- [ ] 上电中位、软限位、符号宏
- [ ] 串口命令或按键：`pan±` / `tilt±` / `center`（命令形式实现时再定）

### Phase 2 — 检测输出收口（1d）

- [ ] 结果带 `cx,cy`、`primary_idx`
- [ ] `get_latest` + 串口人读格式固定
- [ ] LCD 画主目标框（可与现逻辑合并）

### Phase 3 — 中心粗跟（1–2d）

- [ ] `gimbal_track`：死区 + 比例限幅
- [ ] 与 `camera_modules_task` 同任务或紧耦合调用（避免多任务抢舵机）
- [ ] 参数先写死常量，预留 NVS 后续再做

### Phase 3.5 — 图传收口（A+B + 客户端叠框，1–2d，可与 Phase 2/3 交错）

- [ ] **A**：确认 SoftAP 网页 JPEG 预览稳定；文档写明 AP SSID/URL（沿用 `web_ctrl` 配置）
- [ ] **A**：预览开关真正停/启 JPEG 编码，避免与检测抢 CPU
- [ ] **B**：`GET /api/detect/latest`；网页 canvas **客户端叠框** + 显示主目标/云台角（可选）
- [ ] 更新 `README.txt`：删除「无 web streaming」表述，改为图传说明（A+B，非 MJPEG）
- [ ] ~~MJPEG~~ 本迭代明确不做

### Phase 4 — PC 数据与模型替换（并行，周期视采数）

- [ ] PC 数据集 + 10 类训练/导出
- [ ] 替换 `camera_model` 模型文件与类别
- [ ] 回归：坐标、粗跟、人读输出、**图传客户端叠框**

### Phase 5 — 预留接口冻结（0.5d）

- [ ] 冻结 `camera_detection_result_t` 布局、串口行格式、`/api/detect/latest` JSON（后续 SPI 载荷原型）
- [ ] 文档注明：SPI1 协议另开计划

---

## 9. 风险与对策

| 风险 | 对策 |
|------|------|
| 钢珠镜面反光导致漏检 | PC 多光照采数；必要时补灯；别死磕纯 Hough |
| 检测帧率低导致跟丢 | 粗跟限步长；降输入分辨率或隔帧推理；预览与推理可降频 |
| **图传占满 CPU/带宽** | 降 JPEG 质量与刷新率；可关流；**客户端叠框**（已确认） |
| **WiFi SoftAP 与检测抢核** | 图传任务低优先级；单客户端；与推理错峰 |
| JPEG 与 detect JSON 帧不同步 | 带同一 `frame_id`；网页容忍 mild 错位；刷新率 ≤ 检测 |
| 舵机方向与画面相反 | `SERVO_*_SIGN` 宏一次翻转 |
| OpenCV 方案无法上板 | 明确 PC 只验证；量产模型走 ESP-DL |
| 双任务写 PWM | 跟踪只在单一任务写 `servo_set_*` |
| 10 类小样本过拟合 | 先保证「有球/类别粗分」，再提细分类 |

---

## 10. 明确不做（本计划）

- SPI1 与外部设备的帧协议、主从、波特/时钟
- 精跟（全状态 PID / 卡尔曼）——粗跟稳定后再开
- 真实 OpenCV 链入 ESP-IDF 固件
- **MJPEG / 服务端 JPEG 叠框** / RTSP / WebRTC / 多客户端高清图传
- 未评审通过前的大规模目录重构

---

## 11. 评审检查清单

已确认：

- [x] **图传** = A（JPEG 轮询）+ B（detect JSON）+ **客户端叠框**；不上 MJPEG / 服务端画框

仍请确认或改口：

1. PC 先验证、片上 ESP-DL —— 是否按 §2 执行？
2. Pan=GPIO46、Tilt=GPIO3 —— 是否接受？机械装反时只改符号宏。
3. 主目标 = 最高置信度 —— 是否改为「指定 class」或「最近中心」？
4. 粗跟参数初值是否由固件常量起步（不做 NVS）？
5. 10 类名称表是否已有业务名，还是先用 `cls0..cls9`？

评审通过后，按 Phase 0 → 3 / 3.5 开工；Phase 4 与 PC 采数并行。
