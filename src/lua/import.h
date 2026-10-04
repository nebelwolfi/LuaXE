//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_IMPORT_H
#define LUAXE_IMPORT_H

#include "../https/connection/API.h"
#include "../https/misc/json.hpp"
#include "../https/misc/md5.h"
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

static __forceinline bool ichar_equals(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

// Resolution and installation are serialized per process: two states importing
// at once must not race on the store. The module itself is loaded outside the
// lock (its luaopen may import more).
static std::mutex import_mutex;

// The folder each module name resolved to in this process, so a later
// `require("name.sub")` (a module's own Lua files) finds name\sub.lua there.
static std::map<std::string, std::filesystem::path> resolved_dirs;

/// A module name import() may join into a folder: no separators, no drive, no
/// `..` - it names a folder INSIDE a root, never one above it.
static bool valid_import_name(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    return name.front() != '.' && name.back() != '.';
}

/// Where `import(name, version)` loads from (src/lua/modules_dir.h), in order:
///   1. a module the running app BUNDLES: its folder next to the app - never
///      installed or updated, whatever version was asked for;
///   2. a source run only: a flat <root>\modules\<name>\ (the layout before
///      the store, and what a checkout stages): the cwd's, the script's own
///      folder's, then the lxe executable's (modules_dir::source_roots);
///   3. lxe's store, ~\.lxe\modules\<name>\<version>\:
///        "1.2.3"          that version, installed if it is missing
///        a range ">=1", "^1.2" ...  the best installed match, else the best
///                         published one is installed
///        "local"          the highest installed version, never the network
///        nil              the project's pin (./modules/module.json) in a
///                         source run; otherwise in a compiled program the
///                         highest installed version, else the latest is
///                         installed; in a source run with no pin, "latest"
///        "latest"         the registry's latest (asked at most once a day),
///                         installed if missing; offline, the highest installed
/// Returns the folder, or empty with `why` set.
static std::filesystem::path resolve_module(const std::string& name, const std::string& asked, bool compiled, std::string* why) {
    if (auto bundled = LefFile::bundled_modules.find(name); bundled != LefFile::bundled_modules.end()) {
        std::error_code ec;
        if (!bundled->second.empty() && std::filesystem::is_directory(bundled->second, ec)) return bundled->second;
        *why = "it is bundled, but could not be unpacked next to the app (see the warning at start-up)";
        return {};
    }
    if (!compiled) {
        std::error_code ec;
        for (const auto& root : modules_dir::source_roots())
            if (std::filesystem::is_directory(root / name, ec)) return root / name;
    }
    std::string request = asked;
    if (request.empty() && !compiled) request = project_pin(name);
    if (request.empty()) request = compiled ? "installed-or-latest" : "latest";

    if (request == "local") {
        auto dir = modules_dir::best_installed(name, "*");
        if (dir.empty()) *why = "no version is installed in " + (modules_dir::store() / name).string() + " (\"local\" never downloads)";
        return dir;
    }
    if (request == "installed-or-latest") {
        auto dir = modules_dir::best_installed(name, "*");
        if (!dir.empty()) return dir;
        request = "latest";
    }
    auto dir = install_into_store(name, request, std::cout);
    if (!dir.empty()) return dir;
    if (request == "latest") {
        // offline: what is there
        dir = modules_dir::best_installed(name, "*");
        if (!dir.empty()) return dir;
    }
    *why = "no version matching \"" + request + "\" is installed in " + (modules_dir::store() / name).string()
        + " and it could not be installed";
    return {};
}

/// package.loaders entry: `require("name.sub.file")` for a module import()
/// resolved, from <its folder>\sub\file.lua (a module's own Lua files).
static int resolved_module_searcher(lua_State* L) {
    std::string wanted = luaL_checkstring(L, 1);
    auto dot = wanted.find('.');
    if (dot == std::string::npos) return 0;
    auto head = wanted.substr(0, dot);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::filesystem::path dir;
    {
        std::lock_guard lock(import_mutex);
        auto found = resolved_dirs.find(head);
        if (found == resolved_dirs.end()) return 0;
        dir = found->second;
    }
    auto rest = wanted.substr(dot + 1);
    // a dotted name only: no separators, drive or `..` out of the folder
    if (rest.empty() || rest.find_first_of("/\\:") != std::string::npos || rest.find("..") != std::string::npos
        || rest.front() == '.' || rest.back() == '.') return 0;
    std::replace(rest.begin(), rest.end(), '.', '\\');
    for (const auto& candidate : { dir / (rest + ".lua"), dir / rest / "init.lua" }) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        if (luaL_loadfile(L, candidate.string().c_str()) != 0) return lua_error(L);
        return 1;
    }
    lua_pushstring(L, ("\n\tno file '" + (dir / (rest + ".lua")).string() + "'").c_str());
    return 1;
}

