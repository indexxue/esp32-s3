idf_component_register(
    SRCS "main.c" "start.c" "source/web_pages.c"
    INCLUDE_DIRS "." "source"
    EMBED_FILES "source/www/index.html"
    REQUIRES cbb common bsp_driver esp_timer app_update web_ctrl
)
