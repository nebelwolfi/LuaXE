//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_GLOBALS_H
#define LUAXE_GLOBALS_H

namespace globals {
static int printf(lua_State *L) {
    lua_getglobal(L, "print");
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_insert(L, 1);
    lua_pop(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    lua_call(L, 1, 0);
    return 1;
}

decltype(std::chrono::high_resolution_clock::now()) start_time;
static int clock(lua_State *L) {
    size_t time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
    lua_pushnumber(L, (double)time * 0.000001);
    return 1;
}

int lua_sleep(lua_State* L) {
    std::this_thread::sleep_for(std::chrono::milliseconds(luaL_checkinteger(L, 1)));
    return 0;
}

static int open(lua_State* L) {
    lua_register(L, "printf", globals::printf);
    lua_register(L, "clock", globals::clock);
    lua_getglobal(L, "os");
    lua_pushcfunction(L, globals::clock);
    lua_setfield(L, -2, "clock");
    lua_pop(L, 1);
    lua_register(L, "sleep", globals::lua_sleep);
    return 1;
}
}

#endif //LUAXE_GLOBALS_H
