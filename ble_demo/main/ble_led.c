/**
 * @file ble_led.c
 */

#include "ble_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_scene.h"

static const char *TAG = "ble_led";

status_t ble_led_init(void)
{
    status_t err = led_scene_init();
    if (err != STATUS_OK) {
        ESP_LOGE(TAG, "led_scene_init failed: %s", status_to_str(err));
        return err;
    }

    err = led_scene_start_update_task();
    if (err != STATUS_OK) {
        ESP_LOGE(TAG, "led_scene_start_update_task failed: %s", status_to_str(err));
        return err;
    }

    led_scene_run(LED_SCENE_ID_BOOTUP);
    ESP_LOGI(TAG, "bootup scene started (GPIO48 WS2812 x16)");

    return STATUS_OK;
}

void ble_led_on_advertising(void)
{
    led_scene_run_force(LED_SCENE_ID_PAIRING);
    ESP_LOGI(TAG, "pairing scene (waiting for BLE connection)");
}

void ble_led_on_connected(void)
{
    led_scene_run_force(LED_SCENE_ID_WORKING);
    ESP_LOGI(TAG, "working scene (rainbow, connected)");
}

void ble_led_on_disconnected(void)
{
    ble_led_on_advertising();
}

static void ble_led_run_scene_id(led_scene_id_e id)
{
    led_scene_run_force(id);
    ESP_LOGI(TAG, "scene %d", (int)id);
}

void ble_led_on_write(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    if (len == 1) {
        switch (data[0]) {
        case '0':
            ble_led_run_scene_id(LED_SCENE_ID_BOOTUP);
            break;
        case '1':
            ble_led_run_scene_id(LED_SCENE_ID_PAIRING);
            break;
        case '2':
            ble_led_run_scene_id(LED_SCENE_ID_TRIGGER);
            break;
        case '3':
            ble_led_run_scene_id(LED_SCENE_ID_SUCCESS);
            break;
        case '4':
            ble_led_run_scene_id(LED_SCENE_ID_WORKING);
            break;
        case '5':
            ble_led_run_scene_id(LED_SCENE_ID_ERROR);
            break;
        default:
            ble_led_run_scene_id(LED_SCENE_ID_TRIGGER);
            break;
        }
        return;
    }

    ble_led_run_scene_id(LED_SCENE_ID_TRIGGER);
}
