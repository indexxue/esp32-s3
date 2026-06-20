cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
    "../components"
)

add_compile_definitions(BUILD_FACTORY)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(factory)
