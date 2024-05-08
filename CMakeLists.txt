cmake_minimum_required(VERSION 3.27)
project(LuaXE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_TOOLCHAIN_FILE $ENV{CMAKE_TOOLCHAIN_FILE})
add_compile_options(
        $<$<CONFIG:>:/MT> #---------|
        $<$<CONFIG:Debug>:/MTd> #---|-- Statically link the runtime libraries
        $<$<CONFIG:Release>:/MT> #--|
)
set_property(GLOBAL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

include_directories(.) # for pch.h
include_directories(C:\\LuaJIT-2.1.M.64\\src)
find_library(LUAJIT lua51 lib C:\\LuaJIT-2.1.M.64\\src\\)

set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_CURRENT_SOURCE_DIR}/appicon.rc")
file(GLOB_RECURSE https https/*.cpp https/*.h)

add_executable(luaxe main.cpp
        ${APP_ICON_RESOURCE_WINDOWS}
        pch.cpp
        ${https}
        commands/register.h
        commands/update.h
        commands/update.cpp
        lua/lef.h
        lua/state.h
        lua/state.cpp
        lua/lef.cpp
        commands/compile.h
        commands/run.h
        include/shared/env.h
        lua/import.h
        lua/globals.h
        lua/env.h
        include/shared/bind.h
)

target_link_libraries(luaxe LINK_PUBLIC ${LUAJIT} ws2_32.lib)

target_link_options(luaxe PRIVATE /PDB:luaxe.pdb)
target_compile_options(luaxe PRIVATE "/Zi" PRIVATE "/O2")
target_link_options(luaxe PRIVATE "/nologo" PRIVATE "/OPT:REF" PRIVATE "/OPT:ICF" PRIVATE "/INCREMENTAL" PRIVATE "/MACHINE:X64" PRIVATE "/LARGEADDRESSAWARE" PRIVATE "/LTCG"  PRIVATE "/DEBUG")
set_target_properties(luaxe PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
set_target_properties(luaxe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/")
add_custom_command(TARGET luaxe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/bin/luaxe.exe" "D:\\luaxe.dev\\luaxe.exe")

# Precompiled headers
target_precompile_headers(luaxe PRIVATE pch.h)

# Versioning
add_custom_target(depender) # dummy target
add_custom_command(TARGET depender PRE_BUILD COMMAND powershell -ExecutionPolicy Bypass -NoProfile -NonInteractive -File ${CMAKE_SOURCE_DIR}\\update_version.ps1 ${CMAKE_SOURCE_DIR}\\version.h)
add_dependencies(luaxe depender)