idf_component_register(
    SRCS
        "src/net_wifi.c"
        "src/web_ctrl_cmd.c"
        "src/web_bmp_upload.c"
        "src/web_video.c"
        "src/web_server.c"
        "src/web_ctrl_wifi_api.c"
        "src/web_ctrl.c"
    INCLUDE_DIRS "inc"
    REQUIRES common bsp_driver esp_netif esp_wifi esp_event nvs_flash esp_http_server esp_timer
)
