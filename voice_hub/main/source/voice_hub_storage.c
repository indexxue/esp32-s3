#include "voice_hub_storage.h"

#include "voice_hub_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"

#include "board.h"
#include "log.h"
#include "sdcard.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VOICE_HUB_SDCARD_MOUNT_RETRY_MAX (3U)
#define VOICE_HUB_SDCARD_SETTLE_MS (250U)
#define VOICE_HUB_SDCARD_RW_RETRY_MAX (2U)

static const char *s_boot_status_line = NULL;

const char *voice_hub_storage_boot_status_line(void)
{
    return s_boot_status_line;
}

static void voice_hub_storage_set_boot_status(const char *line)
{
    s_boot_status_line = line;
}

static bool_t voice_hub_storage_fat_probe(void)
{
    DIR *dir = opendir(BOARD_SDCARD_MOUNT_POINT);

    if (dir == NULL) {
        LOG_WARN("SD FAT probe: opendir(%s) failed, errno=%d (%s)",
                 BOARD_SDCARD_MOUNT_POINT,
                 (int)errno,
                 strerror(errno));
        return FALSE;
    }
    if (readdir(dir) == NULL) {
        LOG_WARN("SD FAT probe: readdir(%s) failed, errno=%d (%s)",
                 BOARD_SDCARD_MOUNT_POINT,
                 (int)errno,
                 strerror(errno));
        (void)closedir(dir);
        return FALSE;
    }
    (void)closedir(dir);
    return TRUE;
}

