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
    void(*set_shared_data)(const char*, void*, size_t) = nullptr;
    void*(*get_shared_data)(const char*) = nullptr;
};

static _env* inst = nullptr;
}
static bool init(lua_State* L) {
    if (!detail::inst && L) {
        lua_getfield(L, LUA_REGISTRYINDEX, "_env");
        if (!lua_isnil(L, -1)) {
            detail::inst = *(detail::_env**)lua_touserdata(L, -1);
        }
        lua_pop(L, 1);
        return detail::inst;
    }
    return false;
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
template<typename T>
static void set(const char* key, T&& data) {
    detail::inst->set_shared_data(key, &data, sizeof(T));
}
template<typename T>
static void set(const char* key, T* data) {
    detail::inst->set_shared_data(key, data, sizeof(T));
}
template<typename T>
static T* get(const char* key) {
    return detail::inst->get_shared_data(key);
}
}

#endif //LUAXE_ENV_H