厂测固件工程（ESP-IDF 独立 project，CMake 名 `factory`）
=====================================================

与量产工程 `project/` 并列，共用仓库内 `common`、`cbb`、`bsp_driver` 组件与 `flash_partition/partitions_16m_n16r8.csv`。

构建（需已安装本仓库 Espressif 工具链，与 `scripts/build.ps1` 相同环境）：

  powershell -ExecutionPolicy Bypass -File scripts/build_factory.ps1

首次无 `sdkconfig` 时会自动执行 `idf.py set-target esp32s3`。脚本会 dot-source IDF 的 `export.ps1` 以加入交叉编译器 PATH。

烧录说明：

- `idf.py -C factory flash` 会把 `factory.bin` 写到分区表里的 **默认应用槽（app_a / 0x30000）**，与量产工程相同，一般 **不要** 用它覆盖量产槽做厂测发布。
- 厂测镜像应写到 **app_b（ota_1）**，起始地址 **0x00810000**。示例：

  powershell -ExecutionPolicy Bypass -File factory/flash_app_b.example.ps1 -Port COM3

- 一次写入 bootloader、分区表、A 槽量产 + B 槽厂测（需先分别编译 `project` 与 `factory`）：

  powershell -ExecutionPolicy Bypass -File factory/flash_dual_slot.example.ps1 -Port COM3

在 `main.c` 中增加你的厂测任务或调用；`factory.c` 已包含与量产类似的板级初始化、按键、灯效与 USB 厂测命令（`boot_a` / `boot_b` / `boot_q` 等）。双槽说明见 `doc/partition_switch_development_plan.md`。
