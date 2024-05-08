//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_ENV_H
#define LUAXE_ENV_H

namespace lua::env {
namespace detail {
struct _env {
    bool should_exit = false;
    bool should_restart = false;
    bool should_relaunch = false;
    bool is_compiled = false;
    const char* exe = nullptr;
    const char* exec_dir = nullptr;
    lua_State*(*new_state)() = nullptr;
};

static _env* inst = nullptr;
}
static void init(lua_State* L) {
    if (!detail::inst && L) {
        lua_getfield(L, LUA_REGISTRYINDEX, "_env");
        if (!lua_isnil(L, -1)) {
            detail::inst = (detail::_env*)lua_touserdata(L, -1);
        }
        lua_pop(L, 1);
    }
}

bool should_exit() {
    return detail::inst->should_exit;
}
bool should_restart() {
    return detail::inst->should_restart;
}
bool should_relaunch() {
    return detail::inst->should_relaunch;
}
bool is_compiled() {
    return detail::inst->is_compiled;
}
const char* exe() {
    return detail::inst->exe;
}
const char* exec_dir() {
    return detail::inst->exec_dir;
}
lua_State* new_state() {
    return detail::inst->new_state();
}
}

#endif //LUAXE_ENV_H