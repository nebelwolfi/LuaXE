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
include_directories(shared/include)
find_library(LUAJIT lua51 lib C:\\LuaJIT-2.1.M.64\\src\\)

file(GLOB_RECURSE https src/https/*.cpp src/https/*.h)

add_executable(lxe src/main.cpp
        "res/appicon.rc"
        src/commands/install.h
        src/stack_tracer.cpp
        src/stack_tracer.h
        src/pch.cpp
        ${https}
        src/commands/register.h
        src/commands/update.h
        src/commands/update.cpp
        src/commands/compile.h
        src/commands/run.h
        src/lua/lef.h
        src/lua/state.h
        src/lua/state.cpp
        src/lua/lef.cpp
        src/lua/import.h
        src/lua/globals.h
        src/lua/env.h
        src/is_path_in_PATH.cpp

        shared/include/luaxe/debug.h
        shared/include/luaxe/bind.h
        shared/include/luaxe/env.h
        shared/include/luaxe/luaxe.hpp
)

target_link_libraries(lxe LINK_PUBLIC ${LUAJIT} ws2_32.lib dbghelp.lib)

target_link_options(lxe PRIVATE /PDB:lxe.pdb)
target_compile_options(lxe PRIVATE "/Zi" PRIVATE "/O2")
target_link_options(lxe PRIVATE "/nologo" PRIVATE "/OPT:REF" PRIVATE "/OPT:ICF" PRIVATE "/INCREMENTAL" PRIVATE "/MACHINE:X64" PRIVATE "/LARGEADDRESSAWARE" PRIVATE "/LTCG"  PRIVATE "/DEBUG")
set_target_properties(lxe PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
set_target_properties(lxe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/")
add_custom_command(TARGET lxe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/bin/lxe.exe" "${CMAKE_SOURCE_DIR}/bin/lxe.exe")

# Precompiled headers
target_precompile_headers(lxe PRIVATE src/pch.h)

# Versioning
add_custom_target(depender) # dummy target
add_custom_command(TARGET depender PRE_BUILD COMMAND powershell -ExecutionPolicy Bypass -NoProfile -NonInteractive -File ${CMAKE_SOURCE_DIR}\\update_version.ps1 ${CMAKE_SOURCE_DIR}\\version.h)
#add_dependencies(lxe depender)