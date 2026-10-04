//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_RUN_H
#define LUAXE_RUN_H

#include "src/lua/state.h"
#include "src/lua/lef.h"
#include "src/lua/env.h"
#include "src/lua/modules_dir.h"
#include <luaxe/bind.h>

/// Case-insensitive extension test ("x.LEF" is a .lef).
static bool has_extension(const std::string& path, const char* extension) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == extension;
}

/// A file `lxe run` can execute: a .lua or a .lef.
static bool is_script_path(const std::string& path) {
    return has_extension(path, ".lua") || has_extension(path, ".lef");
}

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
        i++; // past "run": the program's arguments start after it
    } else {
        source = __argv[++i];
        if (!is_script_path(source)) {
            out << "Invalid source provided, try \"luaxe help run\"." << std::endl;
            return;
        }
        i++;
    }
    // A .lef runs exactly like the payload of a compiled .exe (main.cpp): its
    // bundled DLLs (-b, lua51.dll included) are unpacked before the runtime is
    // bound and the state exists, and it runs as compiled, so import() and the
    // app resolve the bundled modules instead of looking for them on disk.
    bool is_lef = has_extension(source, ".lef");
    if (!is_lef) {
        // A source run also finds flat modules\ next to the script it runs (a
        // checkout started from another folder), see modules_dir::source_roots.
        std::error_code ec;
        auto script = std::filesystem::weakly_canonical(std::filesystem::absolute(source, ec), ec);
        if (!ec) modules_dir::set_script_dir(script.parent_path());
    }
    if (is_lef) {
        std::ifstream input(source, std::ios::binary);
        if (!input.is_open()) {
            std::cerr << "Error: Could not open file " << source << std::endl;
            std::exit(1);
        }
        // Through the file system (weakly_canonical), so the spelling - case
        // included - is the file's own whichever way it was typed; always absolute.
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(source, ec), ec);
        if (ec) canonical = std::filesystem::absolute(source, ec).lexically_normal();
        lua::env::lef_path = canonical.string();
        // The app's own folder: what it bundles unpacks next to it.
        modules_dir::set_app_dir(canonical.parent_path());
        if (!LefFile::extract_bundled_dlls(std::string(std::istreambuf_iterator<char>(input), {}))) {
            std::cerr << "Error: " << source << " is not a valid .lef file" << std::endl;
            std::exit(1);
        }
    }
    // A script that does not load, or raises, fails the process (exit 1): `lxe
    // <file>` is what the .lef association and scripts call.
    bool failed = false;
    load_lua_state_and_run([&](lua_State* L) {
        lua_createtable(L, __argc - i, 0);
        for (int j = i; j < __argc; j++) {
            lua_pushstring(L, __argv[j]);
            lua_rawseti(L, -2, j - i + 1);
        }
        lua_setglobal(L, "arg");
        if (!is_lef) {
            load_lua_file(L, source);
        } else {
            load_lef_file(L, source, i);
        }
        if (lua::pcall(L, 0, 0) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            std::cerr << "Error: " << (message ? message : "(error object is not a string)") << std::endl;
            lua_pop(L, 1);
            failed = true;
        }
    }, is_lef);
    if (failed) std::exit(1);
}

#endif //LUAXE_RUN_H
