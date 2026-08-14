idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/ui.c"
        "source/audio.c"
        "source/agent.c"
        "source/pet_opus.c"
        "source/web_pages.c"
        "source/web_skin.c"
        "source/pet/pet_core/pet_core.c"
        "source/pet/pet_fs.c"
        "source/pet/sd_cfg.c"
        "source/pet/pet_res/pet_res.c"
        "source/pet/pet_view/pet_view.c"
    INCLUDE_DIRS
        "."
        "source"
        "source/pet"
        "source/pet/pet_core"
        "source/pet/pet_res"
        "source/pet/pet_view"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash lvgl esp_driver_i2s
                 esp_websocket_client json esp_wifi 78__esp-opus
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=4
    LED_SCENE_BRIGHTNESS_SCALE=128)
