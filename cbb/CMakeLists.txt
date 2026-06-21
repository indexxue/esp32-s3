idf_component_register(
    SRCS
        "ds3231.c"
        "eeprom.c"
        "ili9341.c"
        "st7789.c"
        "lcd.c"
        "mcp4651.c"
        "mpu6050.c"
        "qmi8658a.c"
        "oled.c"
        "tb6612.c"
        "ws2812b.c"
        "xpt2046.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_rmt
)
