//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_IMPORT_H
#define LUAXE_IMPORT_H

#include "../https/connection/API.h"
#include "../https/misc/json.hpp"
#include "../https/misc/md5.h"
#include <sys/utime.h>
#include "../commands/install.h"
#include <unordered_set>

extern "C" int ll_loadfunc(lua_State *L, const char *path, const char *name, int r);

int lua_pushandstore(lua_State *L, const char *name) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "modules");
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
    return 1;
}

std::unordered_map<std::string, std::mutex> fs_mutexes;

static __forceinline bool ichar_equals(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

std::unordered_set<std::string> installed_modules;

static int import(lua_State* L) {
    std::string name = lua_tostring(L, 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::string version = "latest";
    if (lua_isstring(L, 2))
        version = lua_tostring(L, 2);
    else {
        nlohmann::json j;
        if (std::filesystem::exists("modules/module.json")) {
            std::ifstream file("modules/module.json");
            file >> j;
            if (j.contains("dependencies") && j["dependencies"].contains(name))
                version = j["dependencies"][name];
        }
    }
    {
        // check if its already in package.modules
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "modules");
        lua_pushvalue(L, 1); // push name
        lua_gettable(L, -2);
        if (!lua_isnil(L, -1)) {
            return 1;
        }
        lua_pop(L, 3);
    }

    lua_remove(L, 1); // remove name
    if (!lua_isnone(L, 1))
        lua_remove(L, 1); // remove version
    auto argc = lua_gettop(L);
    auto modulename = name;
    auto name_after_dot = name;
    bool bHasDot = false;
    if (modulename.find('.') != std::string::npos) {
        modulename = modulename.substr(0, modulename.find("."));
        name_after_dot = name.substr(name.find('.')+1);
        bHasDot = true;
    }
    bool bShouldUpdate = true;
    if (!bHasDot && (version == "local" || lua::env::is_compiled())) {
        bShouldUpdate = !std::filesystem::exists(std::filesystem::path("modules") / modulename);
    }
    if (bShouldUpdate && !installed_modules.contains(modulename)) {
        install_module(modulename + "@" + version);
        installed_modules.insert(modulename);
    }
    //printf("Importing module: %s | bHasDot: %d | moduleentry: %s\n", modulename.c_str(), bHasDot, (std::filesystem::path("modules") / modulename / (name_after_dot + ".dll")).string().c_str());
    bool isDllInclude = std::filesystem::exists(std::filesystem::path("modules") / modulename / (name_after_dot + ".dll"));
    if (!isDllInclude && !bHasDot) {
        bool hasLuaFile = std::filesystem::exists(std::filesystem::path("modules") / modulename / (name_after_dot + ".lua"));
        if (!hasLuaFile) {
            // probably tried to just install a non-dll module, no need to load anything
            return 0;
        }
        name = modulename + "." + name_after_dot; // try to load lua file instead
    }
    lua_getglobal(L, "arg");
    lua_newtable(L);
    for (auto i = 1; i <= argc; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
    if (isDllInclude)
    {
        {
            // package.cpath = package.cpath + ";%dir%/modules/" + name + "/?.dll";
            lua_getglobal(L, "package");
            lua_getfield(L, -1, "cpath");
            std::string cpath = lua_tostring(L, -1);
            lua_pop(L, 1);
            cpath += ";" + std::filesystem::current_path().string() + "\\modules\\" + modulename + "\\?.dll";
            lua_pushstring(L, cpath.c_str());
            lua_setfield(L, -2, "cpath");
            lua_pop(L, 1);
        }
        if (!std::filesystem::exists(std::filesystem::current_path() / "modules" / "lua51.dll")) {
            API a;
            if (!a.DownloadFile("luaxe.dev", "/module/lua51.dll", (std::filesystem::current_path() / "modules" / "lua51.dll").string())) {
                lua_pushstring(L, "Failed to download lua51.dll");
                lua_error(L);
                return 0;
            } else
                AddDllDirectory((std::filesystem::current_path() / "modules").wstring().c_str());
        }
        AddDllDirectory((std::filesystem::current_path() / "modules" / modulename).wstring().c_str());
        if (ll_loadfunc(L, ((std::filesystem::current_path() / "modules" / modulename / name_after_dot).string() + ".dll").c_str(), name.c_str(), 0)) {
            lua_pop(L, 1);
            lua_getglobal(L, "require");
            lua_pushstring(L, name.c_str());
            lua_call(L, 1, 1);
            isDllInclude = false;
        } else lua_call(L, 0, 1);
    } else {
        lua_getglobal(L, "require");
        lua_pushstring(L, name.c_str());
        lua_call(L, 1, 1);
    }
    lua_pushvalue(L, -2);
    lua_setglobal(L, "arg");
    return isDllInclude ? lua_pushandstore(L, name.c_str()) : 1;
}

static int import_open(lua_State* L) {
    lua_pushcfunction(L, import);
    lua_setglobal(L, "import");
    return 0;
}

#endif //LUAXE_IMPORT_H
