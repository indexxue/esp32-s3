idf_component_register(
    SRCS "main.c" "start.c"
    INCLUDE_DIRS "."
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl
)
