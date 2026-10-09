# ============================================================================
#  alpha —— 演示插件: 自转循环 + 队列限速 + 回包
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_alpha LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_alpha MODULE alpha.cpp)
target_link_libraries(mdpsr_alpha PRIVATE mdpsr_common)

set_target_properties(mdpsr_alpha PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_alpha")

# 每个 config 都明确写死: 多配置生成器不会再给 *_<CONFIG> 形式追加子目录,
# 这样 dll 和 plugin.json 永远躺在同一个目录里 (dll_path 是相对清单目录的)。
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_alpha PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/alpha"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/alpha"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/plugins/alpha")
endforeach()

add_custom_command(TARGET mdpsr_alpha POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_alpha>/plugin.json"
    COMMENT "copy alpha/plugin.json")
