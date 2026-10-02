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
# TB-195: ONE Lua runtime. lxe.exe links NO LuaJIT - not the static
# C:\LuaJIT-2.1.M.64\src\lua51.lib, not shared/lib/lua51dyn.lib. The runtime is
# loaded at RUN time (LoadLibrary + GetProcAddress) by src/lua/lua_runtime.cpp
# and every lua_* in lxe is a forwarding stub into it, so the exe and every
# module share ONE lua51.dll in one address space. shared/lib/lua51dyn.lib stays
# the import library the MODULES use (thread.dll, network.dll, ...) and is the
# import library that describes the DLL this exe loads.
# FORCE so a re-configure picks up a changed LUA51_DLL instead of keeping the
# first (possibly empty) value forever.
set(LUA51_DLL "$ENV{LUA51_DLL}" CACHE FILEPATH "lua51.dll to stage next to lxe.exe (TB-195)" FORCE)

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
        src/lua/lua_runtime.h
        src/lua/lua_runtime.cpp
        src/lua/globals.h
        src/lua/env.h
        src/is_path_in_PATH.cpp

        shared/include/luaxe/debug.h
        shared/include/luaxe/bind.h
        shared/include/luaxe/env.h
        shared/include/luaxe/luaxe.hpp
)

# TB-195: no LuaJIT library at all - the runtime is loaded at run time by
# src/lua/lua_runtime.cpp. bcrypt.dll is the OS's SHA-256, used to verify the
# runtime before it is loaded (it is in every Windows, and it is a delay-free
# import the OS resolves before main()).
target_link_libraries(lxe LINK_PUBLIC ws2_32.lib dbghelp.lib bcrypt.lib)

target_link_options(lxe PRIVATE /PDB:lxe.pdb)
target_compile_options(lxe PRIVATE "/Zi" PRIVATE "/O2")
target_link_options(lxe PRIVATE "/nologo" PRIVATE "/OPT:REF" PRIVATE "/OPT:ICF" PRIVATE "/INCREMENTAL" PRIVATE "/MACHINE:X64" PRIVATE "/LARGEADDRESSAWARE" PRIVATE "/LTCG"  PRIVATE "/DEBUG")
set_target_properties(lxe PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
set_target_properties(lxe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/")
add_custom_command(TARGET lxe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/bin/lxe.exe" "${CMAKE_SOURCE_DIR}/bin/lxe.exe")
# TB-195: stage the runtime the exe will load at run time, so a dev build starts
# without a download. Only when the caller says which build it wants.
if(LUA51_DLL AND EXISTS "${LUA51_DLL}")
    add_custom_command(TARGET lxe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LUA51_DLL}" "$<TARGET_FILE_DIR:lxe>/lua51.dll")
endif()

# Precompiled headers
target_precompile_headers(lxe PRIVATE src/pch.h)

# Versioning
add_custom_target(depender) # dummy target
add_custom_command(TARGET depender PRE_BUILD COMMAND powershell -ExecutionPolicy Bypass -NoProfile -NonInteractive -File ${CMAKE_SOURCE_DIR}\\update_version.ps1 ${CMAKE_SOURCE_DIR}\\version.h)
#add_dependencies(lxe depender)