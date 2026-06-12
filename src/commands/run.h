//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_RUN_H
#define LUAXE_RUN_H

#include "src/lua/state.h"
#include <luaxe/bind.h>

static void parse_and_run(std::ostream& out, int i) {
    std::string source;
    if (__argc <= i + 1) {
        if (std::filesystem::exists("main.lua")) {
            source = "main.lua";
        } else if (std::filesystem::exists("main.lef")) {
            source = "main.lef";
        } else {
            out << "No source provided, try \"luaxe help compile\"." << std::endl;
            return;
        }
    } else {
        source = __argv[++i];
        if (!source.ends_with(".lua") && !source.ends_with(".lef")) {
            out << "Invalid source provided, try \"luaxe help run\"." << std::endl;
            return;
        }
        i++;
    }
    load_lua_state_and_run([&](lua_State* L) {
        lua_createtable(L, __argc - i, 0);
        for (int j = i; j < __argc; j++) {
            lua_pushstring(L, __argv[j]);
            lua_rawseti(L, -2, j - i + 1);
        }
        lua_setglobal(L, "arg");
        if (source.ends_with(".lua")) {
            load_lua_file(L, source);
        } else {
            load_lef_file(L, source);
        }
        if (lua::pcall(L, 0, 0) != LUA_OK) {
            std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
            lua_pop(L, 1);
        }
    }, false);
}

#endif //LUAXE_RUN_H
