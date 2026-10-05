//
// Created by Nebelwolfi on 08/05/2024.
//
#include "../pch.h"
#include "state.h"
#include "lef.h"
#include "env.h"
#include "globals.h"
#include "import.h"
#include "lua_runtime.h"
#include "modules_dir.h"
#include <csignal>
#include "src/stack_tracer.h"

std::vector<int(*)(lua_State*)> on_close;

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

std::unordered_map<std::string, void*> shared_data;
StackTracer tracer;

void load_lua_state_and_run(std::function<void(lua_State*)> func, bool compiled)
{
    // TB-195: ONE Lua runtime, bound before the first lua_* call. lxe.exe links
    // no LuaJIT; this loads lua51.dll (beside the exe, then <exe>\modules, then
    // the payload a compiled exe just extracted) and points every lua_* call at
    // it, which is also the image every module's lua51dyn.lib import resolves to.
    // A payload's bundled lua51.dll was unpacked into <exe>\modules beforehand
    // (LefFile::extract_bundled_dlls), which is one of those places.
    {
        std::string runtime_error;
        if (!lua_runtime::ensure({}, &runtime_error)) {
            std::cerr << "Error: no usable Lua runtime." << std::endl
                      << "  " << runtime_error << std::endl;
            std::exit(1);
        }
    }

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

    if (!IsDebuggerPresent())
        AddVectoredExceptionHandler(0, +[](PEXCEPTION_POINTERS ExceptionInfo) -> LONG {
            if (ExceptionInfo->ExceptionRecord->ExceptionCode < 0x80000000) { // Not an error
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (ExceptionInfo->ExceptionRecord->ExceptionCode >= 0xE24C4A00
                && ExceptionInfo->ExceptionRecord->ExceptionCode <= 0xE24C4AFF) { // LuaJIT
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (ExceptionInfo->ExceptionRecord->ExceptionCode == 0xE06D7363) { // C++
                return EXCEPTION_CONTINUE_SEARCH;
            }
            tracer.HandleException(ExceptionInfo);
            std::cerr << "An uncaught exception occurred." << std::endl;
            std::cerr << tracer.GetExceptionMsg() << std::endl;
            return EXCEPTION_CONTINUE_SEARCH;
        });

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
        if (std::find_if(on_close.begin(), on_close.end(), [&](auto&& a) { return a == f; }) == on_close.end())
            on_close.push_back(f);
    };
    lua::env::detail::inst->set_shared_data = +[](const char* key, void* data, size_t size) {
        if (!data) {
            shared_data.erase(key);
            return;
        }
        if (shared_data.contains(key)) {
            free(shared_data[key]);
        }
        shared_data[key] = malloc(size);
        memcpy(shared_data[key], data, size);
    };
    lua::env::detail::inst->get_shared_data = +[](const char* key) -> void* {
        return shared_data[key];
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

        {
            // The payload's own modules are searched LAST, after package.path:
            // a program may point package.path at another source tree on
            // purpose (girl's --girl-code-root does exactly that), so the order
            // is the program's to manage, not lxe's.
            lua_getglobal(L, LUA_LOADLIBNAME);
            lua_getfield(L, -1, "loaders");
            lua_pushcclosure(L, +[](lua_State* L) -> int {
                // TB-399 item 1: find_chunk hands back the chunk held by a
                // shared_ptr, so a concurrent clear_loaded()/register_loaded() on
                // the main state cannot free these bytes while they are loaded.
                auto name = std::string(lua_tostring(L, 1));
                if (auto chunk = LefFile::find_chunk(name)) {
                    luaL_loadbuffer(L, chunk->data.c_str(), chunk->data.size(), ("=" + chunk->name).c_str());
                    return 1;
                }
                return 0;
            }, 0);
            lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
            lua_pop(L, 2);
        }
        {
            // package.path += the project's flat <cwd>\modules for a source run
            // (import.h). The store is versioned: import() resolves a module's
            // folder, and require("name.sub") finds its files from there.
            lua_getglobal(L, "package");
            lua_newtable(L);
            lua_setfield(L, -2, "modules");
            lua_getfield(L, -1, "path");
            std::string path = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (!lua::env::detail::inst->is_compiled) {
                std::error_code ec;
                auto cwd = std::filesystem::current_path(ec);
                if (!ec) path += ";" + cwd.string() + "\\modules\\?.lua";
            }
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
        LefFile::clear_loaded();
        if (!IsDebuggerPresent())
            __try {
                func(L);
            } __except (tracer.ExceptionFilter(GetExceptionInformation())) {
                std::cerr << "An exception occurred." << std::endl;
                std::cerr << tracer.GetExceptionMsg() << std::endl;
            }
        else
            func(L);
        for (auto&& f : on_close)
            f(L);
        lua_close(L);
    } while (lua::env::should_restart());

    CoUninitialize();
}

static void push_failing_chunk(lua_State* L, const std::string& message);

void load_lua_file(lua_State* L, const std::string& source)
{
    if (luaL_loadfile(L, source.c_str())) {
        // Leave a chunk that raises the load error: the caller's pcall then
        // reports it once and fails, instead of calling nothing successfully.
        const char* message = lua_tostring(L, -1);
        std::string text = message ? message : ("cannot load " + source);
        lua_pop(L, 1);
        push_failing_chunk(L, text);
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
// Leaves a chunk that raises `message` on the stack, for a payload that cannot
// be read: the caller pcalls whatever these functions leave there.
static void push_failing_chunk(lua_State* L, const std::string& message)
{
    lua_pushstring(L, message.c_str());
    lua_pushcclosure(L, +[](lua_State* L) -> int {
        lua_pushvalue(L, lua_upvalueindex(1));
        return lua_error(L);
    }, 1);
}

void load_lef_file(lua_State* L, const std::string& path, int first_arg)
{
    auto parsed = LefFile::load_from_file(path);
    if (!parsed) {
        push_failing_chunk(L, path + " is not a valid .lef file");
        return;
    }
    LefFile file = std::move(*parsed);
    // Like a compiled exe's payload (load_lef_memory): the program sees only its
    // own arguments, never lxe's `run <file>.lef` in front of them.
    if (first_arg < 1) first_arg = 1;
    lua_createtable(L, file.args.size() + (__argc > first_arg ? __argc - first_arg : 0), 0);
    for (int j = 0; j < file.args.size(); j++) {
        lua_pushstring(L, file.args[j].c_str());
        lua_rawseti(L, -2, j + 1);
    }
    for (int j = first_arg; j < __argc; j++) {
        lua_pushstring(L, __argv[j]);
        lua_rawseti(L, -2, j - first_arg + 1 + file.args.size());
    }
    lua_setglobal(L, "arg");
    load_lua_memory(L, file.files[0].data, file.files[0].name);
}
void load_lef_memory(lua_State* L, const std::string& data)
{
    auto parsed = LefFile::load_from_memory(data);
    if (!parsed) {
        push_failing_chunk(L, "this executable's payload is not a valid .lef");
        return;
    }
    LefFile file = std::move(*parsed);
    lua_createtable(L, file.args.size() + __argc - 1, 0);
    for (int j = 0; j < file.args.size(); j++) {
        lua_pushstring(L, file.args[j].c_str());
        lua_rawseti(L, -2, j + 1);
    }
    for (int j = 1; j < __argc; j++) {
        lua_pushstring(L, __argv[j]);
        lua_rawseti(L, -2, j + file.args.size());
    }
    lua_setglobal(L, "arg");
    load_lua_memory(L, file.files[0].data, file.files[0].name);
}