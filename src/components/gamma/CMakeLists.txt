# ============================================================================
#  gamma —— 演示插件: 两个 handle 一条队列 / 运行时自己建队列 / 卸载可观测
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_gamma LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_gamma MODULE gamma.cpp)
target_link_libraries(mdpsr_gamma PRIVATE mdpsr_common)

set_target_properties(mdpsr_gamma PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_gamma")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_gamma PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/gamma"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/gamma"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/gamma")
endforeach()

add_custom_command(TARGET mdpsr_gamma POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_gamma>/plugin.json"
    COMMENT "copy gamma/plugin.json")
