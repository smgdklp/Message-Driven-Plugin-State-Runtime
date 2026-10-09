# ============================================================================
#  beta —— 演示插件: 不声明队列 (走 Queue_default) + 跨插件请求/回包
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_beta LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_beta MODULE beta.cpp)
target_link_libraries(mdpsr_beta PRIVATE mdpsr_common)

set_target_properties(mdpsr_beta PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_beta")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_beta PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/beta"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/beta"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/beta")
endforeach()

add_custom_command(TARGET mdpsr_beta POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_beta>/plugin.json"
    COMMENT "copy beta/plugin.json")
