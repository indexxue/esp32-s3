idf_component_register(
    SRCS
        "src/log.c"
        "src/flexible_button.c"
        "src/button.c"
        "src/led_scene.c"
        "src/board.c"
        "src/sdcard.c"
        "src/attitude.c"
        "src/lcd_gallery.c"
        "src/lcd_video.c"
        "src/persist.c"
        "src/boot_slot.c"
        "src/battery.c"
        "src/cmd.c"
    INCLUDE_DIRS "inc"
    REQUIRES log bsp_driver cbb freertos fatfs vfs esp_driver_sdmmc esp_timer sdmmc nvs_flash app_update esp_new_jpeg
)
