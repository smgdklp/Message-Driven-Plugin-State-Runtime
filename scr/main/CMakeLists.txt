# ============================================================================
#  scr/main/CMakeLists.txt —— 主程序 + 组织编译成完整项目
#
#  这是整个项目的入口:
#      cmake -S scr/main -B build -G "Visual Studio 18 2026" -A x64
#      cmake --build build --config Release
#
#  产物 (MDPSR_OUT, 默认 <根>/build):
#      build/mdpsr.exe            宿主 (config.json 必须与它同目录)
#      build/config.json          初始化配置: plugin 列表 + init_cmd 后门标签
#      build/core/                内核组件 DLLMgr
#      build/capture/             截屏插件
#      build/cmd/                 仿真 cmd 插件
#
#  为什么默认用 Visual Studio 生成器而不是 Ninja:
#      中文版 MSVC 的 /showIncludes 输出是 "注意: 包含文件: xxx"。
#      CMake 把这个非 ASCII 前缀写进 Ninja 的 rules.ninja 时会经过一次码页转换,
#      与 cl.exe 的实际输出对不上, 于是 Ninja 的依赖库 (.ninja_deps) 是空的 ——
#      症状是"改了头文件不重编, 链接时报找不到符号"。
#      MSBuild 用自带文件跟踪, 与本机码页无关。想用 Ninja 请先确认依赖跟踪正常
#      (改个头文件, 看看对应源文件会不会重编)。
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

message(STATUS "mdpsr 根目录    : ${MDPSR_ROOT}")
message(STATUS "mdpsr 产物目录  : ${MDPSR_OUT}")
message(STATUS "mdpsr OpenCV    : ${MDPSR_OPENCV_ROOT}")

# ---------------------------------------------------------------------------
#  公共库
# ---------------------------------------------------------------------------
add_subdirectory("${MDPSR_ROOT}/coment" "${CMAKE_BINARY_DIR}/coment")

# ---------------------------------------------------------------------------
#  运行时库 (静态库) —— /test 下的测试项目直接链接它
# ---------------------------------------------------------------------------
add_library(mdpsr_runtime STATIC
    queue_map.cpp
    dispatcher.cpp
    runtime.cpp
    loader.cpp
    host_api.cpp
)
target_link_libraries(mdpsr_runtime PUBLIC mdpsr_common)
target_link_libraries(mdpsr_runtime PUBLIC user32 shell32)

# ---------------------------------------------------------------------------
#  宿主可执行文件 —— config.json 必须与它同目录
# ---------------------------------------------------------------------------
add_executable(mdpsr main.cpp)
target_link_libraries(mdpsr PRIVATE mdpsr_runtime)

set_target_properties(mdpsr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MDPSR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MDPSR_OUT}")

add_custom_command(TARGET mdpsr POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MDPSR_OUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json" "${MDPSR_OUT}/config.json"
    COMMENT "拷贝 config.json 到宿主 exe 同目录")

# ---------------------------------------------------------------------------
#  各组件 (每个组件自己的 CMakeLists 也能独立编译自己)
# ---------------------------------------------------------------------------
add_subdirectory("${MDPSR_ROOT}/scr/core"    "${CMAKE_BINARY_DIR}/scr/core")
add_subdirectory("${MDPSR_ROOT}/scr/capture" "${CMAKE_BINARY_DIR}/scr/capture")
add_subdirectory("${MDPSR_ROOT}/scr/cmd"     "${CMAKE_BINARY_DIR}/scr/cmd")
