# 与 common/inc/nvs.h 中 NVS_PROJECT_ID_TABLE 保持数值一致；新增项目时同步修改两处。
#
# 工程根 CMakeLists.txt 用法（在 include(project.cmake) 之前）：
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../common/cmake")
#   include(nvs_project_id)
#   nvs_project_use_device_id(MAIN)          # project 工程
#   nvs_project_use_device_id(BALLOT_GUARD)    # ballot_guard 工程
# factory 工程无需调用（默认 NVS_DEVICE_ID_DEFAULT / 0）

set(NVS_PROJECT_ID_MAIN 0x53530101)
set(NVS_PROJECT_ID_BALLOT_GUARD 0x53530102)
set(NVS_PROJECT_ID_VOICE_HUB 0x53530103)

# 追加新项目：set(NVS_PROJECT_ID_FOO 0x53530104)
set(NVS_HARDWARE_ID_VOICE_HUB_REV_A 0x53530202)

macro(nvs_project_use_device_id name)
    if(NOT DEFINED NVS_PROJECT_ID_${name})
        message(FATAL_ERROR "nvs_project_use_device_id: unknown project '${name}' (see nvs_project_id.cmake)")
    endif()
    add_compile_definitions(NVS_DEFAULT_DEVICE_ID=${NVS_PROJECT_ID_${name}})
endmacro()
