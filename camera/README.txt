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
  以及 `infer ... boxes=N`；LCD 绿框。
  若板上仍像旧模型：删 camera\build 后重编。

详情：`camera/tools/esp-detection/过程文档.md` §5（本地训练树内；本 README 为准入门）。

关键源码：
  components/ball_detect/     ESPDet 封装 + models/s3/*.espdl
  main/source/camera_model.*  检测任务与结果缓存
  main/source/camera_ui.*     LCD 画框
  main/source/camera_sensor.* 预览；blit 后叠框

sdkconfig（见 sdkconfig.defaults）：
  CONFIG_FLASH_ESPDET_PICO_224_224_BALL=y
  CONFIG_BALL_DETECT_MODEL_IN_FLASH_RODATA=y

---- 5. 当前能力与后续 ----

已完成：SoftAP 采数标注、ESPDet 训量化、固件推理、LCD 绿框。
后续可选：`/api/detect/latest` 网页客户端叠框、云台按框中心粗跟、补困难样本再训。

舵机轴：PWM1=GPIO46 Pan；PWM2=GPIO3 Tilt。画面 err_x>0 → pan 右；err_y>0 → tilt 下。
装反时改 `BOARD_SERVO_PAN_SIGN` / `BOARD_SERVO_TILT_SIGN`（`common/inc/board.h`）。

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
        {"center":1} | {"pan_delta":5} | {"tilt":90}
  网页 D-pad：点按一步、长按连续；长按期间暂停 MJPEG，松手约 0.7s 后恢复。
  板键 GPIO0 长按回中。无 USB 串口舵机命令。

Pins: see common/inc/board.h #if defined(BOARD_PROFILE_CAMERA)
  - I2C GPIO4/5: OV2640 SCCB
  - N16R8: GPIO35/36/37 Octal PSRAM；按键 GPIO0
  - WS2812 GPIO48（1 LED）
  - Servo Pan GPIO46；Tilt GPIO3；LEDC 50 Hz
  - SPI1 预留（本阶段不 init）：SCK=21, MOSI=47, MISO=45, CS=14
  - ST7789 SPI2：SCK=39, MOSI=38, CS=42, DC=40, RST=41, BL=1
  - OV2640 DVP：XCLK=15, D0–D7=11/9/8/10/12/18/17/16

OTA:
  idf -Project camera release 1.0.0
  manifest key: products.camera.ota
  上传 camera_*.bin 至 http://<device>/ota

相关工具说明：`camera/tools/steel_ball_annotate/README.txt`  
模型导入（带日期）：`camera/tools/promote_ball_model.py`；训练细则：`camera/tools/esp-detection/过程文档.md`
