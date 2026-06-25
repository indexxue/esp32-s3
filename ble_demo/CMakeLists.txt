cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../common/cmake")
include(git_project_version)
apply_git_project_version("${CMAKE_CURRENT_LIST_DIR}/..")
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.1.0")
endif()

set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble_demo)
