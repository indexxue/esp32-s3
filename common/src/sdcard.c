#include "sdcard.h"

#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "soc/soc_caps.h"

#include "board.h"
#include "dma.h"
#include "log.h"
#include "sdio.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** 记录已挂载 VFS 路径的缓冲区大小（字节，含结尾空字符）。 */
#define SDCARD_MOUNT_PATH_CAP 32U

/** VFS 同时打开文件数上限。 */
#define SDCARD_VFS_MAX_OPEN_FILES 5

/** FAT 分配单元（字节）；与簇相关，影响格式化与元数据占用。 */
#define SDCARD_VFS_ALLOCATION_UNIT_BYTES (16U * 1024U)

static sdmmc_card_t *s_card = NULL;
static char s_mounted_path[SDCARD_MOUNT_PATH_CAP] = {0};

sdmmc_card_t *sdcard_get_card(void)
{
    return s_card;
}

status_t sdcard_mount(const char *base_path)
{
#if !SOC_SDMMC_HOST_SUPPORTED
    (void)base_path;
    LOG_WARN("SDMMC host not supported on this chip");
    return ESP_ERR_NOT_SUPPORTED;
#else
    sdmmc_host_t host = {0};
    sdmmc_slot_config_t slot = {0};
    SdioBoardConfig_t cfg = {0};
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SDCARD_VFS_MAX_OPEN_FILES,
        .allocation_unit_size   = SDCARD_VFS_ALLOCATION_UNIT_BYTES,
    };

    if ((base_path == NULL) || (base_path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cfg.pin_clk     = (s32_t)BOARD_SDCARD_PIN_CLK;
    cfg.pin_cmd     = (s32_t)BOARD_SDCARD_PIN_CMD;
    cfg.pin_d0      = (s32_t)BOARD_SDCARD_PIN_D0;
    cfg.pin_d1      = (s32_t)BOARD_SDCARD_PIN_D1;
    cfg.pin_d2      = (s32_t)BOARD_SDCARD_PIN_D2;
    cfg.pin_d3      = (s32_t)BOARD_SDCARD_PIN_D3;
    cfg.bus_width   = BOARD_SDCARD_BUS_WIDTH;
    cfg.max_freq_khz = BOARD_SDCARD_MAX_FREQ_KHZ;
    cfg.sdmmc_dma_path = (DmaSdmmcPath_t)BOARD_SDCARD_SDMMC_DMA_PATH;
    cfg.host_flags_or  = BOARD_SDCARD_HOST_FLAGS_EXTRA;

    if (SdioFillSdmmcForBoard(&host, &slot, &cfg) != TRUE) {
        LOG_ERROR("SDIO pin/host build failed, esp err %s", esp_err_to_name((esp_err_t)SdioGetLastError()));
        return (status_t)SdioGetLastError();
    }

    esp_err_t err = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        LOG_ERROR("esp_vfs_fat_sdmmc_mount(%s) failed: %s", base_path, esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    (void)memset(s_mounted_path, 0, sizeof(s_mounted_path));
    (void)strncpy(s_mounted_path, base_path, (size_t)SDCARD_MOUNT_PATH_CAP - 1U);

    LOG_INFO("SD card mounted at %s", base_path);
    return ESP_OK;
#endif
}

status_t sdcard_unmount(const char *base_path)
{
#if !SOC_SDMMC_HOST_SUPPORTED
    (void)base_path;
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err;

    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((base_path == NULL) || (strcmp(base_path, s_mounted_path) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_vfs_fat_sdcard_unmount(base_path, s_card);
    if (err != ESP_OK) {
        LOG_ERROR("esp_vfs_fat_sdcard_unmount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    (void)memset(s_mounted_path, 0, sizeof(s_mounted_path));
    LOG_INFO("SD card unmounted from %s", base_path);
    return ESP_OK;
#endif
}

void sdcard_mount_smoke_and_benchmark_log(void)
{
#if !SOC_SDMMC_HOST_SUPPORTED
    return;
#else
    char path[48];
    FILE *fp;

    if (s_card == NULL) {
        LOG_WARN("SD smoke: no card (not mounted or mount failed)");
        return;
    }
    if (s_mounted_path[0] == '\0') {
        LOG_WARN("SD smoke: internal mount path empty");
        return;
    }

    LOG_INFO("SD smoke: FAT OK at %s (sdmmc_card present)", s_mounted_path);

    (void)snprintf(path, sizeof(path), "%s/sd_test.txt", s_mounted_path);
    fp = fopen(path, "w");
    if (fp == NULL) {
        LOG_ERROR("SD smoke: fopen write \"%s\" failed, errno=%d (%s)", path, (int)errno, strerror(errno));
        return;
    }
    if (fprintf(fp, "sd_ok\n") < 0) {
        LOG_ERROR("SD smoke: fprintf failed, errno=%d (%s)", (int)errno, strerror(errno));
        (void)fclose(fp);
        return;
    }
    (void)fclose(fp);

    fp = fopen(path, "r");
    if (fp == NULL) {
        LOG_ERROR("SD smoke: fopen read \"%s\" failed, errno=%d (%s)", path, (int)errno, strerror(errno));
        return;
    }
    {
        char line[16] = {0};
        if (fgets(line, (int)sizeof(line), fp) == NULL) {
            int fe = ferror(fp);
            LOG_ERROR("SD smoke: fgets failed, ferror=%d, errno=%d (%s)", fe, (int)errno, strerror(errno));
            (void)fclose(fp);
            return;
        }
        (void)fclose(fp);
        LOG_INFO("SD smoke: read back \"%s\" -> \"%s\"", path, line);
    }

    sdcard_dma_throughput_benchmark_log();
#endif
}

void sdcard_dma_throughput_benchmark_log(void)
{
#if !SOC_SDMMC_HOST_SUPPORTED
    return;
#else
#define SDCARD_BENCH_CHUNK_BYTES (128U * 1024U)
#define SDCARD_BENCH_FAT_LOOPS (32U)

    sdmmc_card_t *card = s_card;
    void *buf = NULL;
    const size_t chunk = (size_t)SDCARD_BENCH_CHUNK_BYTES;
    size_t loop_ix;
    int64_t t0;
    int64_t dt_us;
    double dt_s;
    double mib_fat_wr = 0.0;
    double mib_fat_rd = 0.0;
    char path[48];
    FILE *fp = NULL;
    size_t total_bytes;

    if (card == NULL) {
        return;
    }
    if (s_mounted_path[0] == '\0') {
        return;
    }

    LOG_INFO(
        "SD DMA bench: FAT seq write+fsync then rewind read, chunk=%u KiB x %u (single w+b handle; raw API skipped while VFS mounted)",
        (unsigned int)(chunk / 1024U),
        (unsigned int)SDCARD_BENCH_FAT_LOOPS);

    buf = DmaMalloc((usize_t)chunk);
    if ((buf == NULL) || (DmaBufferIsBusCapable(buf, (usize_t)chunk) == FALSE)) {
        LOG_WARN("SD DMA bench: DmaMalloc %u bytes failed", (unsigned int)chunk);
        DmaFree(buf);
        return;
    }
    (void)memset(buf, 0x5AU, chunk);

    (void)snprintf(path, sizeof(path), "%s/sdbnch.tmp", s_mounted_path);
    (void)remove(path);

    /* 同一 "w+b" 句柄：避免 wb 关闭后再 rb 打开时目录/FAT 与介质未完全同步导致读路径极慢或隐式重试。 */
    fp = fopen(path, "w+b");
    if (fp == NULL) {
        LOG_WARN("SD DMA bench: fopen w+b failed");
        DmaFree(buf);
        return;
    }
    (void)setvbuf(fp, NULL, _IOFBF, (size_t)8192U);

    t0 = esp_timer_get_time();
    for (loop_ix = 0; loop_ix < SDCARD_BENCH_FAT_LOOPS; loop_ix++) {
        if (fwrite(buf, 1U, chunk, fp) != chunk) {
            LOG_WARN("SD DMA bench: FAT fwrite failed at %u", (unsigned int)loop_ix);
            break;
        }
    }
    if (fflush(fp) != 0) {
        LOG_WARN("SD DMA bench: fflush before fsync failed");
    }
    if (fsync(fileno(fp)) != 0) {
        LOG_WARN("SD DMA bench: fsync failed (errno=%d)", (int)errno);
    }
    dt_us = esp_timer_get_time() - t0;
    if ((loop_ix == SDCARD_BENCH_FAT_LOOPS) && (dt_us > 0)) {
        total_bytes = chunk * (size_t)SDCARD_BENCH_FAT_LOOPS;
        dt_s = (double)dt_us / 1000000.0;
        mib_fat_wr = ((double)total_bytes / (1024.0 * 1024.0)) / dt_s;
    }

    rewind(fp);

    t0 = esp_timer_get_time();
    for (loop_ix = 0; loop_ix < SDCARD_BENCH_FAT_LOOPS; loop_ix++) {
        if (fread(buf, 1U, chunk, fp) != chunk) {
            int fe = ferror(fp);
            LOG_WARN("SD DMA bench: FAT fread failed at %u ferror=%d errno=%d", (unsigned int)loop_ix, fe, (int)errno);
            clearerr(fp);
            break;
        }
    }
    dt_us = esp_timer_get_time() - t0;
    if ((loop_ix == SDCARD_BENCH_FAT_LOOPS) && (dt_us > 0)) {
        total_bytes = chunk * (size_t)SDCARD_BENCH_FAT_LOOPS;
        dt_s = (double)dt_us / 1000000.0;
        mib_fat_rd = ((double)total_bytes / (1024.0 * 1024.0)) / dt_s;
    }

    (void)fclose(fp);
    fp = NULL;
    (void)remove(path);

    LOG_INFO(
        "SD DMA throughput (IDMAC): FAT seq write+fsync %.2f MiB/s | FAT seq read %.2f MiB/s | host real_freq=%d kHz %u-bit",
        mib_fat_wr,
        mib_fat_rd,
        card->real_freq_khz,
        (unsigned int)(1U << (unsigned)card->log_bus_width));

    DmaFree(buf);
#undef SDCARD_BENCH_CHUNK_BYTES
#undef SDCARD_BENCH_FAT_LOOPS
#endif
}
