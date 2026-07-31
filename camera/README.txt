camera — ESP32-S3 摄像头预览 + 钢珠检测 + 云台

外设：ST7789 240×135、OV2640（LCD + SoftAP/STA 网页预览/抓拍 + Wi‑Fi 配网）、
双 MG996R 舵机（Pan/Tilt）、板端 ESPDet 钢珠单类检测（LCD 绿框）。

构建 / 烧录见文末「工程命令」。钢珠从采数到上板的完整流程见下文「钢珠检测流水线」。

================================================================================
钢珠检测流水线（采数 → 训练 → 量化 → 固件）
================================================================================

目标：单类 `ball`（class_id=0），输入 224×224，板端格式 `.espdl`（ESP-DL / ESPDet-Pico）。
坐标约定：画面原点左上，x 向右，y 向下；标签为 YOLO 归一化 `cx cy w h`。

---- 1. 板端采数 + PC 标注 ----

工具目录：`camera/tools/steel_ball_annotate/`

1) 板端烧录本工程，手机/PC 连 SoftAP（默认 `ESP32-WebCtrl` / `esp32web1`），打开
   `http://192.168.4.1/`。
2) PC 安装依赖并启动标注工具：
     cd camera/tools/steel_ball_annotate
     pip install -r requirements.txt
     python annotate_app.py
3) 在工具中填写板端 URL，抓帧并只标 class 0（钢珠）。删废图时同步删标签。
4) 用工具「打包单类」生成 `steel_ball_1cls_*.zip`（内含 train/val 拆分 + 单类 data.yaml）。

建议数据量：冒烟 ≥50 张；可看召回约 150～300 张。覆盖正光/侧光/反光、远近、画面四角。
标注数据默认不入库（见根目录 `.gitignore`）。

目录约定（打包后）：
  steel_ball_1cls/
  ├── data.yaml          # nc:1, names: {0: ball}
  ├── classes.txt        # 一行: ball
  ├── images/train|val/
  └── labels/train|val/  # 与图片同 stem 的 .txt

---- 2. PC 基线验证（可选，YOLOv8） ----

可用 Ultralytics 在云端/本机先训 YOLOv8n（imgsz=224）得到 `best.pt`，用
`validate.py` 看框与中心是否靠谱。

说明：Ultralytics 默认导出的 `best.onnx` **不能直接上 ESP32**（Detect 头与 ESP-DL
后处理不兼容）。板端必须以 ESPDet + `.espdl` 为准；YOLOv8 权重仅作 PC 对照。

---- 3. ESPDet 训练 + 量化（用户在 WSL 自行执行） ----

完整命令与排错见：`camera/tools/esp-detection/过程文档.md`  
（训练/量化由用户在终端跑；下文为摘要。）

目录：`camera/tools/esp-detection/`（已在本仓库 tools 下）。

1) 解压单类 zip → `datasets/steel_ball_1cls/`；val 图拷到 `deploy/ball_calib/`。
2) 确认 `cfg/datasets/steel_ball.yaml`（path=datasets/steel_ball_1cls，names 0:ball）。
3) 按需改 `train.py`：`epochs=200` / `batch=8` / `device=cpu`。
4) 训练：
     conda activate espdet
     python espdet_run.py \
       --class_name ball --pretrained_path None \
       --dataset "cfg/datasets/steel_ball.yaml" \
       --size 224 224 --target "esp32s3" \
       --calib_data "deploy/ball_calib" \
       --espdl "espdet_pico_224_224_ball.espdl" \
       --img "ball_test.jpg"
5) 若末尾 TypeError：可忽略。以日志中的 run 目录为准（可能是 train / train-2…），
   对已生成的 best.onnx 手动量化：
     python /tmp/quant_ball.py   # 脚本内容见 过程文档.md §4
6) 产物在 esp-detection 根目录：`espdet_pico_224_224_ball.espdl`（约数百 KB）。
   CPU 量化可能数分钟无输出，属正常。

---- 4. 带日期导入固件并编译 ----

量化产物默认：`camera/tools/esp-detection/espdet_pico_224_224_ball.espdl`

固件槽位文件名固定（CMake / 加载名勿改）：
  camera/components/ball_detect/models/s3/espdet_pico_224_224_ball.espdl

每次训完用导入脚本（会打日期戳 + 写 boot 日志宏）：
  py -3 camera\tools\promote_ball_model.py --note "train-2 mAP50=0.943"

脚本会：
  1) 归档带日期副本：
       models/s3/archive/espdet_pico_224_224_ball_YYYYMMDD_HHMMSS.espdl
  2) 覆盖上述固定文件名（编入 flash rodata 用）
  3) 更新 components/ball_detect/model_stamp.h
       BALL_DETECT_MODEL_STAMP / NOTE / ARCHIVE

