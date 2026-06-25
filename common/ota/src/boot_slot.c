/**
 * @file boot_slot.c
 * @brief 双槽启动意图：仅通过 ESP-IDF OTA API 更新启动分区元数据。
 */

#include "boot_slot.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include <stdio.h>
#include <string.h>

static const esp_partition_t *boot_slot_find_ota(esp_partition_subtype_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
}

status_t boot_slot_request_app_a(void)
{
    const esp_partition_t *p = boot_slot_find_ota(ESP_PARTITION_SUBTYPE_APP_OTA_0);

    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_ota_set_boot_partition(p);
}

status_t boot_slot_request_factory(void)
{
    const esp_partition_t *p = boot_slot_find_ota(ESP_PARTITION_SUBTYPE_APP_OTA_1);

    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_ota_set_boot_partition(p);
}

void boot_slot_system_reset(void)
{
    esp_restart();
}

status_t boot_slot_format_status(char *buf, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_boot_partition();
    const char *run_label = (run != NULL) ? run->label : "?";
    const char *next_label = (next != NULL) ? next->label : "?";

    if ((buf == NULL) || (cap == 0U)) {
        return STATUS_INVALID_ARG;
    }
    (void)snprintf(buf, cap, "run=%s next=%s", run_label, next_label);
    return STATUS_OK;
}
