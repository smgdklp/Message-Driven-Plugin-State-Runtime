# ============================================================================
#  test/queue/CMakeLists.txt —— 消息队列单元测试 (独立可编译)
#
#      cmake -S test/queue -B build/test/queue -G "Visual Studio 18 2026" -A x64
#      cmake --build build/test/queue --config Release
#      build/test/queue/mdpsr_test_queue.exe
#
#  只验证 Queue 的帧语义, 所以只链接 mdpsr_runtime 里的 queue_map 部分。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_test_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MDPSR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MDPSR_OUT "${MDPSR_ROOT}/build" CACHE PATH "编译产物目录")

# 只为拿到运行时静态库; 整个项目也会一起被配置
add_subdirectory("${MDPSR_ROOT}/scr/main" "${CMAKE_BINARY_DIR}/mdpsr")

add_executable(mdpsr_test_queue queue_test.cpp)
target_link_libraries(mdpsr_test_queue PRIVATE mdpsr_runtime)
target_include_directories(mdpsr_test_queue PRIVATE "${MDPSR_ROOT}/scr/main")

set_target_properties(mdpsr_test_queue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MDPSR_OUT}/test/queue"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MDPSR_OUT}/test/queue"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MDPSR_OUT}/test/queue"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MDPSR_OUT}/test/queue"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MDPSR_OUT}/test/queue")
