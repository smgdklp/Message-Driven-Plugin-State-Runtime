# ============================================================================
#  src/main/CMakeLists.txt
#  组织整个项目: 宿主 (静态库 + 薄 exe) + 四个组件
#
#  产物布局 (每个插件一个自包含子目录, 可以整个拷走):
#      build/mdpsr.exe
#      build/config.json
#      build/plugins/sysmgr/{mdpsr_sysmgr.dll, plugin.json}
#      build/plugins/alpha/...  beta/...  gamma/...
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 公共编译设置
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cmake"
                 "${CMAKE_BINARY_DIR}/cmake_common")

# ---- 宿主: 真正的运行时是一个静态库, exe 只负责 main ----
add_library(mdpsr_runtime STATIC
    runtime/registry.cpp
    runtime/queue.cpp
    runtime/runtime.cpp
    runtime/loader.cpp
    runtime/host_api.cpp
    runtime/json.cpp)

target_include_directories(mdpsr_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mdpsr_runtime PUBLIC mdpsr_common winmm)

# 宿主 exe = main.cpp + GUI 阶段自测 (测试代码整体待在 ./test)
set(MDPSR_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../test")
add_executable(mdpsr main.cpp "${MDPSR_TEST_DIR}/gui_selftest.cpp")
target_include_directories(mdpsr PRIVATE "${MDPSR_TEST_DIR}")
target_link_libraries(mdpsr PRIVATE mdpsr_runtime)

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}")
endforeach()

# config.json 必须和 exe 同目录
add_custom_command(TARGET mdpsr POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
            "$<TARGET_FILE_DIR:mdpsr>/config.json"
    COMMENT "copy config.json")

# ---- 组件 (src/components) ----
set(MDPSR_COMPONENTS sysmgr alpha beta gamma winmsg paint)
foreach(comp IN LISTS MDPSR_COMPONENTS)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../components/${comp}"
                     "${CMAKE_BINARY_DIR}/components/${comp}")
endforeach()

# ---- 测试模块 (test/) ----
# ★ 测试插件不进 src/: 它们是"用来压框架的东西", 不是框架的一部分。
#   删掉整个 test/ 目录, 宿主和 winmsg 一样跑 (只是没了 GUI 自测的夹具)。
set(MDPSR_TEST_PLUGINS circ_a circ_b circ_c)
foreach(tp IN LISTS MDPSR_TEST_PLUGINS)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../test/${tp}"
                     "${CMAKE_BINARY_DIR}/test_plugins/${tp}")
endforeach()

# ---- 一把梭: 构建 + 自测 ----
add_custom_target(selftest
    COMMAND "$<TARGET_FILE:mdpsr>" --selftest --cycles 20
    DEPENDS mdpsr ${MDPSR_COMPONENTS} ${MDPSR_TEST_PLUGINS}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "跑热插拔自测")
