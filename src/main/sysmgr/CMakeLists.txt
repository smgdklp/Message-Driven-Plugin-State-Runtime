# ============================================================================
#  sysmgr —— 内核组件 (有 kernel 权限, 能装卸别人)
#
#  ★ 它住在 src/main 里, 由 src/main/CMakeLists.txt 和框架一起编译,
#    不列进根 CMakeLists 的 PLUGIN_LIST —— 它是框架的一部分, 不是可替换插件。
#
#  产物仍然是一个自包含的 dll 目录 (build/plugins/sysmgr/), 因为运行时就是靠
#  config.json 点名加载它的; 变的是"谁来编它", 不是"它长什么样"。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_sysmgr LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_sysmgr MODULE sysmgr.cpp)
target_link_libraries(mdpsr_sysmgr PRIVATE mdpsr_common)

set_target_properties(mdpsr_sysmgr PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_sysmgr")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_sysmgr PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/sysmgr"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/sysmgr"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/sysmgr")
endforeach()

add_custom_command(TARGET mdpsr_sysmgr POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_sysmgr>/plugin.json"
    COMMENT "copy sysmgr/plugin.json")
