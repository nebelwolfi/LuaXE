//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LUA_ENV_H
#define LUAXE_LUA_ENV_H

#include "shared/include/luaxe/env.h"
#include "shared/include/luaxe/bind.h"
#include "src/lua/modules_dir.h"
#include "src/lua/lef.h"

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
    // env.find_module(name[, range]) -> the folder import(name) would load from
    // WITHOUT installing anything (bundled, then the project's flat folder in a
    // source run, then the best installed version in the store), or nil.
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
        if (!lua::env::is_compiled()) {
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
    env.fun("relaunch", [](lua_State* L) -> int {
        detail::inst->should_exit = true;
        detail::inst->should_relaunch = true;
        detail::inst->should_restart = false;
        return 0;
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
