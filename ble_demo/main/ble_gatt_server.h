/**
 * @file ble_gatt_server.h
 * @brief ESP32-S3 BLE GATT Server 初始化与广播。
 */

#ifndef BLE_GATT_SERVER_H
#define BLE_GATT_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 BLE 控制器、Bluedroid 栈并注册 GATT 服务。 */
esp_err_t ble_gatt_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_SERVER_H */
