idf_component_register(
    SRCS "main.c" "ble_gatt_server.c" "ble_led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt common
)
