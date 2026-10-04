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
#include "lua_runtime.h"
#include "modules_dir.h"
#include "lef.h"

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

// Resolution and installation are serialized per process: two states importing
// at once must not race on installed_modules or on the files being written.
// The module itself is loaded outside the lock (its luaopen may import more).
static std::mutex import_mutex;

// Where import() looks for a module, in order (src/lua/modules_dir.h):
//   - a module the running payload BUNDLES: the modules directory only, never
//     installed or updated - the payload's own copy wins over any other;
//   - a compiled program (an .exe, or `lxe run x.lef`): the modules directory
//     only. Its cwd is the user's workspace, not a place to load DLLs from;
//   - a source run: the project's own <cwd>\modules first (what `lxe install`
//     manages), then the modules directory.
// A module found nowhere is installed into the modules directory - never into
// whatever directory the program happened to be started from.
static std::vector<std::filesystem::path> import_roots(const std::string& modulename, bool compiled) {
    if (LefFile::bundled_modules.contains(modulename)) return { modules_dir::home() };
    std::vector<std::filesystem::path> roots;
    if (!compiled) {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) roots.push_back(cwd / "modules");
    }
    roots.push_back(modules_dir::home());
    return roots;
}

/// A module name import() may join into a modules root: no separators, no
/// drive, no `..` - it names a folder INSIDE the root, never one above it.
static bool valid_import_name(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    return name.front() != '.' && name.back() != '.';
}

static int import(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (!valid_import_name(name))
        return luaL_error(L, "import: \"%s\" is not a module name", name.c_str());
    const bool compiled = lua::env::is_compiled();
    std::string version = "latest";
    if (lua_isstring(L, 2))
        version = lua_tostring(L, 2);
    else {
        // A pinned version: the first modules root that has a module.json.
        auto pin_name = name.substr(0, name.find('.'));
        for (const auto& root : import_roots(pin_name, compiled)) {
            std::error_code ec;
            if (!std::filesystem::exists(root / "module.json", ec)) continue;
            auto j = read_module_json(root);
            if (j.contains("dependencies") && j["dependencies"].contains(name) && j["dependencies"][name].is_string())
                version = j["dependencies"][name].get<std::string>();
            break;
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
    const bool bundled = LefFile::bundled_modules.contains(modulename);
    const auto roots = import_roots(modulename, compiled);
    auto find_root = [&]() -> std::filesystem::path {
        for (const auto& root : roots) {
            std::error_code ec;
            if (std::filesystem::exists(root / modulename, ec)) return root;
        }
        return {};
    };
    std::filesystem::path root;
    {
        std::lock_guard lock(import_mutex);
        root = find_root();
        bool bShouldUpdate = true;
        if (bundled) {
            bShouldUpdate = false;
        } else if (!bHasDot && (version == "local" || compiled)) {
            bShouldUpdate = root.empty();
        }
        if (bShouldUpdate && !installed_modules.contains(modulename)) {
            // Marked done only when it worked: a failed install (offline, an
            // unwritable folder) is tried again by the next import.
            if (install_module(modulename + "@" + version, root.empty() ? modules_dir::home() : root))
                installed_modules.insert(modulename);
            root = find_root();
        }
    }
    if (root.empty()) {
        // Found nowhere and not installable: say so, instead of answering nil.
        std::string where;
        for (const auto& candidate : roots) where += (where.empty() ? "" : ", ") + candidate.string();
        return luaL_error(L, "import: module \"%s\" is not installed in %s%s", modulename.c_str(), where.c_str(),
            bundled ? " (it is bundled, but could not be unpacked there)" : " and could not be installed");
    }
    std::error_code ec;
    bool isDllInclude = std::filesystem::exists(root / modulename / (name_after_dot + ".dll"), ec);
    if (!isDllInclude && !bHasDot) {
        bool hasLuaFile = std::filesystem::exists(root / modulename / (name_after_dot + ".lua"), ec);
        if (!hasLuaFile) {
            // probably tried to just install a non-dll module, no need to load anything
            return 0;
        }
        name = modulename + "." + name_after_dot; // try to load lua file instead
    }
    // The module sees its own arguments as `arg` while it loads; the caller's
    // `arg` is restored afterwards - also when the module raises. The saved value
    // is held at an ABSOLUTE index: ll_loadfunc leaves values of its own on the
    // stack (its registry entry, the luaopen_ symbol name), so "two below the
    // result" was one of those and `arg` came back as the string "luaopen_x".
    lua_getglobal(L, "arg");
    const int saved_arg = lua_gettop(L);
    lua_newtable(L);
    for (auto i = 1; i <= argc; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
    int status = 0;
    if (isDllInclude)
    {
        auto modules_base = root;
        {
            lua_getglobal(L, "package");
            lua_getfield(L, -1, "cpath");
            const char* current = lua_tostring(L, -1);
            std::string cpath = current ? current : "";
            lua_pop(L, 1);
            cpath += ";" + (modules_base / modulename).string() + "\\?.dll";
            lua_pushstring(L, cpath.c_str());
            lua_setfield(L, -2, "cpath");
            lua_pop(L, 1);
        }
        // TB-195: no download here any more. The runtime is bound once, before any
        // state exists (lua_runtime::ensure, called from load_lua_state_and_run
        // and LefFile::store_as_lef), verified against a pinned SHA-256 and
        // cached beside the executable. This path could only fire if a lua_State
        // existed without it, which is impossible - and it would have downloaded
        // an UNVERIFIED runtime into the working directory, which is exactly the
        // hole the one-runtime change closes.
        if (!lua_runtime::loaded()) {
            lua_pushvalue(L, saved_arg);
            lua_setglobal(L, "arg");
            lua_pushstring(L, "the Lua runtime is not loaded; this is a LuaXE bug");
            lua_error(L);
            return 0;
        }
        AddDllDirectory((modules_base / modulename).wstring().c_str());
        const int top = lua_gettop(L);
        if (ll_loadfunc(L, ((modules_base / modulename / name_after_dot).string() + ".dll").c_str(), name.c_str(), 0)) {
            lua_settop(L, top);
            lua_getglobal(L, "require");
            lua_pushstring(L, name.c_str());
            status = lua_pcall(L, 1, 1, 0);
            isDllInclude = false;
        } else {
            // keep only the loaded function (ll_loadfunc's leftovers sit below it)
            if (lua_gettop(L) > top + 1) {
                lua_replace(L, top + 1);
                lua_settop(L, top + 1);
            }
            status = lua_pcall(L, 0, 1, 0);
        }
    } else {
        lua_getglobal(L, "require");
        lua_pushstring(L, name.c_str());
        status = lua_pcall(L, 1, 1, 0);
    }
    lua_pushvalue(L, saved_arg);
    lua_setglobal(L, "arg");
    if (status != 0) return lua_error(L); // the module's own error, unchanged
    return isDllInclude ? lua_pushandstore(L, name.c_str()) : 1;
}

static int import_open(lua_State* L) {
    lua_pushcfunction(L, import);
    lua_setglobal(L, "import");
    return 0;
}

#endif //LUAXE_IMPORT_H
