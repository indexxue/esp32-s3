idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/voice_hub_ui.c"
        "source/voice_hub_camera.c"
        "source/voice_hub_storage.c"
        "source/web_pages.c"
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash esp_new_jpeg
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=1
    LED_SCENE_BRIGHTNESS_SCALE=128)
