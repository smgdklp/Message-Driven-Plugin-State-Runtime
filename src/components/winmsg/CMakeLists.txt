# ============================================================================
#  winmsg —— GUI broker (框架里唯一碰 Win32 的组件)
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mdpsr_winmsg LANGUAGES CXX)

if(NOT TARGET mdpsr_common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake"
                     "${CMAKE_BINARY_DIR}/cmake_common")
endif()

add_library(mdpsr_winmsg MODULE winmsg.cpp)
target_link_libraries(mdpsr_winmsg PRIVATE mdpsr_common msimg32)   # AlphaBlend

set_target_properties(mdpsr_winmsg PROPERTIES PREFIX "" OUTPUT_NAME "mdpsr_winmsg")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(mdpsr_winmsg PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/winmsg"
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/winmsg"
        ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${MDPSR_OUT}/plugins/winmsg")
endforeach()

add_custom_command(TARGET mdpsr_winmsg POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
            "$<TARGET_FILE_DIR:mdpsr_winmsg>/plugin.json"
    COMMENT "copy winmsg/plugin.json")
