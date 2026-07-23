camera — ESP32-S3 camera preview device (OTA via web)

Peripherals: ST7789 240x135, OV2640 camera (LCD + SoftAP/STA web preview/capture + Wi-Fi provision).
Note: on-device detect model is not linked in this build (preview-only); see plan Phase 4.

Build:
  powershell -ExecutionPolicy Bypass -File .\scripts\build_camera.ps1
  or: idf -Project camera build

Clean rebuild:
  Remove-Item camera\sdkconfig, camera\build -Recurse -Force
  idf -Project camera build

Flash & monitor:
  idf -Project camera -p PORT flash monitor

Web (after join SoftAP ESP32-WebCtrl / esp32web1):
  http://192.168.4.1/            camera preview + Wi-Fi provision UI
  http://192.168.4.1/provision   redirects to /
  http://192.168.4.1/ota         OTA upload

Pins: see common/inc/board.h #if defined(BOARD_PROFILE_CAMERA) section.
  - Single I2C GPIO4/5: OV2640 SCCB
  - N16R8: GPIO35/36/37 for Octal PSRAM, button on GPIO0
  - WS2812 RGB on GPIO48 (1 LED)
  - Servo  PWM GPIO3(PWM2) GPIO46(PWM1)
  - 通信   SPI1: SCK=21, MOSI=47, MISO=45, CS=14
  - ST7789 SPI2: SCK=39, MOSI=38, CS=42, DC=40, RST=41, BL=1
  - OV2640 DVP: XCLK=15, D0-D7=11/9/8/10/12/18/17/16

OTA:
  idf -Project camera release 1.0.0
  manifest key: products.camera.ota
  Upload camera_*.bin at http://<device>/ota
