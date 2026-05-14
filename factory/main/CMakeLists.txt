idf_component_register(
    SRCS "main.c" "factory.c"
    INCLUDE_DIRS "."
    REQUIRES cbb common bsp_driver esp_timer app_update
)
