# ============================================================================
#  src/main/CMakeLists.txt —— 框架那一半
#
#  它只产出宿主自己, 里面【没有一行插件路径】:
#      mdpsr_runtime   静态库 (runtime/*.cpp)
#      mdpsr.exe       main.cpp + GUI 自测夹具
#      mdpsr_sysmgr    ★ 内核组件, 和框架一起编, 不进 PLUGIN_LIST
#
#  产物布局:
#      <MDPSR_OUT>/mdpsr.exe
#      <MDPSR_OUT>/config.json
#      <MDPSR_OUT>/plugins/sysmgr/{mdpsr_sysmgr.dll, plugin.json}
#
#  可以直接当顶层工程编 (cmake -S src/main -B build), 也可以被根 CMakeLists
#  当子目录加进来 —— 两种都好使。
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(mdpsr LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT MDPSR_OUT)
        set(MDPSR_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../../build" CACHE PATH "编译产物根目录")
    endif()
endif()

# 公共编译设置 —— 产物根目录 MDPSR_OUT 也在这里兜底定义
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
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}")
endforeach()

# config.json 必须和 exe 同目录。
# 被根 CMakeLists 加进来时【不拷】: 那份由根按本次 PLUGIN_LIST/Mode 生成,
# 直接拷这份全量清单会让引导在"声明了但没编到"上失败。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_custom_command(TARGET mdpsr POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
                "$<TARGET_FILE_DIR:mdpsr>/config.json"
        COMMENT "copy config.json")
endif()

# ---- ★ 内核组件 sysmgr: 和框架一起编, 不是可替换插件 ----
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/sysmgr"
                 "${CMAKE_BINARY_DIR}/sysmgr")

# ---------------------------------------------------------------------------
#  ★ 这里就是以前"插件路径"待过的地方, 现在没了。
#
#  插件住在 src/components 和 test 下, 由【根 CMakeLists 的 PLUGIN_LIST】
#  逐个 add_subdirectory 进来 —— 框架不需要、也不应该知道它们的存在。
#  运行时读 config.json 决定加载谁, 那才是唯一的"组装"环节。
# ---------------------------------------------------------------------------

# ---- 一把梭: 跑自测 ----
add_custom_target(selftest
    COMMAND "$<TARGET_FILE:mdpsr>" --selftest --cycles 20
    DEPENDS mdpsr mdpsr_sysmgr
    WORKING_DIRECTORY "${MDPSR_OUT}"
    COMMENT "跑热插拔自测")