然后编译烧录（仓库根目录）：
  idf -Project camera build
  idf -Project camera -p PORT flash monitor

验收：
  串口 `cam_model init ok (espdet ball stamp=YYYYMMDD_HHMMSS note=...)`
  以及 `infer ... boxes=N`；LCD / 网页 MJPEG 绿框（由宏开关）。
  若板上仍像旧模型：删 camera\build 后重编。

详情：`camera/tools/esp-detection/过程文档.md` §5（本地训练树内；本 README 为准入门）。

关键源码：
  components/ball_detect/     ESPDet 封装 + models/s3/*.espdl
  main/source/camera_model.*  检测任务与结果缓存；叠框宏 CAMERA_DETECT_OVERLAY_LCD/WEB
  main/source/camera_ui.*     LCD 画框 / RGB565 叠框 / lcd_close·open
  main/source/camera_sensor.* 预览；blit 后叠框；JPEG 编码前叠框

叠框宏（camera_model.h，改后重编）：
  CAMERA_DETECT_OVERLAY_LCD  1=LCD 画框，0=关
  CAMERA_DETECT_OVERLAY_WEB  1=网页 MJPEG/JPEG 画框，0=关
  可单独开、全开或全关。

应用模式（camera/main/CMakeLists.txt，改后重编；COLLECT 与 CALIB 不可同时为 1）：
  CAMERA_APP_COLLECT_MODE=1  采数：不启 ESPDet，SoftAP JPEG 给 steel_ball_annotate
  CAMERA_APP_CALIB_MODE=1    单舵机校准：不启 ESPDet；网页「平衡杠校准」（已去掉双轴云台 UI）
  二者均为 0               识别：启检测与叠框
  当前仓库默认 CALIB=1（校准固件）。量产识别请改回 COLLECT=0、CALIB=0。
  校准固件：不注册 MJPEG/预览页，默认暂停图传，httpd LRU 可踢长连接，专供舵机网页调试。
  采数时请关浏览器 /preview.html，再用 annotate 工具 Connect 抓帧。

---- 单舵机校准固件（CAMERA_APP_CALIB_MODE）----

用途：单边平衡杠左-中-右姿态标定（默认通道 Pan/GPIO46，可切 Tilt/GPIO3）；参数落 NVS。

1) CMakeLists.txt：`set(CAMERA_APP_CALIB_MODE 1)`（COLLECT 保持 0），重编烧录：
     idf -Project camera build
     idf -Project camera -p PORT flash monitor
2) 网页：看串口日志 `calib web open: http://x.x.x.x/` —— 已连家里 WiFi 时是 STA IP，
   不是 192.168.4.1。手机/电脑须与板子同一网段再打开该 URL。
3) 串口（USB 监视器，可先验证 PWM，不依赖网页）：
     help
     servo                 # 查 pan/tilt 状态
     servo pan 180         # 角度
     servo pan pulse 1500  # 脉宽 µs
     servo pan nudge -2    # 相对微调 °
     servo tilt 90
     servo center          # 回中位
4) 网页：按 左 → 中 → 右：调角度/脉宽 →「捕获当前」→ 可选 offset →「写入 NVS」。
5) 板键 GPIO0 长按：当前校准通道回 NVS 中位（无则 180°）。
6) 校准完成后改回 `CAMERA_APP_CALIB_MODE 0` 编识别固件；NVS 键 `servo_cal` 保留。
   识别固件：平衡律（球偏左打右；离中越远纠偏越大）；出画回中等待。
   方向：`SERVO_BALL_FOLLOW_INVERT`（0=平衡默认，1=取反）。行程用 NVS L/C/R。

   读校准并自检（判断 L/C/R 是否统一）：
     串口：servo cal
     网页/脚本：GET /api/servo/calib → check.order / check.hint
       py -3 camera/tools/dump_servo_cal.py --url http://<板IP>
     order 期望 L<C<R；non_monotonic 必须重标。
     标定约定：球在杠物理左端时捕获 Left，中间 Center，右端 Right（勿对调）。
   量产侧亦可：`nvs_servo_calib_get()` + `servo_apply_pose()`（见 common/inc/nvs.h）。

API：GET /api/app_mode → mode=calib|collect|detect
     GET/POST /api/servo/calib
       capture/set_offset/goto/nudge/set_angle/set_pulse/save_pose/save/load/reset/set_channel

LCD 关闭：`camera_ui_lcd_close()` 黑屏+关背光+停 blit；`camera_ui_lcd_open()` 恢复。

sdkconfig（见 sdkconfig.defaults）：
  CONFIG_FLASH_ESPDET_PICO_224_224_BALL=y
  CONFIG_BALL_DETECT_MODEL_IN_FLASH_RODATA=y

---- 5. 当前能力与后续 ----

已完成：SoftAP 采数标注、ESPDet 训量化、固件推理、LCD/网页绿框（宏开关）、LCD 关闭接口。
性能（识别模式）：DVP 双缓冲；JPEG 异步任务；MJPEG 在线约 15fps 图传并关 LCD；
  检测周期下限 80ms（跑完即下一帧）；权重优先拷入 PSRAM（`param_copy`）。
后续可选：云台按框中心粗跟、补困难样本再训。
SPI 主机对外通信（与小车 TM4C）：双方公共协议 `camera/plan/camera-spi-protocol.md`（v1.8）。
  固件：SPI3 Master Mode1 / 1 MHz / 20 ms HEARTBEAT（`camera_spi_host` + `spi_link`）；
  引脚 SCK=21 逻辑MOSI=45 逻辑MISO=47 CS=14（软件对调）。
  链路 DOWN/抖动时只发 HEARTBEAT；连续 8 帧合法 HB 后才开 SERVO/DETECT。

舵机轴：PWM1=GPIO46 Pan；PWM2=GPIO3 Tilt。画面 err_x>0 → pan 右；err_y>0 → tilt 下。
装反时改 `BOARD_SERVO_PAN_SIGN` / `BOARD_SERVO_TILT_SIGN`（`common/inc/board.h`）。
角度：0–360° ↔ 500–2500 µs，中位 180°=1500 µs。默认软限位 Pan 0–360、Tilt 60–300（约 240°）。
软限位可运行时改（不写 NVS，重启恢复 board.h 默认）。

================================================================================
工程命令
================================================================================

Build:
  powershell -ExecutionPolicy Bypass -File .\scripts\build_camera.ps1
  or: idf -Project camera build

Clean rebuild:
  Remove-Item camera\sdkconfig, camera\build -Recurse -Force
  idf -Project camera build

Flash & monitor:
  idf -Project camera -p PORT flash monitor

Web（加入 SoftAP ESP32-WebCtrl / esp32web1 后）:
  http://192.168.4.1/            预览 + 配网 + 云台按键
  http://192.168.4.1/ota         OTA 上传
  预览：JPEG 轮询 /api/camera/camera.jpg ；可选 MJPEG /api/camera/stream.mjpg
  舵机：GET /api/servo/status ；POST /api/servo
        {"center":1} | {"pan_delta":5} | {"tilt":180}
        {"pan_min":0,"pan_max":360,"tilt_min":60,"tilt_max":300}  改软限位
        {"limits_reset":1}                                       恢复默认限位
        {"pan":0} / {"tilt":300}                                 测极限角
        {"pan_pulse":500} / {"tilt_pulse":2500}                  原始脉宽（绕过角度软限位）
  网页 D-pad：点按一步、长按连续；可改限位并一键到 min/max。
  长按期间暂停 MJPEG，松手约 0.7s 后恢复。板键 GPIO0：长按回中；双击清除已存 Wi‑Fi 并重启进 SoftAP 配网。无 USB 串口舵机命令。

Pins: see common/inc/board.h #if defined(BOARD_PROFILE_CAMERA)
  - I2C GPIO4/5: OV2640 SCCB
  - N16R8: GPIO35/36/37 Octal PSRAM；按键 GPIO0
  - WS2812 GPIO48（1 LED）
  - Servo Pan GPIO46；Tilt GPIO3；LEDC 50 Hz
  - SPI1 对外（硬件 SPI3，与 ST7789 的 SPI2 隔离）：SCK=21, CS=14；
    逻辑 MOSI=GPIO45、逻辑 MISO=GPIO47（软件对调）；
    Mode1 / 1 MHz / 32B HEARTBEAT 轮询（`camera_spi_host`）
  - ST7789 SPI2：SCK=39, MOSI=38, CS=42, DC=40, RST=41, BL=1
  - OV2640 DVP：XCLK=15, D0–D7=11/9/8/10/12/18/17/16

OTA:
  idf -Project camera release 1.0.0
  manifest key: products.camera.ota
  上传 camera_*.bin 至 http://<device>/ota

相关工具说明：`camera/tools/steel_ball_annotate/README.txt`  
模型导入（带日期）：`camera/tools/promote_ball_model.py`；训练细则：`camera/tools/esp-detection/过程文档.md`
