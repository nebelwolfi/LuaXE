//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LUA_ENV_H
#define LUAXE_LUA_ENV_H

#include <luaxe/env.h>
#include <luaxe/bind.h>

namespace lua::env {
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
