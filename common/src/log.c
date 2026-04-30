#include "log.h"

#include "esp_log.h"

void common_log(const char_t *tag, const char_t *message)
{
    ESP_LOGI(tag, "%s", message);
}
