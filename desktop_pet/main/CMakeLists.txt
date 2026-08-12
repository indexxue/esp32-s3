idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/desktop_pet_selftest.c"
    INCLUDE_DIRS "." "source"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=4
    LED_SCENE_BRIGHTNESS_SCALE=128)
