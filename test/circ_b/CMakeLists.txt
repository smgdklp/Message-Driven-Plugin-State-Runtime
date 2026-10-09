# ============================================================================
#  circ_b —— GUI 测试插件 B (图比窗口大, 缩放动画, 整窗可拖)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_circ_b LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_circ_b MODULE circ_b.cpp)
target_include_directories(mdpsr_circ_b PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")   # test/ (公共实现头)
target_link_libraries(mdpsr_circ_b PRIVATE mdpsr_common)

set_target_properties(mdpsr_circ_b PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_circ_b")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_circ_b PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/circ_b"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/circ_b"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/circ_b")
endforeach()

add_custom_command(TARGET mdpsr_circ_b POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_circ_b>/plugin.json"
    COMMENT "copy circ_b/plugin.json")
