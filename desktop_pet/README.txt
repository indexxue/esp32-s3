desktop_pet — ESP32-S3 桌宠（LVGL UI + 板级驱动）

引脚（common/inc/board.h → BOARD_PROFILE_DESKTOP_PET）：
  - I2C 唯一：SCL=18 SDA=8（ES8311 / QMI8685 / IT7259@0x46；无 INT/RST）
  - GC9A01：SCK=7 MOSI=16 MISO=15 CS=6 DC=17 BL=5；无 RST（靠重新上电）
  - I2S：DIN=9 LRCK=10 DOUT=11 BCLK=12 MCLK=13；SPE_EN=14
  - TB6612：PWMA=45 PWMB=38；M1_IN1=46 M1_IN2=3；M2_IN1=21 M2_IN2=47
  - SDMMC：CK=42 CMD=41 D0=2 D1=1 D2=39 D3=40
  - WS2812 RGB_DIN=48；按键=0；电池 ADC=GPIO4

构建 / 烧录：
  idf -Project desktop_pet build
  idf -Project desktop_pet -p PORT flash monitor

板端 LVGL UI（默认）：
  BoardInit 后启动 desktop_pet_ui（owo + pet / Rec；点 pet 切换表情）
  触摸：IT7259 I2C@0x46 轮询 → LVGL indev（DESKTOP_PET_ENABLE_TOUCH）
  录音：ES8311 + I2S → PSRAM；Stop 后写 `/sdcard/record/rec_XXXX.wav`
  需 SD 已挂载（FAT32）；无卡时仍可录音，仅保存失败
  依赖：main/idf_component.yml → lvgl/lvgl（managed_components 不入库）
  若点击偏移：改 desktop_pet_ui.c 里 it7259 panel/swap/invert 配置

Web（AP/STA 同一 HTTP）：
  SoftAP 默认 http://192.168.4.1/ ；STA 用设备 DHCP IP
  `GET /` SD 文件管理页；`/api/sd/list|file|delete`；另有 `/provision` `/ota`
  需 DESKTOP_PET_ENABLE_WIFI_WEB=1 且 CONFIG_WEB_CTRL_AUTO_START

自检（参考程序，默认不编译/不启动）：
  源码：main/source/desktop_pet_selftest.c|.h
  恢复：CMakeLists SRCS 加回该 .c；start.c 调用 desktop_pet_selftest_start()；
        board.h 中 DESKTOP_PET_ENABLE_SELFTEST=1
  串口：help / ptest / ptest lcd|bat|imu|led|motor|sd|i2c|audio|touch

Bring-up 开关（board.h，改后重编）：
  DESKTOP_PET_ENABLE_LCD / IMU / MOTOR / SDCARD / AUDIO / TOUCH 默认 1
  DESKTOP_PET_ENABLE_SELFTEST 默认 0
  SD：需插卡；FatFS 报 (13)=卡已识别但无合法 FAT → 请格式化为 FAT32（勿用 exFAT）。

OTA：
  idf -Project desktop_pet release 1.0.0

LVGL PC 模拟器（仅本机下载，不入库）：
  powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1
  产物目录 tools\lvgl_sim\（已在 .gitignore）
  用 Visual Studio 打开其中 LVGL.sln，跑 LvglWindowsSimulator
  桌宠屏请按 240×240（GC9A01 圆屏）设计
