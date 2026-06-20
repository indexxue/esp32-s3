# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Allow loading custom components from repository-level component folders.
set(EXTRA_COMPONENT_DIRS
    "../common"
    "../bsp_driver"
    "../cbb"
    "../components"
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../common/cmake")
include(nvs_project_id)
nvs_project_use_device_id(MAIN)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(project)
