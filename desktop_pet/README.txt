desktop_pet — ESP32-S3 桌宠（骨架 + 外设自检）

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

自检（默认开）：
  DESKTOP_PET_ENABLE_SELFTEST=1 → 上电跑一轮烟测 + USB 命令行
  串口监视器输入：
    help
   ptest              # 全项
    ptest lcd|bat|imu|led|motor|sd|i2c|audio|touch

Bring-up 开关（board.h，改后重编）：
  DESKTOP_PET_ENABLE_LCD / IMU / MOTOR / SDCARD / AUDIO / TOUCH 默认 1
  SD：需插卡；FatFS 报 (13)=卡已识别但无合法 FAT → 请格式化为 FAT32（勿用 exFAT）。

OTA：
  idf -Project desktop_pet release 1.0.0
