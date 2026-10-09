# ============================================================================
#  /CMakeLists.txt —— 整个项目的【唯一编译入口】
#
#  用法:
#      cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#      cmake --build build --config Release
#
#  三个开关 (都是 -D 传, 也可以直接用 cmake-gui 改):
#
#      BUILD        产出文件夹。所有产物都落在它下面; 空 = <根>/build
#                       <BUILD>/mdpsr.exe
#                       <BUILD>/config.json
#                       <BUILD>/plugins/<插件名>/{dll, plugin.json}
#
#      PLUGIN_LIST  要编译的插件文件夹 (相对根目录的路径)。逐个
#                   add_subdirectory, 每个插件的产物 + 它自己的 plugin.json
#                   都放进 <BUILD>/plugins/<插件名>/。
#                   空 = 由 Mode 决定 (见下)。
#
#      Mode         分流, 方便两种跑法:
#                       TXST1 = 框架 + test/ 里的测试插件
#                               ★ 另外会自动带上 winmsg + paint: GUI 自测要
#                                 winmsg 当 broker、paint 当第 4 扇窗, 少了任何一个
#                                 --guitest 都会报"窗口没 READY"。这是 gui_selftest
#                                 的夹具要求, 见下面的 _txst1_gui_deps。
#                                 只有这个 Mode 能跑通 --guitest。
#                       TXST2 = src/components/ 里的组件 (含 wingui/winmsg)
#                               (其中 alpha/beta/gamma 是历史遗留, 不保证适配)
#                   只在 PLUGIN_LIST 为空时生效。
#
#      产物布局 (在 <BUILD>/ 下):
#                       mdpsr.exe / config.json / plugins/<名字>/{dll,plugin.json}
#                       _framework/ 和 _plugins/<名字>/ 是 CMake 的中间树, 可以无视。
#
#  ---------------------------------------------------------------------------
#  这一层只做"编排", 不碰源码, 也不知道任何插件内部长什么样:
#
#      · 框架那一半是 src/main —— 它的 CMakeLists 里【没有】任何插件路径。
#        sysmgr 虽然后缀是插件形态, 但它是内核, 就住在 src/main 里, 由框架
#        的 CMakeLists 一起编 (不进 PLUGIN_LIST)。
#      · 插件那一半全都住在 src/components (组件) 和 test (测试夹具) 下,
#        各自带一份 CMakeLists.txt + plugin.json, 都是独立工程。
#      · 谁和谁最终跑在一起, 由运行时读 config.json 决定 —— 构建系统不参与。
#        所以换一个插件 = 换一个文件夹, 不用改这里任何一行。
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
#  BUILD —— 产出文件夹
# ---------------------------------------------------------------------------
set(BUILD "" CACHE STRING "产出文件夹 (空 = <根>/build)")
if(NOT BUILD)
    set(BUILD "${CMAKE_SOURCE_DIR}/build")
endif()
get_filename_component(BUILD "${BUILD}" ABSOLUTE)

# 所有插件共用这一份产物根目录。插件是各自 add_subdirectory 进来的独立工程,
# 谁都不能拿 CMAKE_BINARY_DIR 当基准 (那是各自的 build 树, 不是产物目录)。
set(MDPSR_OUT "${BUILD}" CACHE PATH "编译产物根目录 (dll 落在这里的 plugins/ 下)")

message(STATUS "mdpsr 产出目录 : ${BUILD}")

# ---------------------------------------------------------------------------
#  Mode / PLUGIN_LIST —— 要编哪些插件
# ---------------------------------------------------------------------------
set(Mode "" CACHE STRING "TXST1 = 框架+test 插件; TXST2 = 组件(含 wingui) 组合测试; 空 = 只编框架")
set(PLUGIN_LIST "" CACHE STRING "要编译的插件文件夹 (相对根目录), 空则由 Mode 决定")

# 实际要编的列表用普通变量装: PLUGIN_LIST 是 cache 变量, 直接往里 APPEND
# 会把本次 Mode 推导出来的结果写进缓存, 下次换 Mode 就会残留。
set(_plugins "${PLUGIN_LIST}")

# TXST1 是"GUI 自测"场景: 除了 test 里的三个客户端, 还要 winmsg 当 broker、
# paint 当第 4 扇窗 —— 这是 gui_selftest 的夹具要求, 写在 test/gui_selftest.cpp 里。
set(_txst1_gui_deps "src/components/winmsg" "src/components/paint")

