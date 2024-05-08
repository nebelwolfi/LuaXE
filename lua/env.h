//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LUA_ENV_H
#define LUAXE_LUA_ENV_H

#include "include/shared/env.h"
#include "include/shared/bind.h"

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

    push(L, detail::inst);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "_env");
    lua_setglobal(L, "env");
}
}

#endif //LUAXE_LUA_ENV_H