static status_t voice_hub_storage_raw_sector_probe(void)
{
    sdmmc_card_t *card = sdcard_get_card();
    uint8_t *sector = NULL;
    esp_err_t err;

    if (card == NULL) {
        return STATUS_FAIL;
    }

    sector = (uint8_t *)heap_caps_malloc(512U, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sector == NULL) {
        sector = (uint8_t *)heap_caps_malloc(512U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (sector == NULL) {
        LOG_WARN("SD raw probe: alloc 512 bytes failed");
        return STATUS_FAIL;
    }

    err = sdmmc_read_sectors(card, sector, 0U, 1U);
    if (err != ESP_OK) {
        LOG_ERROR("SD raw probe: read sector 0 failed: %s", esp_err_to_name(err));
        heap_caps_free(sector);
        return STATUS_FAIL;
    }

    LOG_INFO("SD raw probe: sector 0 OK (sig 0x%02x%02x)", (unsigned int)sector[510], (unsigned int)sector[511]);
    heap_caps_free(sector);
    return STATUS_OK;
}

static void voice_hub_storage_log_card_info(void)
{
    sdmmc_card_t *card = sdcard_get_card();

    if (card != NULL) {
        sdmmc_card_print_info(stdout, card);
    }
}

static status_t voice_hub_storage_try_existing_mount(void)
{
    if (sdcard_get_card() == NULL) {
        return STATUS_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(VOICE_HUB_SDCARD_SETTLE_MS));
    if (!voice_hub_storage_fat_probe()) {
        return STATUS_FAIL;
    }
    voice_hub_storage_log_card_info();
    return STATUS_OK;
}

static status_t voice_hub_storage_force_remount(void)
{
    if (sdcard_get_card() != NULL) {
        (void)sdcard_unmount(BOARD_SDCARD_MOUNT_POINT);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (sdcard_mount(BOARD_SDCARD_MOUNT_POINT) != STATUS_OK) {
        return STATUS_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(VOICE_HUB_SDCARD_SETTLE_MS));
    if (!voice_hub_storage_fat_probe()) {
        return STATUS_FAIL;
    }
    voice_hub_storage_log_card_info();
    return STATUS_OK;
}

static status_t voice_hub_storage_mount_with_retry(void)
{
    uint32_t attempt;

    for (attempt = 0U; attempt < VOICE_HUB_SDCARD_MOUNT_RETRY_MAX; attempt++) {
        if (voice_hub_storage_force_remount() == STATUS_OK) {
            LOG_INFO("SD storage ready at %s (attempt %u)", BOARD_SDCARD_MOUNT_POINT, (unsigned)(attempt + 1U));
            return STATUS_OK;
        }
        LOG_WARN("SD mount attempt %u/%u failed", (unsigned)(attempt + 1U), (unsigned)VOICE_HUB_SDCARD_MOUNT_RETRY_MAX);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return STATUS_FAIL;
}

static status_t voice_hub_storage_rw_test_once(void)
{
    char path[48];
    FILE *fp;
    const char expect[] = "voice_hub_sd_ok\n";
    char line[32];

    (void)snprintf(path, sizeof(path), "%s/sd_test.txt", BOARD_SDCARD_MOUNT_POINT);
    fp = fopen(path, "w");
    if (fp == NULL) {
        LOG_ERROR("SD rw test: fopen write \"%s\" failed, errno=%d (%s)", path, (int)errno, strerror(errno));
        return STATUS_FAIL;
    }
    if (fputs(expect, fp) == EOF) {
        LOG_ERROR("SD rw test: fputs failed, errno=%d (%s)", (int)errno, strerror(errno));
        (void)fclose(fp);
        return STATUS_FAIL;
    }
    if (fflush(fp) != 0) {
        LOG_ERROR("SD rw test: fflush failed, errno=%d (%s)", (int)errno, strerror(errno));
        (void)fclose(fp);
        return STATUS_FAIL;
    }
    if (fsync(fileno(fp)) != 0) {
        LOG_ERROR("SD rw test: fsync failed, errno=%d (%s)", (int)errno, strerror(errno));
        (void)fclose(fp);
        return STATUS_FAIL;
    }
    (void)fclose(fp);

    fp = fopen(path, "r");
    if (fp == NULL) {
        LOG_ERROR("SD rw test: fopen read \"%s\" failed, errno=%d (%s)", path, (int)errno, strerror(errno));
        return STATUS_FAIL;
    }
    (void)memset(line, 0, sizeof(line));
    if (fgets(line, (int)sizeof(line), fp) == NULL) {
        LOG_ERROR("SD rw test: fgets failed, errno=%d (%s)", (int)errno, strerror(errno));
        (void)fclose(fp);
        return STATUS_FAIL;
    }
    (void)fclose(fp);

    if (strcmp(line, expect) != 0) {
        LOG_ERROR("SD rw test: verify mismatch got \"%s\"", line);
        return STATUS_FAIL;
    }

    LOG_INFO("SD rw test: PASS (w/r %s)", path);
    return STATUS_OK;
}

status_t voice_hub_storage_init(void)
{
#if !VOICE_HUB_ENABLE_SDCARD
    LOG_INFO("voice_hub SD disabled (VOICE_HUB_ENABLE_SDCARD=0)");
    voice_hub_storage_set_boot_status(NULL);
    return STATUS_OK;
#else
    if (voice_hub_storage_try_existing_mount() == STATUS_OK) {
        LOG_INFO("SD storage ready at %s (BoardInit)", BOARD_SDCARD_MOUNT_POINT);
        return STATUS_OK;
    }
    if (voice_hub_storage_mount_with_retry() == STATUS_OK) {
        return STATUS_OK;
    }
    voice_hub_storage_set_boot_status("SD mount FAIL");
    return STATUS_FAIL;
#endif
}

bool_t voice_hub_storage_is_mounted(void)
{
#if !VOICE_HUB_ENABLE_SDCARD
    return FALSE;
#else
    return sdcard_get_card() != NULL ? TRUE : FALSE;
#endif
}

status_t voice_hub_storage_run_rw_test(void)
{
#if !VOICE_HUB_ENABLE_SDCARD
    return STATUS_FAIL;
#else
    uint32_t attempt;

    if (!voice_hub_storage_is_mounted()) {
        LOG_ERROR("SD rw test: not mounted");
        voice_hub_storage_set_boot_status("SD mount FAIL");
        return STATUS_FAIL;
    }

    if (voice_hub_storage_raw_sector_probe() != STATUS_OK) {
        voice_hub_storage_set_boot_status("SD raw read FAIL");
        return STATUS_FAIL;
    }

    for (attempt = 0U; attempt < VOICE_HUB_SDCARD_RW_RETRY_MAX; attempt++) {
        if (voice_hub_storage_rw_test_once() == STATUS_OK) {
            sdcard_dma_throughput_benchmark_log();
            voice_hub_storage_set_boot_status("SD rw test OK");
            return STATUS_OK;
        }
        if ((attempt + 1U) < VOICE_HUB_SDCARD_RW_RETRY_MAX) {
            LOG_WARN("SD rw test: retry after remount (%u/%u)",
                     (unsigned)(attempt + 2U),
                     (unsigned)VOICE_HUB_SDCARD_RW_RETRY_MAX);
            if (voice_hub_storage_force_remount() != STATUS_OK) {
                break;
            }
        }
    }
    voice_hub_storage_set_boot_status("SD rw test FAIL");
    return STATUS_FAIL;
#endif
}

status_t voice_hub_storage_write_jpeg_snapshot(const uint8_t *data, uint32_t len)
{
#if !VOICE_HUB_ENABLE_SDCARD
    (void)data;
    (void)len;
    return STATUS_FAIL;
#else
    (void)data;
    (void)len;
    /* TODO(M3): 写入 /sdcard/capture/YYYYMMDD_HHMMSS.jpg */
    LOG_WARN("voice_hub_storage_write_jpeg_snapshot: not implemented");
    return STATUS_FAIL;
#endif
}
