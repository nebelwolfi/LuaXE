//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LUA_ENV_H
#define LUAXE_LUA_ENV_H

#include "shared/include/luaxe/env.h"
#include "shared/include/luaxe/bind.h"
#include "src/lua/modules_dir.h"
#include "src/lua/lef.h"
#include "src/lua/relaunch.h"

namespace lua::env {
/// The .lef this process runs (`lxe app.lef` / `lxe run app.lef`), absolute;
/// empty for a .lua or a compiled .exe. Lets an app relaunch itself the way it
/// was started: "<lxe.exe> <lef_path> ...".
inline std::string lef_path;

static void open(lua_State*L) {
    auto env = bind::add<detail::_env>(L, "env");

    env.prop("should_exit", [](lua_State* L) -> int {
        lua_pushboolean(L, detail::inst->should_exit);
        return 1;
    }, [](lua_State* L) -> int {
        detail::inst->should_exit = lua_toboolean(L, 3);
        return 0;
    });
    env.prop("should_restart", [](lua_State* L) -> int {
        lua_pushboolean(L, detail::inst->should_restart);
        return 1;
    }, [](lua_State* L) -> int {
        detail::inst->should_restart = lua_toboolean(L, 3);
        return 0;
    });
    env.prop("should_relaunch", [](lua_State* L) -> int {
        lua_pushboolean(L, detail::inst->should_relaunch);
        return 1;
    }, [](lua_State* L) -> int {
        detail::inst->should_relaunch = lua_toboolean(L, 3);
        return 0;
    });
    env.prop("is_compiled", [](lua_State* L) -> int {
        lua_pushboolean(L, detail::inst->is_compiled);
        return 1;
    }, [](lua_State* L) -> int {
        detail::inst->is_compiled = lua_toboolean(L, 3);
        return 0;
    });
    // lxe's versioned module store, %USERPROFILE%\.lxe\modules (src/lua/modules_dir.h).
    env.prop("modules_dir", [](lua_State* L) -> int {
        lua_pushstring(L, modules_dir::store().string().c_str());
        return 1;
    });
    // %USERPROFILE%\.lxe (or LXE_HOME).
    env.prop("lxe_home", [](lua_State* L) -> int {
        lua_pushstring(L, modules_dir::lxe_home().string().c_str());
        return 1;
    });
    // The folder the running app's bundled modules unpack to (<app dir>\modules),
    // nil when nothing runs as an app.
    env.prop("app_modules_dir", [](lua_State* L) -> int {
        if (modules_dir::app_dir().empty()) return 0;
        lua_pushstring(L, (modules_dir::app_dir() / "modules").string().c_str());
        return 1;
    });
    // env.find_module(name[, range[, store_only]]) -> the folder import(name)
    // would load from WITHOUT installing anything (bundled, then the project's
    // flat folder in a source run, then the best installed version in the
    // store), or nil. store_only = true asks the store alone: never a flat
    // modules\ folder of the cwd, which for an app is the user's workspace.
    env.fun("find_module", [](lua_State* L) -> int {
        std::string name = luaL_checkstring(L, 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::string range = lua_isstring(L, 2) ? lua_tostring(L, 2) : "*";
        if (name.empty() || name.find_first_of("/\\:.") != std::string::npos) return 0;
        std::error_code ec;
        if (auto bundled = LefFile::bundled_modules.find(name); bundled != LefFile::bundled_modules.end()) {
            if (!std::filesystem::is_directory(bundled->second, ec)) return 0;
            lua_pushstring(L, bundled->second.string().c_str());
            return 1;
        }
        const bool store_only = lua_toboolean(L, 3) != 0;
        if (!lua::env::is_compiled() && !store_only) {
            for (const auto& root : modules_dir::source_roots()) {
                if (std::filesystem::is_directory(root / name, ec)) {
                    lua_pushstring(L, (root / name).string().c_str());
                    return 1;
                }
            }
        }
        auto dir = modules_dir::best_installed(name, range);
        if (dir.empty()) return 0;
        lua_pushstring(L, dir.string().c_str());
        return 1;
    });
    env.prop("lef_path", [](lua_State* L) -> int {
        if (lef_path.empty()) return 0;
        lua_pushstring(L, lef_path.c_str());
        return 1;
    });
    env.fun("exit", [](lua_State* L) -> int {
        ::exit(luaL_optinteger(L, 1, 0));
        return 0;
    });
    // env.relaunch([args]): when the program returns, start it again as a fresh
    // process with `args` (a table of strings; none = no arguments) - see
    // src/lua/relaunch.h. The program still has to return (or end) on its own.
    // Call it from the program's main state only.
    env.fun("relaunch", [](lua_State* L) -> int {
        std::vector<std::string> args;
        if (lua_istable(L, 1)) {
            int count = (int)lua_objlen(L, 1);
            for (int i = 1; i <= count; i++) {
                lua_rawgeti(L, 1, i);
                size_t length = 0;
                const char* value = lua_tolstring(L, -1, &length);
                if (!value) {
                    lua_pop(L, 1);
                    return luaL_error(L, "env.relaunch: argument %d is not a string", i);
                }
                if (length > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, (int)length, nullptr, 0) == 0) {
                    lua_pop(L, 1);
                    return luaL_error(L, "env.relaunch: argument %d is not valid UTF-8", i);
                }
                args.emplace_back(value, length);
                lua_pop(L, 1);
            }
        } else if (!lua_isnoneornil(L, 1)) {
            return luaL_error(L, "env.relaunch: expected a table of arguments");
        }
        relaunch::request(args);
        // The runtime (lua51.dll) has a C runtime of its own: what the program
        // printed sits in ITS stdout buffer, which a nested relaunch (this
        // process stays alive as the waiting parent) would never flush. Flush
        // it when the state closes.
        lua::env::on_close(+[](lua_State* L) -> int {
            luaL_dostring(L, "pcall(function() io.stdout:flush() end) pcall(function() io.stderr:flush() end)");
            return 0;
        });
        detail::inst->should_exit = true;
        detail::inst->should_relaunch = true;
        detail::inst->should_restart = false;
        return 0;
    });
    // "launcher" when a launcher started this process (the relaunch happens in
    // its console), "nested" when lxe will start and wait for the next run itself.
    env.prop("relaunch_mode", [](lua_State* L) -> int {
        lua_pushstring(L, relaunch::mode());
        return 1;
    });
    env.fun("reload", [](lua_State* L) -> int {
        detail::inst->should_restart = true;
        return 0;
    });
    env.prop("cpu_cores", [](lua_State* L) -> int {
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        lua_pushinteger(L, sysinfo.dwNumberOfProcessors);
        return 1;
    });
    env.prop("clipboard", [](lua_State *L) -> int {
        if (!OpenClipboard(NULL))
           return 0;
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData == NULL) {
           CloseClipboard();
           return 0;
        }
        char* pszText = static_cast<char*>(GlobalLock(hData));
        if (pszText == NULL) {
           CloseClipboard();
           return 0;
        }
        lua_pushstring(L, pszText);
        GlobalUnlock(hData);
        CloseClipboard();
        return 1;
    }, [](lua_State *L) -> int {
        if (!OpenClipboard(NULL))
            return 0;
        EmptyClipboard();
        size_t len;
        auto s = luaL_checklstring(L, 3, &len);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if (!hg) {
            CloseClipboard();
            return 0;
        }
        memcpy(GlobalLock(hg), s, len + 1);
        GlobalUnlock(hg);
        SetClipboardData(CF_TEXT, hg);
        CloseClipboard();
        GlobalFree(hg);
        return 0;
    });

    push(L, detail::inst);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "_env");
    lua_setglobal(L, "env");
}
}

#endif //LUAXE_LUA_ENV_H
