idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/desktop_pet_ui.c"
        "source/desktop_pet_audio.c"
        "source/desktop_pet_agent.c"
        "source/desktop_pet_opus.c"
        "source/web_pages.c"
        "../pet_core/pet_core.c"
        "../pet_fs.c"
        "../pet_res/pet_res.c"
        "../pet_view/pet_view.c"
    INCLUDE_DIRS
        "."
        "source"
        ".."
        "../pet_core"
        "../pet_res"
        "../pet_view"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash lvgl esp_driver_i2s
                 esp_websocket_client json esp_wifi 78__esp-opus
)

# 自检参考程序（默认不编）：source/desktop_pet_selftest.c — 恢复见该文件头注释 / README.txt

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=4
    LED_SCENE_BRIGHTNESS_SCALE=128)
