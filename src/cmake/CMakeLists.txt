# ============================================================================
#  src/cmake/CMakeLists.txt —— 公共编译设置 (mdpsr_common)
#
#  宿主和所有插件都 link 它, 保证:
#      · 用的是同一份 include (abi.h 是唯一的二进制契约)
#      · 用的是同一套语言/警告开关
#      · 用的是同一个 C++ 运行时 (MSVC: 必须 /MD 一致, 否则跨 DLL 传 pmr 会崩)
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_common LANGUAGES CXX)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(mdpsr_common INTERFACE)

target_include_directories(mdpsr_common INTERFACE
    "${MDPSR_ROOT}/src/main"
    "${MDPSR_ROOT}/src/main/include")

target_compile_features(mdpsr_common INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(mdpsr_common INTERFACE
        /utf-8          # 源码与执行字符集都用 UTF-8 (中文注释/日志)
        /EHsc           # 标准 C++ 异常模型
        /W4             # 高警告等级
        /wd4100         # 未引用的形参 (接口函数形参固定, 不可避免)
        /permissive-    # 严格标准一致性
        /Zc:__cplusplus)
    target_compile_definitions(mdpsr_common INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
endif()
