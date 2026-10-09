# ============================================================================
#  src/cmake/CMakeLists.txt —— 公共编译设置 (mdpsr_common)
#
#  宿主和所有插件都 link 它, 保证:
#      · 用的是同一份 include (abi.h 是唯一的二进制契约)
#      · 用的是同一套语言/警告开关
#      · 用的是同一个 C++ 运行时 (MSVC: /MD 必须一致, 否则跨 DLL 传 pmr 会崩)
#
#  它也可以被单独 add_subdirectory —— 这样每个组件都能独立编译自己。
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_common LANGUAGES CXX)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 产物根目录: 由根 CMakeLists 统一设定 (BUILD 变量)。
# 单独编某个插件时没人设, 就在这儿兜底成 <根>/build —— 反正绝不能拿
# CMAKE_BINARY_DIR 当基准, 那是各自的 build 树, 会把 plugins/ 套娃。
if(NOT MDPSR_OUT)
    set(MDPSR_OUT "${MDPSR_ROOT}/build" CACHE PATH "编译产物根目录 (dll 落在这里的 plugins/ 下)")
endif()

add_library(mdpsr_common INTERFACE)

target_include_directories(mdpsr_common INTERFACE
    "${MDPSR_ROOT}/src/main"
    "${MDPSR_ROOT}/src/main/include"
    "${MDPSR_ROOT}/src/components"
    "${MDPSR_ROOT}/src/components/winmsg")   # GUI 客户端的协议头 (winmsg_client.h)

target_compile_features(mdpsr_common INTERFACE cxx_std_20)

option(MDPSR_ASAN "用 AddressSanitizer 编译 (查内存/数据竞争类问题)" OFF)

if(MSVC)
    target_compile_options(mdpsr_common INTERFACE
        /utf-8            # 源码与执行字符集都用 UTF-8 (中文注释/日志)
        /EHsc             # 标准 C++ 异常模型
        /W4               # 高警告等级
        /wd4100           # 未引用的形参 (接口函数形参固定, 不可避免)
        /permissive-      # 严格标准一致性
        /Zc:__cplusplus
        /Zc:preprocessor)
    target_compile_definitions(mdpsr_common INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    if(MDPSR_ASAN)
        # /fsanitize=address 只给编译器: cl.exe 会在目标文件里留下指令, 让链接器
        # 自己挂上 ASan 运行时。把它再传给链接器是无效的 (LNK4044)。
        target_compile_options(mdpsr_common INTERFACE /fsanitize=address /Zi /Od)
        target_link_options(mdpsr_common INTERFACE /INCREMENTAL:NO /DEBUG)
    endif()
endif()
