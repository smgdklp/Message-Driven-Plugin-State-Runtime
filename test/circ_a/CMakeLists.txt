# ============================================================================
#  circ_a —— GUI 测试插件 A (图比窗口小, 换色动画, 点击关窗)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_circ_a LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_circ_a MODULE circ_a.cpp)
target_include_directories(mdpsr_circ_a PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")   # test/ (公共实现头)
target_link_libraries(mdpsr_circ_a PRIVATE mdpsr_common)

set_target_properties(mdpsr_circ_a PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_circ_a")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_circ_a PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_a"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_a"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_a")
endforeach()

add_custom_command(TARGET mdpsr_circ_a POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_circ_a>/plugin.json"
    COMMENT "copy circ_a/plugin.json")
