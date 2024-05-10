//
// Created by Nebelwolfi on 08/05/2024.
//
#include <pch.h>
#include "state.h"
#include "lef.h"
#include "lua/env.h"
#include "globals.h"
#include "import.h"
#include <signal.h>

std::vector<std::function<int(lua_State*)>> on_close;

void sigIntHandler(sig_atomic_t s){
    lua::env::detail::inst->should_exit = true;
}

BOOL consoleHandler(DWORD CEvent)
{
    if (CEvent == CTRL_CLOSE_EVENT) {
        lua::env::detail::inst->should_exit = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return TRUE;
    }
    return FALSE;
}

void load_lua_state_and_run(std::function<void(lua_State*)> func, bool compiled)
{
    signal(SIGINT, sigIntHandler);
    if (GetConsoleWindow()) {
        SetConsoleOutputCP(65001), SetConsoleCP(65001);
        SetConsoleCtrlHandler(consoleHandler, TRUE);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINITBASE_MULTITHREADED);
    if (FAILED(hr))
    {
        auto buffer = new wchar_t[256];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, hr, 0, buffer, 256, nullptr);
        std::cerr << "Failed to initialize COM library: " << std::endl;
        std::wcerr << buffer << std::endl;
        return;
    }

    lua::env::detail::inst = new lua::env::detail::_env {
        .should_exit = false,
        .should_restart = false,
        .should_relaunch = false,
        .is_compiled = compiled
    };
    // Set exe and exec_dir
    {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);

        auto exe_file_name = std::filesystem::path(exe_path).filename().string();
        lua::env::detail::inst->exe = static_cast<const char *>(malloc(exe_file_name.size() + 1));
        strcpy((char*)lua::env::detail::inst->exe, exe_file_name.c_str());

        auto exec_dir = std::filesystem::path(exe_path).parent_path().string();
        lua::env::detail::inst->exec_dir = static_cast<const char *>(malloc(exec_dir.size() + 1));
        strcpy((char*)lua::env::detail::inst->exec_dir, exec_dir.c_str());
    }
    lua::env::detail::inst->close_state = +[](int(*f)(lua_State*)) {
        on_close.emplace_back(f);
    };
    lua::env::detail::inst->new_state = +[]() -> lua_State* {
        lua::env::detail::inst->should_restart = false;
        lua::env::detail::inst->should_relaunch = false;
        lua::env::detail::inst->should_exit = false;

        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "__METASTORE");
        lua::env::open(L);
        luaopen_ffi(L);
        lua_setglobal(L, "ffi");
        globals::open(L);
        import_open(L);

        LefFile::loaded.clear();

        {
            lua_getglobal(L, LUA_LOADLIBNAME);
            lua_getfield(L, -1, "loaders");
            lua_pushcclosure(L, +[](lua_State* L) -> int {
                auto name = std::string(lua_tostring(L, 1));
                for (auto&& lefs : LefFile::loaded)
                for (auto&& file : lefs.files) {
                    if (std::ranges::equal(file.name, name, ichar_equals) || std::ranges::equal(file.name, "modules." + name, ichar_equals)) {
                        luaL_loadbuffer(L, file.data.c_str(), file.data.size(), ("=" + file.name).c_str());
                        return 1;
                    }
                }
                return 0;
            }, 0);
            lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
            lua_pop(L, 2);
        }
        {
            // package.path = package.path + ";%dir%/modules/?.lua";
            lua_getglobal(L, "package");
            lua_newtable(L);
            lua_setfield(L, -2, "modules");
            lua_getfield(L, -1, "path");
            std::string path = lua_tostring(L, -1);
            lua_pop(L, 1);
            path += ";" + std::filesystem::current_path().string() + "\\modules\\?.lua";
            lua_pushstring(L, path.c_str());
            lua_setfield(L, -2, "path");
            lua_pop(L, 1);
        }
        return L;
    };

    do {
        on_close.clear();
        globals::start_time = std::chrono::high_resolution_clock::now();
        lua_State* L = lua::env::new_state();
        func(L);
        lua_close(L);
        for (auto&& f : on_close)
            f(L);
    } while (lua::env::should_restart());

    CoUninitialize();
}

void load_lua_file(lua_State* L, const std::string& source)
{
    if (luaL_loadfile(L, source.c_str())) {
        std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
        return;
    }
}
void load_lua_memory(lua_State* L, const std::string& source, const std::string& chunk_name)
{
    //printf("source: %s\n", chunk_name.c_str());
    if (luaL_loadbuffer(L, source.c_str(), source.size(), ("=" + chunk_name).c_str())) {
        std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
        lua_pop(L, 1);
        return;
    }
}
void load_lef_file(lua_State* L, const std::string& path)
{
    LefFile file = LefFile::load_from_file(path).value();
    lua_createtable(L, file.args.size(), 0);
    for (int j = 0; j < file.args.size(); j++) {
        lua_pushstring(L, file.args[j].c_str());
        lua_rawseti(L, -2, j + 1);
    }
    lua_setglobal(L, "arg");
    load_lua_memory(L, file.files[0].data, file.files[0].name);
}
void load_lef_memory(lua_State* L, const std::string& data)
{
    LefFile file = LefFile::load_from_memory(data).value();
    lua_createtable(L, file.args.size(), 0);
    for (int j = 0; j < file.args.size(); j++) {
        lua_pushstring(L, file.args[j].c_str());
        lua_rawseti(L, -2, j + 1);
    }
    lua_setglobal(L, "arg");
    load_lua_memory(L, file.files[0].data, file.files[0].name);
}