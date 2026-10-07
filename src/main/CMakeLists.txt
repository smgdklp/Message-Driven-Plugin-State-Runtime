# ============================================================================
#  src/main/CMakeLists.txt —— 宿主 + 组织整个项目
#
#  入口:
#      cmake -S src/main -B build -G "Visual Studio 18 2026" -A x64
#      cmake --build build --config Release
#
#  产物 (MDPSR_OUT, 默认 <根>/build):
#      build/mdpsr.exe              宿主
#      build/config.json            初始化配置 (必须与 exe 同目录)
#      build/resource/              公共素材
#      build/core/                  内核组件
#      build/<每个插件一个子目录>/   dll + plugin.json, 自包含 (方便单独拷走)
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MDPSR_OUT "${MDPSR_ROOT}/build" CACHE PATH "编译产物目录")
set(MDPSR_OPENCV_ROOT "D:/Cpp/Packages/opencv/build" CACHE PATH "OpenCV build 目录")

message(STATUS "mdpsr 根目录  : ${MDPSR_ROOT}")
message(STATUS "mdpsr 产物目录: ${MDPSR_OUT}")

# ---------------------------------------------------------------------------
#  公共设置
# ---------------------------------------------------------------------------
add_subdirectory("${MDPSR_ROOT}/src/cmake" "${CMAKE_BINARY_DIR}/cmake_common")

# ---------------------------------------------------------------------------
#  运行时静态库 —— /test 下的测试项目直接链接它
# ---------------------------------------------------------------------------
add_library(mdpsr_runtime STATIC
    runtime/mainmap.cpp
    runtime/queue.cpp
    runtime/runtime.cpp
    runtime/loader.cpp
    runtime/json.cpp
    runtime/host_api.cpp
)
target_link_libraries(mdpsr_runtime PUBLIC mdpsr_common)
target_link_libraries(mdpsr_runtime PUBLIC user32)

target_include_directories(mdpsr_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime")

# ---------------------------------------------------------------------------
#  宿主可执行文件
# ---------------------------------------------------------------------------
add_executable(mdpsr main.cpp)
target_link_libraries(mdpsr PRIVATE mdpsr_runtime)

set_target_properties(mdpsr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MDPSR_OUT}")

# config.json 与素材: 每次都同步 (不能只在 exe 重链时才拷 —— 只改 config.json
# 是很常见的操作, 漏拷会让人对着旧配置排查半天)
add_custom_target(mdpsr_runtime_files ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json" "${MDPSR_OUT}/config.json"
    COMMENT "同步 config.json 到运行时根目录")
add_dependencies(mdpsr_runtime_files mdpsr)

if(EXISTS "${MDPSR_ROOT}/resource")
    add_custom_command(TARGET mdpsr_runtime_files POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${MDPSR_ROOT}/resource" "${MDPSR_OUT}/resource"
        COMMENT "同步 resource 素材目录")
endif()

# ---------------------------------------------------------------------------
#  各组件 (每个组件的 CMakeLists 也能独立编译自己)
# ---------------------------------------------------------------------------
add_subdirectory("${MDPSR_ROOT}/src/components/core"  "${CMAKE_BINARY_DIR}/components/core")
add_subdirectory("${MDPSR_ROOT}/src/components/ticker" "${CMAKE_BINARY_DIR}/components/ticker")
