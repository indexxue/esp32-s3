cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../common/cmake")
include(git_project_version)
apply_git_project_version("${CMAKE_CURRENT_LIST_DIR}/..")
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.0")
endif()

include(nvs_project_id)

add_compile_definitions(BOARD_PROFILE_VOICE_HUB=1)
add_compile_definitions(NVS_DEFAULT_HARDWARE_ID=${NVS_HARDWARE_ID_VOICE_HUB_REV_A})

set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
    "../components"
)

nvs_project_use_device_id(VOICE_HUB)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_hub)
