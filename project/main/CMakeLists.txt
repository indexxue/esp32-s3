idf_component_register(
    SRCS "project.c" "board.c"
    INCLUDE_DIRS "."
    REQUIRES common bsp_driver
)
