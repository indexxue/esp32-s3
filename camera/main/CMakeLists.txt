# camera 主组件：LCD 预览 + SoftAP/STA 网页图传 + 钢珠 ESPDet 检测叠框。

idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/camera_ui.c"
        "source/camera_sensor.c"
        "source/camera_model.cpp"
        "source/web_pages.c"
        "source/servo_ctrl.c"
        "source/servo_calib_cmd.c"
        "source/servo_ball_follow.c"
        "source/camera_spi_host.c"
        "source/spi_link.c"
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html" "source/www/preview.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash esp_new_jpeg ball_detect
)

# 采数 / 校准 / 识别：改对应行后重编烧录即可（不必 fullclean）。
# COLLECT=1 采数（关 ESPDet）；CALIB=1 单舵机校准（关 ESPDet）；二者不可同时为 1。
# 均为 0 = 识别（开检测）。
set(CAMERA_APP_COLLECT_MODE 0)
set(CAMERA_APP_CALIB_MODE 0)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=1
    LED_SCENE_BRIGHTNESS_SCALE=128
    CAMERA_APP_COLLECT_MODE=${CAMERA_APP_COLLECT_MODE}
    CAMERA_APP_CALIB_MODE=${CAMERA_APP_CALIB_MODE})
