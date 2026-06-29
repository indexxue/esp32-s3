#include "voice_hub_storage.h"

#include "voice_hub_config.h"

#include "board.h"
#include "log.h"
#include "sdcard.h"

status_t voice_hub_storage_init(void)
{
#if !VOICE_HUB_ENABLE_SDCARD
    LOG_INFO("voice_hub SD disabled (VOICE_HUB_ENABLE_SDCARD=0)");
    return STATUS_OK;
#else
    if (sdcard_get_card() != NULL) {
        LOG_INFO("SD storage ready at %s", BOARD_SDCARD_MOUNT_POINT);
        return STATUS_OK;
    }
    if (sdcard_mount(BOARD_SDCARD_MOUNT_POINT) != STATUS_OK) {
        LOG_WARN("SD card mount failed at %s (check 1-bit pins / card)", BOARD_SDCARD_MOUNT_POINT);
        return STATUS_FAIL;
    }
    LOG_INFO("SD storage ready at %s", BOARD_SDCARD_MOUNT_POINT);
    return STATUS_OK;
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
