# ============================================================================
#  src/components/core/CMakeLists.txt —— 内核组件
#
#  能独立编译自己:
#      cmake -S src/components/core -B build/core -G "Visual Studio 18 2026" -A x64
#      cmake --build build/core --config Release
#
#  产物: <MDPSR_OUT>/core/  (mdpsr_core.dll + plugin.json, 自包含)
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_core LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT DEFINED MDPSR_OUT)
    set(MDPSR_OUT "${MDPSR_ROOT}/build")
endif()
if(NOT TARGET mdpsr_common)
    add_subdirectory("${MDPSR_ROOT}/src/cmake" "${CMAKE_BINARY_DIR}/_mdpsr_cmake")
endif()

add_library(mdpsr_core SHARED
    core_mgr.cpp
    core_plugin.cpp
)

target_link_libraries(mdpsr_core PRIVATE mdpsr_common)
target_link_libraries(mdpsr_core PRIVATE user32)

# 复用宿主的 JSON 实现 (同进程, 只有一份实现)
target_include_directories(mdpsr_core PRIVATE
    "${MDPSR_ROOT}/src/main/runtime")

set(OUTDIR "${MDPSR_OUT}/core")

set_target_properties(mdpsr_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mdpsr_core"
    RUNTIME_OUTPUT_DIRECTORY "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTDIR}"
    LIBRARY_OUTPUT_DIRECTORY "${OUTDIR}")

# 清单每次同步 (改 plugin.json 不需要重编 dll)
add_custom_target(mdpsr_core_files ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTDIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${OUTDIR}/plugin.json"
    COMMENT "同步 core 的 plugin.json")
add_dependencies(mdpsr_core_files mdpsr_core)
