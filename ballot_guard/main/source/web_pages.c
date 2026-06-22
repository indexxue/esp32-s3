/**
 * @file web_pages.c
 * @brief ballot_guard 工程级 Web 页面 handler。
 */

#include "web_pages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "vote_menu_demo.h"
#include "vote_menu_pages.h"
#include "vote_nvs.h"
#include "vote_status.h"
#include "vote_history.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define VOTE_STATUS_JSON_BUF (2048U)
#define VOTE_HIST_JSON_BUF (4096U)
#define VOTE_CAND_JSON_BUF (512U)
#define VOTE_CAND_POST_BUF (512U)
#define VOTE_SETTINGS_JSON_BUF (512U)
#define VOTE_SETTINGS_POST_BUF (256U)

static bool json_extract_quoted(const char *body, const char *key, char *out, size_t out_cap)
{
    const char *p;
    const char *start;
    const char *end;
    size_t len;

    if (body == NULL || key == NULL || out == NULL || out_cap == 0U) {
        return false;
    }

    p = strstr(body, key);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    start = p + 1;
    end   = strchr(start, '"');
    if (end == NULL) {
        return false;
    }
    len = (size_t)(end - start);
    if (len + 1U > out_cap) {
        return false;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool json_extract_int(const char *body, const char *key, int *out)
{
    const char *p;
    char *endptr;
    long v;

    if (body == NULL || key == NULL || out == NULL) {
        return false;
    }

    p = strstr(body, key);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    v = strtol(p, &endptr, 10);
    if (endptr == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

static esp_err_t http_read_post_body(httpd_req_t *req, char *body, size_t body_cap)
{
    size_t total = 0U;
    int rlen;

    if (req == NULL || body == NULL || body_cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0U) {
        body[0] = '\0';
        return ESP_OK;
    }
    if (req->content_len >= body_cap) {
        return ESP_ERR_NO_MEM;
    }

    while (total < req->content_len) {
        rlen = httpd_req_recv(req, body + total, req->content_len - total);
        if (rlen <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)rlen;
    }
    body[total] = '\0';
    return ESP_OK;
}

static bool vote_phase_allows_admin_edit(const char **phase_out)
{
    int cd = 0;
    const char *phase = vote_status_current_phase(&cd);

    if (phase_out != NULL) {
        *phase_out = phase;
    }
    if (phase == NULL) {
        return true;
    }
    return (strcmp(phase, "voting") != 0) && (strcmp(phase, "selecting") != 0) && (strcmp(phase, "cooldown") != 0);
}

static esp_err_t vote_admin_edit_conflict(httpd_req_t *req)
{
    (void)httpd_resp_set_status(req, "409 Conflict");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"voting_in_progress\"}", HTTPD_RESP_USE_STRLEN);
}

static const char *web_menu_err_to_str(vote_menu_web_err_e err)
{
    switch (err) {
    case VOTE_MENU_WEB_ERR_INVALID_TIME:
        return "invalid_time";
    case VOTE_MENU_WEB_ERR_COUNT_REDUCE:
        return "count_reduce_blocked";
    case VOTE_MENU_WEB_ERR_INVALID_ARG:
        return "invalid_arg";
    case VOTE_MENU_WEB_ERR_CLOCK:
        return "clock_failed";
    case VOTE_MENU_WEB_ERR_RESET:
        return "reset_failed";
    case VOTE_MENU_WEB_ERR_SAVE:
    default:
        return "save_failed";
    }
}

static esp_err_t vote_web_send_menu_err(httpd_req_t *req, vote_menu_web_err_e err)
{
    char json[96];
    const int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", web_menu_err_to_str(err));

    if (n <= 0 || (size_t)n >= sizeof(json)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save_failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (err == VOTE_MENU_WEB_ERR_INVALID_ARG || err == VOTE_MENU_WEB_ERR_INVALID_TIME ||
        err == VOTE_MENU_WEB_ERR_COUNT_REDUCE) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
    } else {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
    }
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t vote_status_get_handler(httpd_req_t *req)
{
    char json[VOTE_STATUS_JSON_BUF];
    const size_t n = vote_status_build_json(json, sizeof(json));

    if (n == 0U) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t vote_history_get_handler(httpd_req_t *req)
{
    char json[VOTE_HIST_JSON_BUF];
    const size_t n = vote_history_build_json(json, sizeof(json));

    if (n == 0U) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t vote_candidates_get_handler(httpd_req_t *req)
{
    char json[VOTE_CAND_JSON_BUF];
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    size_t n;
    size_t off = 0U;

    n = (size_t)snprintf(json, sizeof(json), "{\"ok\":true,\"count\":%u,\"names\":[", (unsigned)count);
    if (n <= 0U || n >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json overflow");
    }
    off = n;

    for (i = 0U; i < count; i++) {
        n = (size_t)snprintf(json + off, sizeof(json) - off, "%s\"%s\"", (i > 0U) ? "," : "",
                             vote_status_candidate_name(i));
        if (n <= 0U || (off + n >= sizeof(json))) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json overflow");
        }
        off += n;
    }

    n = (size_t)snprintf(json + off, sizeof(json) - off, "]}");
    if (n <= 0U || (off + n >= sizeof(json))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json overflow");
    }

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, off + n);
}

static esp_err_t vote_history_clear_post_handler(httpd_req_t *req)
{
    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }
    if (!vote_history_clear_all()) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"clear_failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_candidates_reset_post_handler(httpd_req_t *req)
{
    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }
    if (!vote_nvs_restore_default_candidate_names()) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"reset_failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static bool json_extract_name_at(const char *body, int index, char *out, size_t out_cap)
{
    const char *p = strstr(body, "\"names\"");
    const char *start;
    const char *end;
    int i;
    size_t len;

    if (body == NULL || out == NULL || out_cap == 0U) {
        return false;
    }

    if (p == NULL) {
        return false;
    }
    p = strchr(p, '[');
    if (p == NULL) {
        return false;
    }
    p++;

    for (i = 0; i < index; i++) {
        p = strchr(p, '"');
        if (p == NULL) {
            return false;
        }
        p++;
        end = strchr(p, '"');
        if (end == NULL) {
            return false;
        }
        p = strchr(end, ',');
        if (p == NULL && i + 1 < index) {
            return false;
        }
        if (p != NULL) {
            p++;
        }
    }

    start = strchr(p, '"');
    if (start == NULL) {
        return false;
    }
    start++;
    end = strchr(start, '"');
    if (end == NULL) {
        return false;
    }
    len = (size_t)(end - start);
    if (len + 1U > out_cap) {
        return false;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static esp_err_t vote_candidates_post_handler(httpd_req_t *req)
{
    char body[VOTE_CAND_POST_BUF];
    char name[VOTE_NVS_CAND_NAME_BUF];
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }

    for (i = 0U; i < count; i++) {
        if (!json_extract_name_at(body, (int)i, name, sizeof(name))) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_names\"}", HTTPD_RESP_USE_STRLEN);
        }
        if (!vote_nvs_validate_candidate_name(name)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_name\"}", HTTPD_RESP_USE_STRLEN);
        }
        if (!vote_nvs_set_candidate_name(i, name)) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save_failed\"}", HTTPD_RESP_USE_STRLEN);
        }
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_settings_get_handler(httpd_req_t *req)
{
    vote_menu_settings_t *st = vote_menu_settings();
    uint8_t ch = 0U;
    uint8_t cm = 0U;
    uint8_t cs = 0U;
    char json[VOTE_SETTINGS_JSON_BUF];
    int n;
    const bool editable = vote_phase_allows_admin_edit(NULL);

    if (st == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)vote_menu_web_read_clock(&ch, &cm, &cs);

    n = snprintf(json,
                 sizeof(json),
                 "{\"ok\":true,\"editable\":%s,"
                 "\"schedule\":{\"start_h\":%u,\"start_m\":%u,\"end_h\":%u,\"end_m\":%u},"
                 "\"candidate_count\":%u,\"cooldown_sec\":%u,"
                 "\"clock\":{\"hour\":%u,\"minute\":%u,\"second\":%u}}",
                 editable ? "true" : "false",
                 (unsigned)st->start_h,
                 (unsigned)st->start_m,
                 (unsigned)st->end_h,
                 (unsigned)st->end_m,
                 (unsigned)st->candidate_count,
                 (unsigned)st->cooldown_sec,
                 (unsigned)ch,
                 (unsigned)cm,
                 (unsigned)cs);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t vote_settings_schedule_post_handler(httpd_req_t *req)
{
    char body[VOTE_SETTINGS_POST_BUF];
    int sh = 0;
    int sm = 0;
    int eh = 0;
    int em = 0;
    vote_menu_web_err_e err;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }

    if (!json_extract_int(body, "\"start_h\"", &sh) || !json_extract_int(body, "\"start_m\"", &sm) ||
        !json_extract_int(body, "\"end_h\"", &eh) || !json_extract_int(body, "\"end_m\"", &em)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_arg\"}", HTTPD_RESP_USE_STRLEN);
    }

    err = vote_menu_web_save_schedule((uint8_t)sh, (uint8_t)sm, (uint8_t)eh, (uint8_t)em);
    if (err != VOTE_MENU_WEB_OK) {
        return vote_web_send_menu_err(req, err);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_settings_count_post_handler(httpd_req_t *req)
{
    char body[VOTE_SETTINGS_POST_BUF];
    int count = 0;
    vote_menu_web_err_e err;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }

    if (!json_extract_int(body, "\"candidate_count\"", &count)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_arg\"}", HTTPD_RESP_USE_STRLEN);
    }

    err = vote_menu_web_save_count((uint8_t)count);
    if (err != VOTE_MENU_WEB_OK) {
        return vote_web_send_menu_err(req, err);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_settings_cooldown_post_handler(httpd_req_t *req)
{
    char body[VOTE_SETTINGS_POST_BUF];
    int sec = 0;
    vote_menu_web_err_e err;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }

    if (!json_extract_int(body, "\"cooldown_sec\"", &sec)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_arg\"}", HTTPD_RESP_USE_STRLEN);
    }

    err = vote_menu_web_save_cooldown((uint8_t)sec);
    if (err != VOTE_MENU_WEB_OK) {
        return vote_web_send_menu_err(req, err);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_settings_clock_post_handler(httpd_req_t *req)
{
    char body[VOTE_SETTINGS_POST_BUF];
    int hour = 0;
    int minute = 0;
    int second = 0;
    vote_menu_web_err_e err;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!json_extract_int(body, "\"hour\"", &hour) || !json_extract_int(body, "\"minute\"", &minute) ||
        !json_extract_int(body, "\"second\"", &second)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_arg\"}", HTTPD_RESP_USE_STRLEN);
    }

    err = vote_menu_web_save_clock((uint8_t)hour, (uint8_t)minute, (uint8_t)second);
    if (err != VOTE_MENU_WEB_OK) {
        return vote_web_send_menu_err(req, err);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vote_settings_reset_post_handler(httpd_req_t *req)
{
    vote_menu_web_err_e err;

    if (!vote_phase_allows_admin_edit(NULL)) {
        return vote_admin_edit_conflict(req);
    }

    err = vote_menu_web_reset_data();
    if (err != VOTE_MENU_WEB_OK) {
        return vote_web_send_menu_err(req, err);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_pages_root_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);

    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, len);
}

esp_err_t web_pages_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/vote/status", .method = HTTP_GET, .handler = vote_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/vote/history", .method = HTTP_GET, .handler = vote_history_get_handler, .user_ctx = NULL},
        {.uri = "/api/vote/history/clear", .method = HTTP_POST, .handler = vote_history_clear_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/candidates", .method = HTTP_GET, .handler = vote_candidates_get_handler, .user_ctx = NULL},
        {.uri = "/api/vote/candidates", .method = HTTP_POST, .handler = vote_candidates_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/candidates/reset", .method = HTTP_POST, .handler = vote_candidates_reset_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings", .method = HTTP_GET, .handler = vote_settings_get_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings/schedule", .method = HTTP_POST, .handler = vote_settings_schedule_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings/count", .method = HTTP_POST, .handler = vote_settings_count_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings/cooldown", .method = HTTP_POST, .handler = vote_settings_cooldown_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings/clock", .method = HTTP_POST, .handler = vote_settings_clock_post_handler, .user_ctx = NULL},
        {.uri = "/api/vote/settings/reset", .method = HTTP_POST, .handler = vote_settings_reset_post_handler, .user_ctx = NULL},
    };
    esp_err_t err;
    size_t i;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0U; i < (sizeof(uris) / sizeof(uris[0])); i++) {
        err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
