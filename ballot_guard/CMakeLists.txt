cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
    "../components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ballot_guard)
