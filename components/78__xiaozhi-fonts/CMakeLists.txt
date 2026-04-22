file(GLOB SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
file(GLOB EMOJI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/emoji/*.c)

if(NOT CONFIG_LV_USE_IMGFONT)
    list(FILTER SOURCES EXCLUDE REGEX "/font_emoji_(32|64)\\.c$")
endif()

idf_component_register(
    SRCS
        ${SOURCES}
        ${EMOJI_SOURCES}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        "lvgl"
)

# 为组件添加编译宏定义
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
