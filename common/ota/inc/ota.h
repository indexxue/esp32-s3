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

#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** OTA 会话状态（与 HTTP `/api/ota/status` 字段一致）。 */
typedef enum {
    OTA_SESSION_IDLE = 0,
    OTA_SESSION_WRITING,
    OTA_SESSION_READY,
    OTA_SESSION_PULLING,
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

/** semver 三段比较：new 严格大于 cur 时返回 true。 */
bool ota_version_is_greater(const char *new_ver, const char *cur_ver);

/**
 * @brief 设置下一会话写入完成后须匹配的 SHA256（hex64）；空串或 NULL 表示不校验。
 * @note 须在 `ota_upload_begin` 之前调用；会话 abort/end 后清除。
 */
status_t ota_upload_set_expected_sha256_hex(const char *sha256_hex);

typedef struct {
    char manifest_url[512];
    char product[32];
    bool apply_after_pull;
} ota_pull_request_t;

/** 后台从 manifest URL 拉取镜像并写入对侧槽（单任务；进行中返回 ESP_ERR_INVALID_STATE）。 */
status_t ota_pull_start(const ota_pull_request_t *req);

/** 同步拉取（须在独立任务中调用，勿阻塞 HTTP 处理线程）。 */
status_t ota_pull_from_manifest(const ota_pull_request_t *req);

/** 云端拉取 transport 用：idle → pulling 标记。 */
status_t ota_session_begin_cloud_pull(void);

/** 清除 pulling 标记。 */
void ota_session_end_cloud_pull(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_OTA_H */
