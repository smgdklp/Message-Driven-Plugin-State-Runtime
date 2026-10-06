# ============================================================================
#  coment/CMakeLists.txt —— 公共库链接
#
#  所有组件共享的东西都在这里: 头文件目录、C++ 标准、编译选项、宏。
#  其它组件只需要 target_link_libraries(<自己> PRIVATE mdpsr_common)。
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr_common LANGUAGES CXX)
endif()

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(mdpsr_common INTERFACE)

target_include_directories(mdpsr_common INTERFACE "${MDPSR_ROOT}/include")
target_compile_features(mdpsr_common INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(mdpsr_common INTERFACE /utf-8 /EHsc /W4 /wd4100)
    target_compile_definitions(mdpsr_common INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
endif()

message(STATUS "mdpsr 根目录    : ${MDPSR_ROOT}")
