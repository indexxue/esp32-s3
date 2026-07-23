# camera 主组件：LCD 预览 + SoftAP/STA 网页图传（本阶段不含端侧检测模型）。
# 钢珠检测模型待 Phase 4 再接入（见 camera/plan/steel_ball_gimbal_plan.md）。

idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/camera_ui.c"
        "source/camera_sensor.c"
        "source/web_pages.c"
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash esp_new_jpeg
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=1
    LED_SCENE_BRIGHTNESS_SCALE=128)
