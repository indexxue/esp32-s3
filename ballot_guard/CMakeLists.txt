cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
    "../components"
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../common/cmake")
include(nvs_project_id)
nvs_project_use_device_id(BALLOT_GUARD)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ballot_guard)
