# camera 主组件：预览 + SoftAP/STA 网页图传；DETECT=1 时附加钢珠 ESPDet。

set(CAMERA_APP_DETECT_MODE 0)
set(CAMERA_APP_COLLECT_MODE 0)
set(CAMERA_APP_CALIB_MODE 0)

set(CAMERA_MAIN_SRCS
    "main.c"
    "start.c"
    "source/camera_app_boot.c"
    "source/camera_app_input.c"
    "source/camera_ui.c"
    "source/camera_sensor.c"
    "source/web_pages.c"
    "source/servo_ctrl.c"
    "source/servo_calib_cmd.c"
    "source/camera_spi_host.c"
    "source/spi_link.c"
)

set(CAMERA_MAIN_REQUIRES
    cbb
    common
    bsp_driver
    esp_timer
    app_update
    web_ctrl
    nvs_flash
    esp_new_jpeg
)

if(CAMERA_APP_DETECT_MODE)
    list(APPEND CAMERA_MAIN_SRCS
        "source/camera_model.cpp"
        "source/servo_ball_follow.c"
    )
    list(APPEND CAMERA_MAIN_REQUIRES ball_detect)
else()
    list(APPEND CAMERA_MAIN_SRCS "source/camera_model_stub.c")
endif()

idf_component_register(
    SRCS ${CAMERA_MAIN_SRCS}
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html" "source/www/preview.html"
    REQUIRES ${CAMERA_MAIN_REQUIRES}
)

# 采数 / 校准 / 识别：改对应行后重编烧录即可（不必 fullclean）。
# DETECT=1 识别；COLLECT=1 采数（关 ESPDet）；CALIB=1 单舵机校准（关 ESPDet）。
# 默认全 0 = 预览量产路径（不开检测）。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=4
    LED_SCENE_BRIGHTNESS_SCALE=128
    CAMERA_APP_DETECT_MODE=${CAMERA_APP_DETECT_MODE}
    CAMERA_APP_COLLECT_MODE=${CAMERA_APP_COLLECT_MODE}
    CAMERA_APP_CALIB_MODE=${CAMERA_APP_CALIB_MODE})
