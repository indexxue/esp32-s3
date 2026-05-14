# ESP32-S3 SDMMC + FAT 读写技术说明

本文说明本工程中 SD 卡挂载、DMA 与文件读写路径、自测接口及常见问题，便于维护与换板。

## 1. 软件栈概览

从应用到硬件的典型路径如下。

```
应用 fopen/fread/fwrite/fsync
    → ESP-IDF VFS（`esp_vfs_fat`）
    → FatFs
    → SD/MMC 磁盘抽象（`sdmmc`）
    → SDMMC 主机驱动（`esp_driver_sdmmc`）
    → 片上 SDMMC 外设 + 内置 IDMAC（DMA 描述符环，非应用层可选 GDMA 通道号）
    → SD 卡物理总线（CLK / CMD / DAT0~DAT3）
```

**读与写在物理层共用同一组引脚与主机控制器**，在软件层则经过 FatFs 与 VFS 的不同逻辑（目录项、FAT 表、簇分配与读路径等），因此性能与行为可能不对称；属正常现象。

## 2. 本工程相关模块与配置

| 位置 | 作用 |
|------|------|
| `common/inc/board.h` | `BOARD_SDCARD_PIN_*`、`BOARD_SDCARD_BUS_WIDTH`、`BOARD_SDCARD_MAX_FREQ_KHZ`、`BOARD_SDCARD_MOUNT_POINT` 等板级宏 |
| `bsp_driver/inc/sdio.h` / `sdio.c` | 根据 `SdioBoardConfig_t` 填充 `sdmmc_host_t` / `sdmmc_slot_config_t`（含 `host_flags_or`、DMA 路径枚举） |
| `bsp_driver/inc/dma.h` / `dma.c` | `DmaMalloc` / `DmaBufferIsBusCapable`；SDMMC 使用主机内置 IDMAC 的说明见 `DmaSdmmcPath_t` |
| `common/src/sdcard.c` | `sdcard_mount` / `sdcard_unmount`、烟测与吞吐自测 |

挂载时使用 `esp_vfs_fat_sdmmc_mount`，分配单元等见 `sdcard.c` 内 `esp_vfs_fat_mount_config_t`（如 `allocation_unit_size`）。

## 3. 数据缓冲与 DMA

- **经 FAT/VFS 的 `fread`/`fwrite`**：缓冲区由应用提供；大块顺序 I/O 时，底层块设备仍会通过主机 **IDMAC** 与介质交换数据。使用 **DMA 能力内存**（如 `DmaMalloc`）做应用缓冲，有利于与驱动期望一致，尤其在后续若扩展直接块访问时。
- **`sdmmc_read_sectors` / `sdmmc_write_sectors`（`sdmmc_cmd.h`）**：数据缓冲区须 **DMA 可访问** 且满足对齐要求（见 ESP-IDF SDMMC 文档）。**在已通过 FAT 挂载同一张卡时，不宜与 FatFs 并发直访扇区**，否则易出现长时间阻塞或异常；若需裸扇区压测，应在卸载 FAT 或独占初始化场景下进行。

## 4. 推荐 I/O 习惯（避免读极慢等问题）

1. **`fflush` 与 `fsync`**：`fflush` 主要作用于 C 库/VFS 层缓存；要保证目录与数据在介质上达到一致状态，写后应 **`fsync(fileno(fp))`**（在支持的路径上）。
2. **同一句柄先写再读**：使用 **`fopen(..., "w+b")`**，写完后 `fflush` + `fsync`，再 **`rewind`** 后 `fread`，可避免「关闭写文件再立刻以只读打开」时，FAT/目录与介质未完全同步导致的读路径极慢或隐式重试。
3. **缓冲策略**：全缓冲 `setvbuf(..., _IOFBF, ...)` 可减少小粒度系统调用次数；需根据 RAM 权衡缓冲区大小。
4. **文件名**：默认 FAT 常为 **8.3 短文件名** 时，主文件名过长会导致 `fopen` 失败（如 `errno = EINVAL`）；烟测使用 `sd_test.txt`、`sdbnch.tmp` 等短名。

## 5. 自测与吞吐接口（`sdcard.c`）

| API | 说明 |
|-----|------|
| `sdcard_mount_smoke_and_benchmark_log()` | 挂载成功后调用：写读 `sd_test.txt` 烟测，再调用吞吐测试 |
| `sdcard_dma_throughput_benchmark_log()` | 仅吞吐测试：在挂载点下创建临时文件，**顺序写（含 `fsync`）+ `rewind` 顺序读**，打印 MiB/s 与 `real_freq_khz`、线宽 |

吞吐测试使用 **128 KiB × 32 轮**（约 4 MiB 量级），临时文件名为 `sdbnch.tmp`，测试结束后 **`remove` 删除**。

应用侧示例（本工程 `main.c` 在 `app_run` 起始处）：

```c
sdcard_mount_smoke_and_benchmark_log();
```

## 6. 硬件与排障要点

- **线长、阻抗、串扰**：高速 4 线模式下，读方向对采样时刻更敏感；布线差时易出现 CRC 重试，表现为某一方向吞吐骤降。
- **供电与去耦**：写突发与读突发电流不同，电源不稳易引发偶发错误与重试。
- **卡质量与接触**：劣质卡或接触不良时，读错误重试往往比写更明显。

若吞吐日志中读远慢于写，优先在软件侧确认 **`fsync` + 同句柄 `rewind` 读**；仍异常时再查硬件与换卡。

## 7. 参考

- ESP-IDF：SDMMC Host、FAT 文件系统、VFS 相关章节（版本以仓库内 `Espressif/frameworks/esp-idf-*` 为准）。
- 工程内编译与烧录步骤：`doc/compile_flash_erase_monitor.md`。
