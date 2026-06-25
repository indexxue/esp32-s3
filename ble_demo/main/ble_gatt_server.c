/**
 * @file ble_gatt_server.c
 * @brief 单服务 BLE GATT Server：可读/可写/可 Notify 的自定义特征值。
 */

#include "ble_gatt_server.h"

#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_led.h"

#define TAG "ble_gatt"

#define BLE_DEVICE_NAME "ESP32-S3-BLE-Demo"

#define GATTS_APP_ID 0
#define GATTS_SERVICE_UUID 0xFFF0
#define GATTS_CHAR_UUID 0xFFF1
#define GATTS_NUM_HANDLE 4

#define ADV_CONFIG_FLAG (1 << 0)

static uint16_t s_conn_id = 0;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_service_handle = 0;
static uint16_t s_char_handle = 0;
static uint16_t s_cccd_handle = 0;
static bool s_notify_enabled = false;
static bool s_service_ready = false;
static uint8_t s_adv_config_done = 0;

static uint8_t s_char_value[20] = "hello ble";
static const uint16_t s_char_value_len = 9;

static esp_attr_value_t s_char_attr = {
    .attr_max_len = sizeof(s_char_value),
    .attr_len = s_char_value_len,
    .attr_value = s_char_value,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void send_write_response(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
    }
}

static void start_advertising_if_ready(void)
{
    if (s_adv_config_done == 0) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_config_done &= ~ADV_CONFIG_FLAG;
        start_advertising_if_ready();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "Conn params: int=%d latency=%d timeout=%d",
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATT register, status=%d app_id=%d", param->reg.status, param->reg.app_id);
        s_gatts_if = gatts_if;

        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);

        esp_gatt_srvc_id_t service_id = {
            .is_primary = true,
            .id = {
                .inst_id = 0,
                .uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = {.uuid16 = GATTS_SERVICE_UUID},
                },
            },
        };

        s_adv_config_done |= ADV_CONFIG_FLAG;
        esp_err_t ret = esp_ble_gap_config_adv_data(&s_adv_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "config adv data failed: %s", esp_err_to_name(ret));
        }

        esp_ble_gatts_create_service(gatts_if, &service_id, GATTS_NUM_HANDLE);
        break;

    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(TAG, "Service created, handle=%d", param->create.service_handle);
        s_service_handle = param->create.service_handle;

        esp_bt_uuid_t char_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = {.uuid16 = GATTS_CHAR_UUID},
        };

        esp_ble_gatts_start_service(s_service_handle);
        ret = esp_ble_gatts_add_char(
            s_service_handle,
            &char_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
            &s_char_attr,
            NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "add char failed: %s", esp_err_to_name(ret));
        }
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        ESP_LOGI(TAG, "Characteristic added, handle=%d", param->add_char.attr_handle);
        s_char_handle = param->add_char.attr_handle;

        esp_bt_uuid_t cccd_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG},
        };
        esp_ble_gatts_add_char_descr(
            s_service_handle,
            &cccd_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            NULL,
            NULL);
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        ESP_LOGI(TAG, "CCCD added, handle=%d", param->add_char_descr.attr_handle);
        s_cccd_handle = param->add_char_descr.attr_handle;
        s_service_ready = true;
        break;

    case ESP_GATTS_READ_EVT: {
        esp_gatt_rsp_t rsp = {0};
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = s_char_attr.attr_len;
        memcpy(rsp.attr_value.value, s_char_value, s_char_attr.attr_len);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
        break;
    }

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_cccd_handle && param->write.len == 2) {
            uint16_t cccd = (uint16_t)(param->write.value[1] << 8 | param->write.value[0]);
            s_notify_enabled = (cccd == 0x0001);
            ESP_LOGI(TAG, "Notify %s", s_notify_enabled ? "enabled" : "disabled");
        } else if (param->write.handle == s_char_handle) {
            uint16_t copy_len = param->write.len;
            if (copy_len > sizeof(s_char_value)) {
                copy_len = sizeof(s_char_value);
            }
            memcpy(s_char_value, param->write.value, copy_len);
            s_char_attr.attr_len = copy_len;
            ESP_LOGI(TAG, "Characteristic written, len=%u", copy_len);
            ESP_LOG_BUFFER_HEX(TAG, s_char_value, copy_len);
            ble_led_on_write(param->write.value, copy_len);
        }
        send_write_response(gatts_if, param);
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "Connected, conn_id=%u, remote " ESP_BD_ADDR_STR,
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        ble_led_on_connected();
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Disconnected, remote " ESP_BD_ADDR_STR ", reason=0x%02x",
                 ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
        s_notify_enabled = false;
        ble_led_on_disconnected();
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT && param->reg.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "GATT app register failed, status=%d", param->reg.status);
        return;
    }

    gatts_profile_event_handler(event, gatts_if, param);
}

static void notify_task(void *arg)
{
    uint32_t counter = 0;

    (void)arg;

    while (1) {
        if (s_service_ready && s_notify_enabled && s_gatts_if != ESP_GATT_IF_NONE) {
            uint8_t payload[8];
            payload[0] = (uint8_t)(counter & 0xFF);
            payload[1] = (uint8_t)((counter >> 8) & 0xFF);
            payload[2] = (uint8_t)((counter >> 16) & 0xFF);
            payload[3] = (uint8_t)((counter >> 24) & 0xFF);
            esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_char_handle, sizeof(payload), payload, false);
            counter++;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t ble_gatt_server_start(void)
{
    esp_err_t ret;

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gatts_app_register(GATTS_APP_ID);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gatt_set_local_mtu(517);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set local MTU failed: %s", esp_err_to_name(ret));
    }

    xTaskCreate(notify_task, "ble_notify", 3072, NULL, 5, NULL);

    return ESP_OK;
}
