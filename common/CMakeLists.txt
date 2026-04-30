idf_component_register(
    SRCS
        "src/error.c"
        "src/log.c"
        "src/button.c"
        "src/led_scene.c"
        "src/config.c"
    INCLUDE_DIRS "inc"
    REQUIRES log bsp_driver
)
