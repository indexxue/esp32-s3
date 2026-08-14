/**
 * @file web_skin.h
 * @brief Web zip skin replace: staging dir + boot commit + POST /api/pet/skin.
 */

#ifndef WEB_SKIN_H
#define WEB_SKIN_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** If `/sdcard/.pet_next/pack.bin` exists, replace content root (`pet/`). Call after SD mount. */
void web_skin_commit_pending(void);

/** `POST /api/pet/skin` — raw zip body, then delayed reboot. */
esp_err_t web_skin_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SKIN_H */
