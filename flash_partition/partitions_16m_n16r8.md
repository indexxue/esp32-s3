# ESP32-S3 N16R8 Flash 分区说明

芯片：**ESP32-S3 N16R8** — **16 MiB** 片内 Flash（N16）、**8 MiB** OPI PSRAM（R8）。PSRAM 不在分区表里配置，由 `sdkconfig` 中 PSRAM 相关选项管理。

**地址体系**：下表与框图使用 **SPI Flash 线性偏移**（与 `partitions_*.csv`、`esptool.py` 烧录地址一致，基址 **0x00000000**）。SoC 上电后 **CPU 取指地址** 经 Cache/MMIO 映射，日常固件请用 `esp_partition_*` 获取运行时可写可读逻辑地址，勿与下述物理偏移混用。

**两种末地址写法**：**§0 框图 / Summary** 采用与 STM32 数据手册相同的 **闭区间末字节**（`Start - End` 均落在该段内）。**§3 表格** 中 **End(不含)** 为 **半开区间** 上界，便于与 `offset + size` 校验。

---

## 0. Flash 框图（STM32 手册风格）

下列布局与 `partitions_16m_n16r8.csv` 一致；框内右侧为 **大小** 与 **用途** 简述。

```text
# ESP32-S3 N16R8 Flash Partition Layout
# Total: 16MiB (0x1000000), Base: 0x00000000  (SPI Flash linear offset / esptool)

0x00000000  ┌─────────────────────────┐
            │      Bootloader         │  32KiB (0x8000)   2nd-stage bootloader (not in CSV)
0x00007FFF  ├─────────────────────────┤
0x00008000  │    Partition table      │  4KiB (0x1000)    partition binary @ CONFIG offset
0x00008FFF  ├─────────────────────────┤
0x00009000  │          nvs            │  128KiB (0x20000) NVS key-value
0x00028FFF  ├─────────────────────────┤
0x00029000  │        otadata          │  8KiB (0x2000)    OTA slot metadata (required)
0x0002AFFF  ├─────────────────────────┤
0x0002B000  │        phy_init         │  4KiB (0x1000)    PHY calibration (Wi-Fi/BT)
0x0002BFFF  ├─────────────────────────┤
0x0002C000  │  (64KiB 对齐留白)       │  16KiB (0x4000)   ESP-IDF 为 `app` 自动插入，非 CSV 独立行
0x0002FFFF  ├─────────────────────────┤
0x00030000  │                         │
            │   APP-A (app_a/ota_0)   │  8257536B (~7.88MiB) (0x7E0000)  Primary application
            │                         │
0x0080FFFF  ├─────────────────────────┤
0x00810000  │                         │
            │   APP-B (app_b/ota_1)   │  8257536B (~7.88MiB) (0x7E0000)  Factory test / OTA peer
            │                         │
0x00FEFFFF  ├─────────────────────────┤
0x00FF0000  │  (Flash 末段留白)       │  64KiB (0x10000)  未在 CSV 中声明；可留作将来扩展
0x00FFFFFF  └─────────────────────────┘

# Summary (inclusive ranges — 与框图边界行一致)
#   Bootloader:       0x00000000 - 0x00007FFF
#   Partition table:  0x00008000 - 0x00008FFF
#   nvs:              0x00009000 - 0x00028FFF
#   otadata:          0x00029000 - 0x0002AFFF
#   phy_init:         0x0002B000 - 0x0002BFFF
#   (对齐间隙)        0x0002C000 - 0x0002FFFF  (0x4000，满足 app 分区基址 64KiB 对齐)
#   APP-A:            0x00030000 - 0x0080FFFF  (normal production; subtype ota_0)
#   APP-B:            0x00810000 - 0x00FEFFFF  (factory test / OTA peer; subtype ota_1)
#
# OTA: esp_ota writes the inactive slot — if running from APP-A, update image goes to APP-B.
# Boot slot selection: see CONFIG_BOOTLOADER_APP_*, otadata, and esp_ota_set_boot_partition().
```

---

## 1. 角色定义（与你的业务一致）

| 分区名（CSV） | 子类型（必须） | 含义 |
|---------------|----------------|------|
| **app_a** | `ota_0` | **正式应用（App_A）**：量产设备日常运行的固件所在分区。 |
| **app_b** | `ota_1` | **厂测 / 备用槽（App_B）**：产线厂测固件、或 OTA 时写入的**对侧**分区；与 App_A **同大小**。 |

---

## 2. OTA 与「一般覆盖 App_B」的理解

双槽 OTA：**向当前未运行的槽**写入新固件，再校验、切换启动分区。

- 若现场 **固定从 App_A 启动**，则 OTA 写入 **App_B**（对侧），即常见「覆盖 B」。
- 若从 **App_B** 启动，则下一次 OTA 写入 **App_A**。

---

## 3. Flash 全地址映射（嵌入式核对用）

**Flash 总容量**：`0x00000000` — `0x01000000`（16 MiB = 16 777 216 字节；末字节 `0x00FFFFFF`）。

### 3.1 片头（不在 `partitions_*.csv` 中声明）

