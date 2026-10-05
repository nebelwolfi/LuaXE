//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_IMPORT_H
#define LUAXE_IMPORT_H

#include "../https/connection/API.h"
#include "../https/misc/json.hpp"
#include "../https/misc/md5.h"
#include "../commands/install.h"
#include <chrono>
#include <future>
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
// lock (its luaopen may import more). The NETWORK stays outside it too
// (TB-399 item 3): resolve_module -> install_into_store asks the registry and
// downloads files, and import() used to hold import_mutex across all of it -
// every other import, and every require("name.sub") through
// resolved_module_searcher, blocked for the whole download. Now the first
// importer of a name registers an in-flight install, releases the lock, and
// does the network; concurrent importers of the same name wait on its future
// outside the lock. Publishing (resolved_dirs) still happens under the lock.
//
// NOTE (review of 382024e): like publish_mutex in install.h, these are
// static-in-header (one instance per including TU). Only state.cpp includes
// this header today, so there is exactly one - a second including TU would
// silently split lock AND map. If that ever happens, make both inline (C++17).
static std::mutex import_mutex;

// The folder each module name resolved to in this process, so a later
// `require("name.sub")` (a module's own Lua files) finds name\sub.lua there.
//
// An install IN FLIGHT is a promise, not a folder: while the first importer of
// a module does the network (resolve_module does registry I/O and downloads -
// OUTSIDE the lock since TB-399 item 3), later importers of the same module
// wait on its future, also outside the lock. The installer publishes the folder
// under the lock; the map itself is only touched under the lock.
static std::map<std::string, std::shared_future<std::filesystem::path>> resolved_dirs;

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
        // An install in flight is not ours to wait on here (this searcher runs
        // inside require, which must not block on another state's network):
        // not published yet means not found yet. So a require("name.sub")
        // that races import("name") falls through to the next searcher instead
        // of failing OR hanging - usually "not found" until the install lands,
        // then found on retry. (Review of 382024e: documented, not blocked.)
        if (found->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 0;
        try {
            dir = found->second.get();
        } catch (const std::exception&) {
            return 0; // a failed install publishes nothing: behave as not found
        } catch (...) {
            return 0;
        }
        // Ready but EMPTY: the install failed between set_value and erase (the
        // entry is erased under the same lock right after). An empty dir would
        // resolve against the cwd - never what this searcher means.
        if (dir.empty()) return 0;
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
    // Someone else's install of this module (wait on it below), or the promise
    // THIS call fulfils when its own install finishes. shared_future has no
    // set_value; the promise lives in this frame and the map holds only the
    // future, so publish/erase under the lock is what wakes the waiters.
    std::shared_future<std::filesystem::path> in_flight;
    std::shared_ptr<std::promise<std::filesystem::path>> installing;
    // What THIS import asked for: a waiter re-checks it against the folder the
    // installer publishes (one version per process - review of 382024e: the
    // first draft skipped the check on the waiter path and silently took the
    // other import's version).
    std::string asked = version;
    {
        std::lock_guard lock(import_mutex);
        auto known = resolved_dirs.find(modulename);
        // One folder per module per process: a second import of a module that
        // already resolved (another of its files, or the same name again) uses it.
        if (known != resolved_dirs.end()) {
            auto ready = known->second.wait_for(std::chrono::seconds(0));
            if (ready == std::future_status::ready) {
                // Guarded like the waiter path: a stored exception must become a
                // Lua error, never cross the C frame (fatal). Unreachable today
                // (the installer only set_values, never set_exception) - kept
                // symmetric so it stays unreachable safely.
                try {
                    dir = known->second.get();
                } catch (const std::exception& thrown) {
                    return luaL_error(L, "import: module \"%s\": resolving it failed (%s)",
                        modulename.c_str(), thrown.what());
                } catch (...) {
                    return luaL_error(L, "import: module \"%s\": resolving it failed",
                        modulename.c_str());
                }
                // Ready but EMPTY: the install failed between set_value and erase
                // (same lock, right after). Fall through to the empty-dir error
                // below - with why already set when THIS call installed, or the
                // waiter's message otherwise.
                if (dir.empty() && why.empty())
                    why = "another import's install failed (see its error above)";
            } else {
                in_flight = known->second; // install in flight: wait on it below
            }
        } else {
            // First importer: promise the folder, then resolve WITHOUT the lock
            // (resolve_module does registry I/O and downloads). Concurrent
            // importers of the same module find the promise and wait on it.
            installing = std::make_shared<std::promise<std::filesystem::path>>();
            in_flight = installing->get_future().share();
            resolved_dirs.emplace(modulename, in_flight);
        }
    }
    if (installing) {
        // Outside import_mutex: the network. install_into_store publishes the
        // version folder atomically (one MoveFileExW); two installers of one
        // module in two PROCESSES cannot both publish: the loser finds
        // best_installed and uses it.
        //
        // resolve_module/install_into_store catch their own registry/parse
        // errors (install.h), but a filesystem throw OUTSIDE them (bad_alloc,
        // a throwing path overload) must still wake the waiters: a promise
        // destroyed without a value makes every waiter's get() throw
        // broken_promise across the C++ frame (fatal) instead of erroring.
        try {
            dir = resolve_module(modulename, version, compiled, &why);
        } catch (const std::exception& thrown) {
            why = std::string("unexpected error while installing: ") + thrown.what();
            dir.clear();
        } catch (...) {
            why = "unexpected error while installing";
            dir.clear();
        }
        installing->set_value(dir); // wakes the waiters' copy of the future
        std::lock_guard lock(import_mutex);
        // Publish under the lock; a failure publishes nothing: erase, so the
        // next import retries instead of waiting on an empty promise forever.
        // (Only OUR key: nobody else replaces it - waiters only copy - so no
        // identity check beyond the key is needed.)
        auto it = resolved_dirs.find(modulename);
        if (it != resolved_dirs.end()) {
            if (!dir.empty())
                it->second = in_flight; // now ready: waiters' get() returns
            else
                resolved_dirs.erase(it);
        }
    } else if (in_flight.valid() && dir.empty()) {
        // Someone else's install: wait on it OUTSIDE the lock (bounded only by
        // the install itself; the installer never holds import_mutex meanwhile,
        // so this cannot deadlock with it). The future is always fulfilled
        // (empty on failure), so get() cannot throw broken_promise - but a
        // stored exception still could in theory: guard it into a Lua error.
        try {
            dir = in_flight.get();
        } catch (const std::exception& thrown) {
            return luaL_error(L, "import: module \"%s\": another import's install failed (%s)",
                modulename.c_str(), thrown.what());
        } catch (...) {
            return luaL_error(L, "import: module \"%s\": another import's install failed",
                modulename.c_str());
        }
        if (dir.empty())
            return luaL_error(L, "import: module \"%s\": %s", modulename.c_str(),
                "another import's install failed (see its error above)");
    }
    if (!dir.empty() && !asked.empty() && asked != "local" && asked != "latest") {
        // One version per process - on EVERY path, including a folder another
        // import's install just published (review of 382024e: the waiter path
        // skipped this and silently took the wrong version).
        //
        // Store folders only: bundled modules and flat source-roots resolve to
        // whatever was asked for by contract (import.h header docs), and stock
        // never checked them - only the second import of an installed module.
        // A store folder is <store>\<name>\<version>.
        auto folder = dir.filename().string();
        bool from_store = false;
        {
            std::error_code ec;
            auto store_name = modules_dir::store() / modulename;
            from_store = !dir.empty()
                && std::filesystem::equivalent(dir.parent_path(), store_name, ec) && !ec;
        }
        if (from_store && modules_dir::is_version(folder) && !modules_dir::satisfies(folder, asked)) {
            return luaL_error(L, "import: module \"%s\" %s is already in use here; \"%s\" was asked for",
                modulename.c_str(), folder.c_str(), asked.c_str());
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
