#ifndef COMMON_SDCARD_H
#define COMMON_SDCARD_H

#include "sdmmc_cmd.h"
#include "type.h"

/**
 * 使用 `board.h` 中的 SDMMC 引脚挂载 FAT（VFS 路径 `base_path`）。
 * 重复挂载返回 `ESP_ERR_INVALID_STATE`。
 */
status_t sdcard_mount(const char *base_path);

/** 与已成功挂载的 `base_path` 一致；卸载后句柄失效。 */
status_t sdcard_unmount(const char *base_path);

/** 未挂载时为 NULL。 */
sdmmc_card_t *sdcard_get_card(void);

/**
 * 挂载成功后的烟测：写读短文本 `sd_test.txt`（8.3 短名），再调用 `sdcard_dma_throughput_benchmark_log`。
 * 依赖当前已成功挂载（`sdcard_get_card()` 非 NULL 且内部挂载路径已记录）。未挂载时打 WARN 并返回。
 */
void sdcard_mount_smoke_and_benchmark_log(void);

/**
 * 在已挂载且存在卡时，用 DMA 能力缓冲区测 FAT 顺序写（含 `fsync`）与顺序读吞吐并打印 MiB/s。
 * 使用同一 `w+b` 文件句柄写后 `rewind` 再读，避免关写再开读时元数据/介质未同步导致读极慢。
 * 无卡或未挂载时静默返回。通常由 `sdcard_mount_smoke_and_benchmark_log` 调用，也可单独调用。
 */
void sdcard_dma_throughput_benchmark_log(void);

#endif
