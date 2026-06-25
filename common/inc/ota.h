/**
 * @file ota.h
 * @brief 双槽 OTA 写入与会话管理（`esp_ota_begin/write/end`），见 `doc/ota_development_plan.md`。
 */

#ifndef COMMON_OTA_H
#define COMMON_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

#include <stddef.h>
#include <stdint.h>

/** OTA 会话状态（与 HTTP `/api/ota/status` 字段一致）。 */
typedef enum {
    OTA_SESSION_IDLE = 0,
    OTA_SESSION_WRITING,
    OTA_SESSION_READY,
} ota_session_state_e;

typedef struct {
    ota_session_state_e state;
    size_t              expected_size;
    size_t              written;
    char                run_label[17];
    char                target_label[17];
    char                run_version[32];
    char                pending_version[32];
} ota_status_t;

/** 填充当前 OTA 会话与运行槽信息。 */
status_t ota_get_status(ota_status_t *out);

/**
 * @brief 开始写入对侧槽。
 * @param image_size  镜像总字节数（HTTP Content-Length）。
 * @param header_peek 首包数据（须含 `esp_app_desc` 魔数以便版本比较）。
 * @param header_len  `header_peek` 有效长度。
 */
status_t ota_upload_begin(size_t image_size, const uint8_t *header_peek, size_t header_len);

status_t ota_upload_write(const uint8_t *data, size_t len);

/** 完成写入并校验镜像；成功后进入 READY，可 `ota_apply`。 */
status_t ota_upload_end(void);

/** 放弃当前会话（S1）；WRITING 时 `esp_ota_abort`，READY 时仅清除待切换状态。 */
status_t ota_upload_abort(void);

/** READY 状态下切换启动分区并复位（S2）；失败时返回错误码。 */
status_t ota_apply(void);

/** 新固件启动并通过自检后调用，取消 rollback 待定状态（D3）。 */
status_t ota_confirm_running_image(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_OTA_H */