static int import(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (!valid_import_name(name))
        return luaL_error(L, "import: \"%s\" is not a module name", name.c_str());
    const bool compiled = lua::env::is_compiled();
    std::string version;
    if (lua_isstring(L, 2)) version = lua_tostring(L, 2);
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
    std::filesystem::path dir;
    std::string why;
    {
        std::lock_guard lock(import_mutex);
        auto known = resolved_dirs.find(modulename);
        // One folder per module per process: a second import of a module that
        // already resolved (another of its files, or the same name again) uses it.
        if (known != resolved_dirs.end()) {
            dir = known->second;
            // A store version folder that does not satisfy what is asked now:
            // say so (a process holds one version of a module).
            auto folder = dir.filename().string();
            if (!version.empty() && version != "local" && version != "latest" && modules_dir::is_version(folder)
                && !modules_dir::satisfies(folder, version)) {
                return luaL_error(L, "import: module \"%s\" %s is already in use here; \"%s\" was asked for",
                    modulename.c_str(), folder.c_str(), version.c_str());
            }
        } else {
            dir = resolve_module(modulename, version, compiled, &why);
            if (!dir.empty()) resolved_dirs[modulename] = dir;
        }
    }
    if (dir.empty())
        return luaL_error(L, "import: module \"%s\": %s", modulename.c_str(), why.c_str());
    std::error_code ec;
    auto file_stem = name_after_dot;
    std::replace(file_stem.begin(), file_stem.end(), '.', '\\');
    bool isDllInclude = std::filesystem::exists(dir / (file_stem + ".dll"), ec);
    std::filesystem::path lua_file;
    if (!isDllInclude) {
        lua_file = dir / (file_stem + ".lua");
        if (!std::filesystem::exists(lua_file, ec)) {
            if (!bHasDot) return 0; // a module with nothing to load (installed for its files)
            return luaL_error(L, "import: module \"%s\" has no %s(.dll|.lua) in %s", modulename.c_str(),
                file_stem.c_str(), dir.string().c_str());
        }
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
        {
            lua_getglobal(L, "package");
            lua_getfield(L, -1, "cpath");
            const char* current = lua_tostring(L, -1);
            std::string cpath = current ? current : "";
            lua_pop(L, 1);
            cpath += ";" + dir.string() + "\\?.dll";
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
        AddDllDirectory(dir.wstring().c_str());
        const int top = lua_gettop(L);
        if (ll_loadfunc(L, (dir / (file_stem + ".dll")).string().c_str(), name.c_str(), 0)) {
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
        // A Lua file of the module: loaded from its folder and kept in
        // package.loaded under its dotted name, like require would.
        status = luaL_loadfile(L, lua_file.string().c_str());
        if (status == 0) {
            lua_pushstring(L, name.c_str());
            status = lua_pcall(L, 1, 1, 0);
            if (status == 0) {
                if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
                lua_getglobal(L, "package");
                lua_getfield(L, -1, "loaded");
                lua_pushvalue(L, -3);
                lua_setfield(L, -2, name.c_str());
                lua_pop(L, 2);
            }
        }
    }
    lua_pushvalue(L, saved_arg);
    lua_setglobal(L, "arg");
    if (status != 0) return lua_error(L); // the module's own error, unchanged
    return isDllInclude ? lua_pushandstore(L, name.c_str()) : 1;
}

static int import_open(lua_State* L) {
    lua_pushcfunction(L, import);
    lua_setglobal(L, "import");
    // require("name.sub") for a module import() resolved: its own Lua files.
    lua_getglobal(L, LUA_LOADLIBNAME);
    lua_getfield(L, -1, "loaders");
    // APPENDED, after the stock searchers: programs index package.loaders by
    // position (loaders[2] is Lua's path searcher), so lxe never reorders it.
    // It only answers names whose head import() resolved.
    lua_pushcfunction(L, resolved_module_searcher);
    lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
    lua_pop(L, 2);
    return 0;
}

#endif //LUAXE_IMPORT_H
