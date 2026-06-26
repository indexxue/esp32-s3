ballot_guard — 基于 ESP32-S3 的端边协同智能选票识别与全流程监管系统

分区：与 project/ 相同，使用 flash_partition/partitions_16m_n16r8.csv
  - nvs      @ 0x9000
  - app_a    @ 0x30000 (ota_0，idf.py flash 默认写入此槽)
  - app_b    @ 0x810000 (ota_1)

构建：
  powershell -ExecutionPolicy Bypass -File .\scripts\build_ballot_guard.ps1

若从旧版升级后 /ota 不可用：删除 ballot_guard/sdkconfig 后重新 build（从 sdkconfig.defaults 合并 OTA 选项）。

烧录（槽位 A）：
  idf -Project ballot_guard -p PORT flash monitor

OTA（阶段 2，与 project/ 共用 common/ota + web_ctrl）：
  - SoftAP 维护：连接设备 AP → http://192.168.4.1/ota 上传 ballot_guard_*.bin
  - STA 云端拉包：/ota 页「云端拉包」或 POST /api/ota/pull
      manifest 须含 products.ballot_guard.ota（idf -Project ballot_guard release <ver>）
  - 默认 product 键：ballot_guard（sdkconfig WEB_CTRL_OTA_DEFAULT_PRODUCT）
  - 设计文档：doc/ota_development_plan.md

Release：
  idf -Project ballot_guard release 1.0.0
  产物：firmware/<ver>/manifest.json → products.ballot_guard.ota.image
