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
    void(*close_state)(int(*)(lua_State*)) = nullptr;
};

static _env* inst = nullptr;
}
static void init(lua_State* L) {
    if (!detail::inst && L) {
        lua_getfield(L, LUA_REGISTRYINDEX, "_env");
        if (!lua_isnil(L, -1)) {
            detail::inst = *(detail::_env**)lua_touserdata(L, -1);
        }
        lua_pop(L, 1);
    }
}

static bool should_exit() {
    return detail::inst->should_exit;
}
static bool should_restart() {
    return detail::inst->should_restart;
}
static bool should_relaunch() {
    return detail::inst->should_relaunch;
}
static bool is_compiled() {
    return detail::inst->is_compiled;
}
static const char* exe() {
    return detail::inst->exe;
}
static const char* exec_dir() {
    return detail::inst->exec_dir;
}
static lua_State* new_state() {
    return detail::inst->new_state();
}
static void on_close(int(*f)(lua_State*)) {
    detail::inst->close_state(f);
}
}

#endif //LUAXE_ENV_H