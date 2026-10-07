# ============================================================================
#  src/components/ticker/CMakeLists.txt —— 示例插件
#
#  能独立编译自己:
#      cmake -S src/components/ticker -B build/ticker -G "Visual Studio 18 2026" -A x64
#      cmake --build build/ticker --config Release
#
#  产物: <MDPSR_OUT>/ticker/  (mdpsr_ticker.dll + plugin.json, 自包含)
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_ticker LANGUAGES CXX)
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

add_library(mdpsr_ticker SHARED
    ticker.cpp
    ticker_plugin.cpp
)

target_link_libraries(mdpsr_ticker PRIVATE mdpsr_common)
target_link_libraries(mdpsr_ticker PRIVATE user32)

set(OUTDIR "${MDPSR_OUT}/ticker")

set_target_properties(mdpsr_ticker PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mdpsr_ticker"
    RUNTIME_OUTPUT_DIRECTORY "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTDIR}"
    LIBRARY_OUTPUT_DIRECTORY "${OUTDIR}")

add_custom_target(mdpsr_ticker_files ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTDIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${OUTDIR}/plugin.json"
    COMMENT "同步 ticker 的 plugin.json")
add_dependencies(mdpsr_ticker_files mdpsr_ticker)
