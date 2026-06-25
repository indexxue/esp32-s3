/**
 * @file main.c
 * @brief ESP32-S3 BLE 演示工程入口。
 *
 * ESP32-S3 仅支持 BLE（低功耗蓝牙），不支持经典蓝牙 BR/EDR。
 */

#include "ble_gatt_server.h"
#include "ble_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ble_demo";

static void bootup_to_pairing_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500));
    ble_led_on_advertising();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "ESP32-S3 BLE GATT Server demo starting");

    if (ble_led_init() != STATUS_OK) {
        ESP_LOGE(TAG, "RGB init failed");
        return;
    }

    xTaskCreate(bootup_to_pairing_task, "ble_led_boot", 2048, NULL, 4, NULL);

    err = ble_gatt_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "BLE ready — scan for device name \"ESP32-S3-BLE-Demo\"");
}
