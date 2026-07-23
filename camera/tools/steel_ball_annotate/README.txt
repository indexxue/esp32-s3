steel_ball_annotate — SoftAP 采数 + YOLO 标注（PC 上位机）

用途
  连接 ESP32-S3 camera SoftAP，轮询 JPEG 预览，抓帧落盘，
  对 10 类钢珠画框，导出 YOLO 归一化标签（供后续训练 / ESP-DL）。

前置
  1. 板端 camera 固件已启动 SoftAP + 网页图传
  2. PC 连上设备热点（默认入口 http://192.168.4.1/）
  3. Python 3 + Tkinter（Windows 官方安装包通常已含）

安装
  cd camera\tools\steel_ball_annotate
  py -3 -m pip install -r requirements.txt

运行
  py -3 .\annotate_app.py

操作
  Connect   — 读 /api/camera/status，开始轮询 /api/camera/camera.jpg（约 4 fps）
  Capture   — 当前帧写入 dataset/images/{时间戳}.jpg，进入标注
  拖拽画框  — 像素 xywh（原点左上）；数字键 0-9 或下拉选类
  点选框    — 再按 0-9 可改类；Del/Backspace 删除
  Save      — 写 dataset/labels/{同名}.txt（YOLO: cls cx cy w h 归一化）
  Prev/Next — 浏览已采图片并加载对应标签
  再按 Connect — 回到实时预览

坐标约定（与板端一致）
  原点左上，x 右、y 下
  帧尺寸以 /api/camera/status 的 width/height 为准（当前 240x240）
  YOLO: cx=(x+w/2)/W, cy=(y+h/2)/H, nw=w/W, nh=h/H

目录
  dataset/images/   抓帧 JPEG（不入库）
  dataset/labels/   YOLO txt（不入库）
  dataset/classes.txt   类名 cls0..cls9（可改业务名）
  dataset/data.yaml     ultralytics 风格占位
  config.json           记住 Base URL（本地，不入库）

故障提示
  连接失败     — PC 未连 SoftAP 或 Base URL 错误
  403          — 非 SoftAP/同子网，板端拒绝摄像头接口
  503          — 相机未就绪 / WiFi 暂停 / 无帧

本工具不做：USB 摄像头、YOLO 训练、板端 detect JSON 叠框、云台仿真。