if(NOT _plugins)
    set(_mode "${Mode}")
    string(TOUPPER "${_mode}" _mode)
    if(_mode STREQUAL "TXST1")
        # 框架 + test/ 里的测试插件。
        # ★ 再带上 winmsg + paint: GUI 自测要 winmsg 当 broker、要 paint 当第 4 扇窗,
        #   少了任何一个 --guitest 都会报"窗口没 READY"。
        file(GLOB _dirs RELATIVE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/test/*")
        foreach(_d IN LISTS _dirs)
            if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_d}"
               AND EXISTS "${CMAKE_SOURCE_DIR}/${_d}/CMakeLists.txt")
                list(APPEND _plugins "${_d}")
            endif()
        endforeach()
        foreach(_gui IN LISTS _txst1_gui_deps)
            if(EXISTS "${CMAKE_SOURCE_DIR}/${_gui}/CMakeLists.txt")
                list(APPEND _plugins "${_gui}")
            endif()
        endforeach()
        message(STATUS "mdpsr Mode     : TXST1 (框架 + test 插件 + winmsg/paint)")
    elseif(_mode STREQUAL "TXST2")
        # 组件插件 (alpha/beta/gamma/paint/winmsg) 组合测试
        file(GLOB _dirs RELATIVE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src/components/*")
        foreach(_d IN LISTS _dirs)
            if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_d}"
               AND EXISTS "${CMAKE_SOURCE_DIR}/${_d}/CMakeLists.txt")
                list(APPEND _plugins "${_d}")
            endif()
        endforeach()
        message(STATUS "mdpsr Mode     : TXST2 (组件 + wingui 组合)")
    elseif(_mode STREQUAL "")
        message(STATUS "mdpsr Mode     : (空) 只编框架 + sysmgr, 不编任何插件")
    else()
        message(FATAL_ERROR "Mode 只能是 TXST1 / TXST2 或留空, 现在是 '${Mode}'")
    endif()
endif()
list(SORT _plugins)

# ---------------------------------------------------------------------------
#  框架 (含内核组件 sysmgr) —— 它的 CMakeLists 里没有任何插件路径
# ---------------------------------------------------------------------------
add_subdirectory(src/main "${BUILD}/_framework")

# ---------------------------------------------------------------------------
#  插件 —— 每个文件夹一个独立工程, 产物各自进 <BUILD>/plugins/<名字>/
#
#  plugin.json 的拷贝由每个插件自己的 CMakeLists 负责 (POST_BUILD),
#  路径用的是 ${MDPSR_OUT}, 所以无论从哪儿编都落同一个地方。
# ---------------------------------------------------------------------------
foreach(_p IN LISTS _plugins)
    get_filename_component(_name "${_p}" NAME)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_p}/CMakeLists.txt")
        message(FATAL_ERROR "PLUGIN_LIST 里的 '${_p}' 没有 CMakeLists.txt")
    endif()
    add_subdirectory("${CMAKE_SOURCE_DIR}/${_p}" "${BUILD}/_plugins/${_name}")
endforeach()

message(STATUS "mdpsr 插件     : ${_plugins}")

# ---------------------------------------------------------------------------
#  生成 <BUILD>/config.json —— 这就是"组合"的唯一环节
#
#  宿主是照着 config.json 里的顺序装插件的, 所以这份文件必须【只列本次真正
#  编出来的插件】, 否则引导会因为"声明了但没装到"直接失败。
#
#  顺序沿用 src/main/config.json 里作者写好的那份 (它体现依赖关系:
#  sysmgr 在内核位, winmsg 在 GUI 客户端前面); 这次没编的就跳过, 编了但
#  那份清单里没有的就追加到末尾。
# ---------------------------------------------------------------------------
set(_canon "${CMAKE_SOURCE_DIR}/src/main/config.json")
set(_order "")
if(EXISTS "${_canon}")
    file(READ "${_canon}" _canon_text)
    string(REGEX MATCHALL "plugins/[A-Za-z0-9_]+/plugin.json" _canon_hits "${_canon_text}")
    foreach(_h IN LISTS _canon_hits)
        string(REGEX REPLACE "^plugins/([A-Za-z0-9_]+)/plugin.json$" "\\1" _n "${_h}")
        list(APPEND _order "${_n}")
    endforeach()
endif()

# 本次真正会产出的插件名: sysmgr (框架自带) + _plugins 里的每个
set(_built "sysmgr")
foreach(_p IN LISTS _plugins)
    get_filename_component(_n "${_p}" NAME)
    list(APPEND _built "${_n}")
endforeach()

# 按 cano 顺序筛, 再补上没出现在 cano 里的
set(_final "")
foreach(_n IN LISTS _order)
    if(_n IN_LIST _built)
        list(APPEND _final "${_n}")
        list(REMOVE_ITEM _built "${_n}")
    endif()
endforeach()
foreach(_n IN LISTS _built)
    list(APPEND _final "${_n}")
endforeach()

set(_json "{\n  \"plugin\": [\n")
set(_first TRUE)
foreach(_n IN LISTS _final)
    if(NOT _first)
        string(APPEND _json ",\n")
    endif()
    set(_first FALSE)
    string(APPEND _json "    \"plugins/${_n}/plugin.json\"")
endforeach()
string(APPEND _json "\n  ]\n}\n")

file(WRITE "${BUILD}/config.json" "${_json}")
message(STATUS "mdpsr config   : ${BUILD}/config.json -> ${_final}")