| 区域 | Start | End(不含) | 末字节(含) | Size (hex) | 说明 |
|------|--------|-----------|------------|------------|------|
| Bootloader 区 | `0x00000000` | `0x00008000` | `0x00007FFF` | `0x8000` | 32 KiB；镜像实际长度以编译产物为准 |
| Partition table | `0x00008000` | `0x00009000` | `0x00008FFF` | `0x1000` | 4 KiB；`CONFIG_PARTITION_TABLE_OFFSET` 默认 `0x8000` |
| **片头合计** | `0x00000000` | `0x00009000` | — | `0x9000` | 36 KiB（Bootloader + 分区表）；其后接 CSV 首分区 **nvs** |

### 3.2 CSV 声明分区（当前 `partitions_16m_n16r8.csv`）

| 分区名 | Type | Subtype | Start | End(不含) | 末字节(含) | Size (hex) |
|--------|------|---------|--------|-----------|------------|------------|
| nvs | data | nvs | `0x00009000` | `0x00029000` | `0x00028FFF` | `0x20000` |
| otadata | data | ota | `0x00029000` | `0x0002B000` | `0x0002AFFF` | `0x2000` |
| phy_init | data | phy | `0x0002B000` | `0x0002C000` | `0x0002BFFF` | `0x1000` |
| *(对齐留白)* | — | — | `0x0002C000` | `0x00030000` | `0x0002FFFF` | `0x4000` |
| **app_a**（App_A） | app | ota_0 | `0x00030000` | `0x00810000` | `0x0080FFFF` | `0x7E0000` |
| **app_b**（App_B） | app | ota_1 | `0x00810000` | `0x00FF0000` | `0x00FEFFFF` | `0x7E0000` |

`app` 分区在 CSV 中省略偏移时，生成器会按 **64KiB** 对齐插入间隙（见 `gen_esp32part.py` 中 `ALIGNMENT[APP]`）。末 **64KiB**（`0xFF0000`—`0x01000000`）未声明为分区，属正常留白。校验：`0x9000 + 0x20000 + 0x2000 + 0x1000 + 0x4000 + 0x7E0000 + 0x7E0000 = 0x00FF0000`（至末槽 **End(不含)**）。

---

## 4. 嵌入式工程引用（C 常量，按需拷贝）

与 **当前** CSV 布局一致；若改分区表，请同步改数值。

```c
/* flash_partition/partitions_16m_n16r8 — 与 partitions_16m_n16r8.csv 一致 */
#define FLASH_CHIP_SIZE         (0x01000000u)   /* 16 MiB */

#define FLASH_BOOTLOADER_START  (0x00000000u)
#define FLASH_BOOTLOADER_END    (0x00008000u)   /* 不含；与 IDF 默认 32KiB 窗口对齐 */

#define FLASH_PART_TABLE_START  (0x00008000u)
#define FLASH_PART_TABLE_END    (0x00009000u)   /* 不含 */

#define PART_NVS_START          (0x00009000u)
#define PART_NVS_SIZE           (0x00020000u)
#define PART_NVS_END            (PART_NVS_START + PART_NVS_SIZE)

#define PART_OTADATA_START      (0x00029000u)
#define PART_OTADATA_SIZE       (0x00002000u)
#define PART_OTADATA_END        (PART_OTADATA_START + PART_OTADATA_SIZE)

#define PART_PHY_INIT_START     (0x0002B000u)
#define PART_PHY_INIT_SIZE      (0x00001000u)
#define PART_PHY_INIT_END       (PART_PHY_INIT_START + PART_PHY_INIT_SIZE)

#define PART_APP_ALIGN_GAP_START (0x0002C000u) /* phy 后至首 app 前的 64KiB 对齐留白 */
#define PART_APP_ALIGN_GAP_SIZE  (0x00004000u)

#define PART_APP_A_START        (0x00030000u)
#define PART_APP_A_SIZE         (0x007E0000u)
#define PART_APP_A_END          (PART_APP_A_START + PART_APP_A_SIZE)

#define PART_APP_B_START        (0x00810000u)
#define PART_APP_B_SIZE         (0x007E0000u)
#define PART_APP_B_END          (PART_APP_B_START + PART_APP_B_SIZE)

/* 未分区留白：PART_APP_B_END .. FLASH_CHIP_SIZE（当前为 0x10000 字节） */

_Static_assert(PART_APP_B_END <= FLASH_CHIP_SIZE, "partition map must not exceed flash top");
```

日常开发 **优先** 使用 `esp_partition_find_*` / `esp_ota_get_next_update_partition()`；上表适用于 **脚本、评审与和 §0 框图对数**。

---

## 5. 供 ESP-IDF 使用的 CSV

编译：**Partition Table → Custom**，路径例如 `../flash_partition/partitions_16m_n16r8.csv`。

```csv
nvs,       data, nvs,     0x9000,  0x20000,
otadata,   data, ota,     ,        0x2000,
phy_init,  data, phy,     ,        0x1000,
app_a,     app,  ota_0,   0x30000, 0x7E0000,
app_b,     app,  ota_1,   ,        0x7E0000,
```

---

## 6. 可选 `sdkconfig.defaults` 片段

```properties
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../flash_partition/partitions_16m_n16r8.csv"
```

修改分区大小或 **Bootloader / 分区表偏移** 后，须重烧分区表，并同步 **§0 框图、Summary、§3 表、§4 常量** 与 CSV 首行 `Offset`。
