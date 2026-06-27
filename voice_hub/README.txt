voice_hub — ESP32-S3 交互式语音设备（局域网对讲 + 辅摄像头）

外设：ST7789 240×135、ES8311、OV2640、1-bit SDMMC。

构建：
  powershell -ExecutionPolicy Bypass -File .\scripts\build_voice_hub.ps1
  或：idf -Project voice_hub build

烧录监视：
  idf -Project voice_hub -p PORT flash monitor

引脚：编辑 common/inc/board.h 中 `#if defined(BOARD_PROFILE_VOICE_HUB)` 段。
  - 单 I2C GPIO4/5：ES8311 + OV2640 SCCB
  - ES8311 → NS4150B，播放使能 GPIO46；录音 I2S DIN=GPIO47
  - WS2812 RGB 数据线 GPIO48（1 颗）
  - 电池 ADC GPIO3（ADC1_CH2），无采样使能脚
Bring-up 顺序：voice_hub/main/voice_hub_config.h 中 VOICE_HUB_ENABLE_*（M1→M5）。

OTA：
  idf -Project voice_hub release 1.0.0
  manifest 键：products.voice_hub.ota
  SoftAP 维护页上传 voice_hub_*.bin
