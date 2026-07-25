idf_component_register(
    SRCS
        "src/log.c"
        "src/flexible_button.c"
        "src/button.c"
        "src/led_scene.c"
        "src/board.c"
        "src/device_profile.c"
        "src/sdcard.c"
        "src/nvs.c"
        "ota/src/boot_slot.c"
        "ota/src/ota.c"
        "ota/src/ota_manifest.c"
        "ota/src/ota_pull.c"
        "src/battery.c"
        "src/buzzer.c"
        "src/cmd.c"
        "src/spi_link.c"
    INCLUDE_DIRS "inc" "ota/inc"
    REQUIRES log bsp_driver cbb freertos fatfs vfs esp_driver_sdmmc esp_timer sdmmc nvs_flash app_update esp_http_client mbedtls
)

# 每次构建刷新 nvs.c 编译时间戳，使 __DATE__ 构建日期与本次编译一致
string(TIMESTAMP NVS_FIRMWARE_BUILD_DATE_YMD "%Y%m%d" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "NVS_FIRMWARE_BUILD_DATE=${NVS_FIRMWARE_BUILD_DATE_YMD}")

# 与 esp_app_desc.version 同源：根 CMakeLists PROJECT_VER（release 通过 -DPROJECT_VER 注入）
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.0")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "NVS_APP_VERSION_STRING=\"${PROJECT_VER}\""
    "NVS_FACTORY_VERSION_STRING=\"${PROJECT_VER}\""
)

set(_nvs_build_stamp "${CMAKE_CURRENT_BINARY_DIR}/nvs_build.stamp")
add_custom_command(
    OUTPUT "${_nvs_build_stamp}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_nvs_build_stamp}"
    COMMENT "Refresh NVS firmware build date"
)
add_custom_target(_nvs_refresh_build_date DEPENDS "${_nvs_build_stamp}")
add_dependencies(${COMPONENT_LIB} _nvs_refresh_build_date)
set_source_files_properties("src/nvs.c" PROPERTIES OBJECT_DEPENDS "${_nvs_build_stamp}")
