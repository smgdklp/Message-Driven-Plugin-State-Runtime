# ============================================================================
#  paint —— GUI 演示客户端 (纯 C 那一侧: 不 include 任何 Windows 头)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_paint LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_paint MODULE paint.cpp)
target_link_libraries(mdpsr_paint PRIVATE mdpsr_common)

set_target_properties(mdpsr_paint PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_paint")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_paint PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/paint"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/paint"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/paint")
endforeach()

add_custom_command(TARGET mdpsr_paint POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_paint>/plugin.json"
    COMMENT "copy paint/plugin.json")
