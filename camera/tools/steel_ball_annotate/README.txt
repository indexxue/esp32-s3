steel_ball_annotate — SoftAP 采数 + YOLO 标注（PC 上位机）

用途
  连接 ESP32-S3 camera SoftAP，独占 MJPEG 预览（板端关 LCD），
  抓帧落盘，画框导出 YOLO 归一化标签（单类阶段只标 class 0 = ball）。
  完整采数→ESPDet 训练→上板流程见：camera/README.txt

前置
  1. 板端 camera 固件已启动 SoftAP + 网页图传
     建议采数固件：CAMERA_APP_COLLECT_MODE=1（关检测）
  2. PC 连上设备热点（默认入口 http://192.168.4.1/）
  3. 关闭浏览器 /preview.html（独占流只能一路）
  4. Python 3 + Tkinter（Windows 官方安装包通常已含）

安装
  cd camera\tools\steel_ball_annotate
  py -3 -m pip install -r requirements.txt

运行
  py -3 .\annotate_app.py
  py -3 .\validate.py
  py -3 .\validate.py -m best.pt -i dataset\images\xxx.jpg

快捷键
  Space           抓帧进入标注
  Esc             回实时预览（有未保存会提示）
  Ctrl+S          手动保存标签
  Ctrl+Z          撤销框操作
  Ctrl+Del        删除当前图片 + 对应标签（不可恢复）
  0-9             当前类；有选中框则改其类别（单类请只用 0）
  Del / Backspace 删除选中框
  ← → / A D       上一张 / 下一张

标注操作
  空白处拖拽     新建框（松手自动保存）
  点选框         选中（可改类 / 删除）
  拖框内部       移动（松手自动保存）
  拖四角手柄     缩放（松手自动保存）
  右侧列表       点选框 / 清框 / Delete this image
  顶栏           Delete image — 删当前图与 labels 同名 txt

预览优化说明
  独占 /api/camera/stream.mjpg（与网页 preview 同通道，板端关 LCD）；
  完整 JPEG 边界解析；UI 限约 15fps；抓帧用最新完整帧；
  显示最多 2× 最近邻放大（源图始终 240×240）。

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

单类训练打包（Pack 1cls zip）
  顶栏按钮一键整理：跳过有图无标；按约 80/20 拆 train/val；
  标签强制 class_id=0；写入 classes.txt=ball 与 nc=1 的 data.yaml；
  打成 steel_ball_1cls_*.zip（不改动 dataset/ 原文件）。
  上传云主机后把 data.yaml 的 path 改成解压绝对路径再训。

故障提示
  连接失败     — PC 未连 SoftAP 或 Base URL 错误
  403          — 非 SoftAP/同子网，板端拒绝摄像头接口
  503 / 超时   — 浏览器开着 /preview.html 独占流，或固件在跑检测抢 CPU
                 Connect 会尝试 pause/resume 释放通道；仍失败则：
                 1) 关掉独占预览页
                 2) 采数固件：CAMERA_APP_COLLECT_MODE=1 重编烧录
  fps 低/rtt 高 — SoftAP 忙或信号差；识别固件下请改采数模式再采

板端模式（camera/main/CMakeLists.txt）
  CAMERA_APP_COLLECT_MODE=1  采数：关 ESPDet，专供本工具 JPEG 抓帧
  CAMERA_APP_COLLECT_MODE=0  识别：开检测（默认）；不要同时开独占预览+本工具
  改宏后：idf -Project camera build 并烧录

验证工具（validate.py）
  可视化选模型（.pt/.onnx）与图片目录/单张图，YOLO 推理叠框+中心点。
  顶栏 Load / Detect / Save；侧栏图片列表与检测列表；conf/iou 滑条。
  快捷键：← → / A D 切图，Enter / F5 检测，Ctrl+S 保存结果。
  配置记住：validate_config.json；结果默认目录 validate_results/。

本工具不做：USB 摄像头、YOLO 训练、板端 detect JSON 叠框、云台仿真。
