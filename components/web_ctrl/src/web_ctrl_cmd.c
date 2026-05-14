/**
 * @file web_ctrl_cmd.c
 * @brief 单槽同步执行：`web_ctrl_cmd` 任务调用 `cmd_process_line_for_web`。
 */

#include "web_ctrl_cmd.h"

#include "cmd.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "web_ctrl_cmd";

#define WEB_CTRL_CMD_TASK_STACK (8192U)
#define WEB_CTRL_CMD_TASK_PRIO  (5U)

typedef struct {
    char line[CMD_LINE_MAX];
    char reply[WEB_CTRL_CMD_REPLY_MAX];
} web_ctrl_cmd_slot_t;

static web_ctrl_cmd_slot_t s_slot;
static SemaphoreHandle_t s_slot_idle;
static SemaphoreHandle_t s_slot_done;
static QueueHandle_t     s_wake;
static TaskHandle_t      s_task;

#define WAKE_POISON (0xFFU)

static void web_ctrl_cmd_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint8_t op = 0U;

        if (xQueueReceive(s_wake, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (op == WAKE_POISON) {
            break;
        }

        cmd_embed_register_defaults_if_needed();

        {
            cmd_web_capture_t cap = {.buf = s_slot.reply, .cap = WEB_CTRL_CMD_REPLY_MAX, .len = 0U};

            cmd_process_line_for_web(s_slot.line, &cap);
        }

        (void)xSemaphoreGive(s_slot_done);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t web_ctrl_cmd_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_slot_idle = xSemaphoreCreateBinary();
    s_slot_done = xSemaphoreCreateBinary();
    if ((s_slot_idle == NULL) || (s_slot_done == NULL)) {
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreGive(s_slot_idle);

    s_wake = xQueueCreate(1U, sizeof(uint8_t));
    if (s_wake == NULL) {
        vSemaphoreDelete(s_slot_idle);
        vSemaphoreDelete(s_slot_done);
        s_slot_idle = NULL;
        s_slot_done = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(web_ctrl_cmd_task, "web_ctrl_cmd", WEB_CTRL_CMD_TASK_STACK, NULL, WEB_CTRL_CMD_TASK_PRIO,
                    &s_task) != pdPASS) {
        vQueueDelete(s_wake);
        vSemaphoreDelete(s_slot_idle);
        vSemaphoreDelete(s_slot_done);
        s_wake      = NULL;
        s_slot_idle = NULL;
        s_slot_done = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "cmd executor started");
    return ESP_OK;
}

void web_ctrl_cmd_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    {
        const uint8_t poison = WAKE_POISON;
        (void)xQueueSend(s_wake, &poison, pdMS_TO_TICKS(2000));
    }

    for (int i = 0; i < 3000; i++) {
        if (s_task == NULL) {
            break;
        }
        vTaskDelay(1);
    }

    if (s_wake != NULL) {
        vQueueDelete(s_wake);
        s_wake = NULL;
    }
    if (s_slot_done != NULL) {
        vSemaphoreDelete(s_slot_done);
        s_slot_done = NULL;
    }
    if (s_slot_idle != NULL) {
        vSemaphoreDelete(s_slot_idle);
        s_slot_idle = NULL;
    }

    ESP_LOGI(TAG, "cmd executor stopped");
}

esp_err_t web_ctrl_cmd_execute_sync(const char *line, char *reply_out, size_t reply_cap, size_t *out_len,
                                    int timeout_ms)
{
    if ((line == NULL) || (reply_out == NULL) || (reply_cap == 0U) || (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_task == NULL) || (s_wake == NULL) || (s_slot_idle == NULL) || (s_slot_done == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_slot_idle, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)strncpy(s_slot.line, line, CMD_LINE_MAX - 1U);
    s_slot.line[CMD_LINE_MAX - 1U] = '\0';
    s_slot.reply[0]                = '\0';

    {
        const uint8_t ping = 1U;
        if (xQueueSend(s_wake, &ping, pdMS_TO_TICKS(500)) != pdTRUE) {
            (void)xSemaphoreGive(s_slot_idle);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (xSemaphoreTake(s_slot_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)xSemaphoreTake(s_slot_done, portMAX_DELAY);
        (void)xSemaphoreGive(s_slot_idle);
        return ESP_ERR_TIMEOUT;
    }

    {
        size_t n = strnlen(s_slot.reply, WEB_CTRL_CMD_REPLY_MAX);
        if (n >= reply_cap) {
            n = reply_cap - 1U;
        }
        (void)memcpy(reply_out, s_slot.reply, n);
        reply_out[n] = '\0';
        *out_len     = n;
    }

    (void)xSemaphoreGive(s_slot_idle);
    return ESP_OK;
}
