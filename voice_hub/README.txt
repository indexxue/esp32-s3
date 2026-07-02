voice_hub — ESP32-S3 交互式设备（局域网 Web + 辅摄像头）

外设：ST7789 240×135、OV2640、1-bit SDMMC。

构建：
  powershell -ExecutionPolicy Bypass -File .\scripts\build_voice_hub.ps1
  或：idf -Project voice_hub build

清空重编：
  Remove-Item voice_hub\sdkconfig, voice_hub\build -Recurse -Force
  idf -Project voice_hub build

烧录监视：
  idf -Project voice_hub -p PORT flash monitor

引脚：编辑 common/inc/board.h 中 `#if defined(BOARD_PROFILE_VOICE_HUB)` 段。
  - 单 I2C GPIO4/5：OV2640 SCCB
  - N16R8：GPIO35/36/37 为 Octal PSRAM，按键用 GPIO0（勿占用 35–37）
  - WS2812 RGB 数据线 GPIO48（1 颗）
  - 电池 ADC GPIO3（ADC1_CH2），无采样使能脚
Bring-up 顺序：common/inc/board.h 中 BOARD_PROFILE_VOICE_HUB 段 VOICE_HUB_ENABLE_*（M1→M3）。

OTA：
  idf -Project voice_hub release 1.0.0
  manifest 键：products.voice_hub.ota
  SoftAP 维护页上传 voice_hub_*.bin
