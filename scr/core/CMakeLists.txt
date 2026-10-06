# ============================================================================
#  scr/core/CMakeLists.txt —— 内核组件 DLLMgr
#  可以独立编译自己:  cmake -S scr/core -B build/core
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_core LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED MDPSR_OUT)
    set(MDPSR_OUT "${MDPSR_ROOT}/build")
endif()
if(NOT TARGET mdpsr_common)
    add_subdirectory("${MDPSR_ROOT}/coment" "${CMAKE_BINARY_DIR}/_coment")
endif()

add_library(mdpsr_core SHARED dllmgr.cpp)
target_link_libraries(mdpsr_core PRIVATE mdpsr_common)
target_link_libraries(mdpsr_core PRIVATE user32)

set_target_properties(mdpsr_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mdpsr_core"
    RUNTIME_OUTPUT_DIRECTORY "${MDPSR_OUT}/core"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MDPSR_OUT}/core"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MDPSR_OUT}/core"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MDPSR_OUT}/core"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MDPSR_OUT}/core"
    LIBRARY_OUTPUT_DIRECTORY "${MDPSR_OUT}/core")

add_custom_command(TARGET mdpsr_core POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${MDPSR_OUT}/core/plugin.json"
    COMMENT "拷贝 core 插件清单 plugin.json")
