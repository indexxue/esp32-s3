idf_component_register(
    SRCS
        "main.c"
        "start.c"
        "source/web_pages.c"
        "source/vote_menu_pages.c"
        "source/vote_menu_lcd.c"
        "source/vote_menu_demo.c"
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl menu
)
