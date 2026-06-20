/**
 * @file web_pages.c
 * @brief ballot_guard 工程级 Web 页面 handler。
 */

#include "web_pages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "vote_menu_config.h"
#include "vote_menu_demo.h"
#include "vote_status.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define VOTE_STATUS_JSON_BUF (2048U)
#define LCD_MENU_JSON_BUF (2048U)
#define LCD_MENU_POST_BUF (256U)

typedef struct {
    const char *key;
    const char *label;
    vote_lcd_screen_id_t screen;
} lcd_screen_def_t;

static const lcd_screen_def_t s_lcd_screens[] = {
    { "home", "主界面", VOTE_LCD_SCREEN_HOME },
    { "voting", "投票等待", VOTE_LCD_SCREEN_VOTING },
    { "select", "选人", VOTE_LCD_SCREEN_SELECT },
    { "cooldown", "冷却", VOTE_LCD_SCREEN_COOLDOWN },
    { "violation", "违规", VOTE_LCD_SCREEN_VIOLATION },
    { "locked", "结果锁定", VOTE_LCD_SCREEN_LOCKED },
    { "history", "历史记录", VOTE_LCD_SCREEN_HISTORY },
    { "admin", "管理员菜单", VOTE_LCD_SCREEN_ADMIN },
    { "admin_time", "投票时间", VOTE_LCD_SCREEN_ADMIN_TIME },
    { "admin_count", "候选人数量", VOTE_LCD_SCREEN_ADMIN_COUNT },
    { "admin_cooldown", "冷却时长", VOTE_LCD_SCREEN_ADMIN_COOLDOWN },
    { "admin_reset", "重置确认", VOTE_LCD_SCREEN_ADMIN_RESET },
};

static const lcd_screen_def_t *lcd_screen_by_key(const char *key)
{
    size_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0U; i < (sizeof(s_lcd_screens) / sizeof(s_lcd_screens[0])); i++) {
        if (strcmp(key, s_lcd_screens[i].key) == 0) {
            return &s_lcd_screens[i];
        }
    }
    return NULL;
}

static const char *lcd_screen_key_of(vote_lcd_screen_id_t id)
{
    size_t i;

    for (i = 0U; i < (sizeof(s_lcd_screens) / sizeof(s_lcd_screens[0])); i++) {
        if (s_lcd_screens[i].screen == id) {
            return s_lcd_screens[i].key;
        }
    }
    return "home";
}

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
    static const char json[] = "{\"ok\":true,\"records\":[]}";

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static size_t lcd_menu_build_json(char *json, size_t cap)
{
    const vote_lcd_screen_id_t cur = vote_menu_demo_current_screen();
    const char *cur_key            = lcd_screen_key_of(cur);
    size_t n                       = 0U;
    size_t i;

    if (json == NULL || cap == 0U) {
        return 0U;
    }

    n = (size_t)snprintf(json,
                         cap,
                         "{\"ok\":true,\"active\":%s,\"page\":\"%s\",\"page_id\":%d,\"screens\":[",
                         vote_menu_demo_is_active() ? "true" : "false",
                         cur_key,
                         (int)cur);

    for (i = 0U; i < (sizeof(s_lcd_screens) / sizeof(s_lcd_screens[0])); i++) {
        const lcd_screen_def_t *s = &s_lcd_screens[i];
        const int on              = (s->screen == cur) ? 1 : 0;

        n += (size_t)snprintf(json + n,
                              (n < cap) ? (cap - n) : 0U,
                              "%s{\"key\":\"%s\",\"label\":\"%s\",\"supported\":true,\"current\":%s}",
                              (i > 0U) ? "," : "",
                              s->key,
                              s->label,
                              on ? "true" : "false");
        if (n >= cap) {
            return 0U;
        }
    }

    n += (size_t)snprintf(json + n, (n < cap) ? (cap - n) : 0U, "]}");
    if (n >= cap) {
        return 0U;
    }
    return n;
}

static esp_err_t lcd_menu_send_ok(httpd_req_t *req)
{
    char json[96];
    const vote_lcd_screen_id_t cur = vote_menu_demo_current_screen();
    const char *key              = lcd_screen_key_of(cur);
    const int n                  = snprintf(json, sizeof(json), "{\"ok\":true,\"page\":\"%s\",\"page_id\":%d}", key,
                                              (int)cur);

    if (n <= 0 || (size_t)n >= sizeof(json)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t lcd_menu_get_handler(httpd_req_t *req)
{
    char json[LCD_MENU_JSON_BUF];
    const size_t n = lcd_menu_build_json(json, sizeof(json));

    if (n == 0U) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t lcd_menu_post_handler(httpd_req_t *req)
{
    char body[LCD_MENU_POST_BUF];
    char page_key[32];
    char event_key[16];
    int page_id                    = -1;
    const lcd_screen_def_t *screen = NULL;
    esp_err_t herr;

    herr = http_read_post_body(req, body, sizeof(body));
    if (herr != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_menu_demo_is_active()) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"lcd_inactive\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (json_extract_quoted(body, "\"event\"", event_key, sizeof(event_key))) {
        menu_evt_t evt = MENU_EVT_HOME;

        if (strcmp(event_key, "up") == 0 || strcmp(event_key, "nav_prev") == 0) {
            evt = MENU_EVT_UP;
        } else if (strcmp(event_key, "down") == 0 || strcmp(event_key, "nav_next") == 0) {
            evt = MENU_EVT_DOWN;
        } else if (strcmp(event_key, "enter") == 0 || strcmp(event_key, "confirm") == 0) {
            evt = MENU_EVT_ENTER;
        } else if (strcmp(event_key, "back") == 0) {
            evt = MENU_EVT_BACK;
        } else {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"unknown_event\"}", HTTPD_RESP_USE_STRLEN);
        }

        if (!vote_menu_demo_dispatch(evt)) {
            (void)httpd_resp_set_status(req, "409 Conflict");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"dispatch_ignored\"}", HTTPD_RESP_USE_STRLEN);
        }

        return lcd_menu_send_ok(req);
    }

    if (json_extract_quoted(body, "\"page\"", page_key, sizeof(page_key))) {
        screen = lcd_screen_by_key(page_key);
    } else if (json_extract_int(body, "\"page_id\"", &page_id)) {
        if (page_id >= (int)VOTE_LCD_SCREEN_HOME && page_id <= (int)VOTE_LCD_SCREEN_ADMIN_RESET) {
            screen = lcd_screen_by_key(lcd_screen_key_of((vote_lcd_screen_id_t)page_id));
        }
    }

    if (screen == NULL) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need page or page_id\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!vote_menu_demo_goto_screen(screen->screen)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"goto_failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    return lcd_menu_send_ok(req);
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
        {.uri = "/api/lcd/menu", .method = HTTP_GET, .handler = lcd_menu_get_handler, .user_ctx = NULL},
        {.uri = "/api/lcd/menu", .method = HTTP_POST, .handler = lcd_menu_post_handler, .user_ctx = NULL},
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
