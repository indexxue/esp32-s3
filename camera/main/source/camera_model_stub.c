/**
 * @file camera_model_stub.c
 * @brief DETECT=0 时的 camera_model 空实现，避免链接 ball_detect。
 */

#include "camera_model.h"

#include "camera_app_config.h"

#if CAMERA_APP_DETECT_MODE
#error "camera_model_stub.c must not be built when CAMERA_APP_DETECT_MODE=1"
#endif

status_t camera_model_init(void)
{
    return STATUS_OK;
}

status_t camera_model_start(void)
{
    return STATUS_OK;
}

bool_t camera_model_is_ready(void)
{
    return FALSE;
}

status_t camera_model_set_enabled(bool_t on)
{
    (void)on;
    return STATUS_FAIL;
}

bool_t camera_model_is_enabled(void)
{
    return FALSE;
}

status_t camera_model_get_latest(camera_model_result_t *out)
{
    return camera_model_get_latest_ex(out, NULL);
}

status_t camera_model_get_latest_ex(camera_model_result_t *out, uint32_t *gen)
{
    if (out != NULL) {
        out->count = 0U;
        out->frame_w = 0U;
        out->frame_h = 0U;
    }
    if (gen != NULL) {
        *gen = 0U;
    }
    return STATUS_FAIL;
}

status_t camera_model_fill_ui_boxes(uint16_t *out_flat,
                                    uint8_t max_count,
                                    uint8_t *out_count,
                                    uint16_t *out_w,
                                    uint16_t *out_h)
{
    (void)out_flat;
    (void)max_count;
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (out_w != NULL) {
        *out_w = 0U;
    }
    if (out_h != NULL) {
        *out_h = 0U;
    }
    return STATUS_FAIL;
}
