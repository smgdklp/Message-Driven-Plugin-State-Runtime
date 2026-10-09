# ============================================================================
#  circ_c —— GUI 测试插件 C (放大 2 倍, 偏置摆到出界)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_circ_c LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_circ_c MODULE circ_c.cpp)
target_include_directories(mdpsr_circ_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")   # test/ (公共实现头)
target_link_libraries(mdpsr_circ_c PRIVATE mdpsr_common)

set_target_properties(mdpsr_circ_c PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_circ_c")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_circ_c PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_c"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_c"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/circ_c")
endforeach()

add_custom_command(TARGET mdpsr_circ_c POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_circ_c>/plugin.json"
    COMMENT "copy circ_c/plugin.json")
