ESP32-S3 BLE 演示工程（ble_demo）
================================

详细说明（应用领域、蓝牙协议入门、本工程协议、测试步骤）请阅读：

  README.md

快速速查
--------
- 广播名：ESP32-S3-BLE-Demo
- 服务 UUID：0xFFF0，特征值 UUID：0xFFF1
- Write 1 字节 '0'~'5' 切换 RGB 灯效；Notify 每秒 4 字节小端计数
- 编译：scripts/build_ble_demo.ps1
- 烧录：idf.py -C ble_demo -p COMx flash monitor
