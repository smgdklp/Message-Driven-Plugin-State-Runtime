# ============================================================================
#  test/mt/CMakeLists.txt —— 多线程读写压力测试 (独立可编译)
#
#      cmake -S test/mt -B build/test/mt -G "Visual Studio 18 2026" -A x64
#      cmake --build build/test/mt --config Release
#      build/test/mt/mdpsr_test_mt.exe
#
#  它不读 config.json、不加载任何插件, 只链接运行时静态库, 直接打
#  Queue / Dispatcher / Map / 引用计数 / 各内存池的多线程正确性。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_test_mt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MDPSR_OUT "${MDPSR_ROOT}/build" CACHE PATH "编译产物目录")

# 为拿到运行时静态库 (宿主 + 各组件也会一起被配置)
add_subdirectory("${MDPSR_ROOT}/scr/main" "${CMAKE_BINARY_DIR}/mdpsr")

add_executable(mdpsr_test_mt mt_test.cpp)
target_link_libraries(mdpsr_test_mt PRIVATE mdpsr_runtime)
target_include_directories(mdpsr_test_mt PRIVATE "${MDPSR_ROOT}/scr/main")

set_target_properties(mdpsr_test_mt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MDPSR_OUT}/test/mt"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MDPSR_OUT}/test/mt"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MDPSR_OUT}/test/mt"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MDPSR_OUT}/test/mt"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MDPSR_OUT}/test/mt")
