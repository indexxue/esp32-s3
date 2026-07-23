# 模型推理开关：1=开启端侧检测（编译 cat_detect + 嵌入 espdl 模型 + 依赖 esp_dl），
# 0=关闭（默认；camera_model.* 走 CAMERA_MODEL_SKIP 桩，节省 Flash 与编译时间）。
set(CAMERA_MODEL_ENABLE 0)

set(CAMERA_MODEL_SRCS "")
set(CAMERA_MODEL_REQUIRES "")
if(CAMERA_MODEL_ENABLE)
    list(APPEND CAMERA_MODEL_SRCS "source/cat_detect.cpp")
    list(APPEND CAMERA_MODEL_REQUIRES esp_dl)
endif()

idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/camera_ui.c"
        "source/camera_sensor.c"
        "source/camera_model.cpp"
        "source/web_pages.c"
        ${CAMERA_MODEL_SRCS}
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl nvs_flash esp_new_jpeg ${CAMERA_MODEL_REQUIRES}
)

if(CAMERA_MODEL_ENABLE)
    target_add_binary_data(${COMPONENT_LIB} "source/model/cat_detect.espdl" BINARY)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_MODEL_SKIP=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LED_SCENE_WS2812_GPIO=48
    LED_SCENE_WS2812_NUM_LEDS=1
    LED_SCENE_BRIGHTNESS_SCALE=128)
