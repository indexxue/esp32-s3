ballot_guard — 基于 ESP32-S3 的端边协同智能选票识别与全流程监管系统

分区：与 project/ 相同，使用 flash_partition/partitions_16m_n16r8.csv
  - nvs      @ 0x9000
  - app_a    @ 0x30000 (ota_0，idf.py flash 默认写入此槽)
  - app_b    @ 0x810000 (ota_1)

构建：
  powershell -ExecutionPolicy Bypass -File .\scripts\build_ballot_guard.ps1

烧录（槽位 A）：
  idf.py -C ballot_guard -p PORT flash monitor
