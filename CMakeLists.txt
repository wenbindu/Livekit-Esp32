cmake_minimum_required(VERSION 3.16)

set(defaults)
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    list(APPEND defaults
        "configs/sdkconfig.defaults.base"
        "configs/sdkconfig.defaults.board.lichuang_esp32s3"
        "configs/sdkconfig.defaults.dev")
    if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.generated")
        list(APPEND defaults "sdkconfig.defaults.generated")
    endif()
    set(SDKCONFIG_DEFAULTS "${defaults}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(livekit_esp32s3)
