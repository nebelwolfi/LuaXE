//
// Created by Nebelwolfi on 08/05/2024.
//
#include "src/pch.h"
#include "lef.h"
#include "lua_runtime.h"
#include "modules_dir.h"
#include "src/json.hpp"

namespace {

/// Case-insensitive equality, the comparison the payload loader has always used.
bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x))
                   == std::tolower(static_cast<unsigned char>(y));
           });
}

// TB-399 item 1: the loaded payloads, as an immutable snapshot of shared owners.
// A writer replaces the whole thing; a reader takes its own shared_ptr and keeps
// the payload alive for as long as it reads it, so clear()/register() can never
// pull the bytes out from under it.
//
// IMMORTAL on purpose (allocated once, never destroyed): a worker thread that
// thread's __gc detached is still running when main() returns, and the CRT's
// static destructors would otherwise destroy this mutex and list under it -
// measured as a 0xC0000409 fast-fail at exit (std::mutex used after
// destruction) in tests\tb399_lef_loaded.ps1.
struct LoadedState {
    std::mutex mutex;
    std::shared_ptr<const std::vector<std::shared_ptr<const LefFile>>> snapshot =
        std::make_shared<const std::vector<std::shared_ptr<const LefFile>>>();
};
LoadedState& loaded_state() {
    static LoadedState* state = new LoadedState(); // never deleted, see above
    return *state;
}

} // namespace

void LefFile::register_loaded(const LefFile& file) {
    auto owner = std::make_shared<const LefFile>(file);
    auto& state = loaded_state();
    std::lock_guard lock(state.mutex);
    auto next = std::make_shared<std::vector<std::shared_ptr<const LefFile>>>(*state.snapshot);
    next->push_back(owner);
    state.snapshot = std::move(next);
}

void LefFile::clear_loaded() {
    auto& state = loaded_state();
    std::shared_ptr<const std::vector<std::shared_ptr<const LefFile>>> dropped;
    {
        std::lock_guard lock(state.mutex);
        dropped = std::move(state.snapshot);
        state.snapshot = std::make_shared<const std::vector<std::shared_ptr<const LefFile>>>();
    }
    // `dropped` is released HERE, outside the lock: it frees only what no reader
    // still holds a reference to.
}

std::shared_ptr<const LefFile::File> LefFile::find_chunk(const std::string& name) {
    std::shared_ptr<const std::vector<std::shared_ptr<const LefFile>>> snapshot;
    {
        auto& state = loaded_state();
        std::lock_guard lock(state.mutex);
        snapshot = state.snapshot;
    }
    // The lock is released here: the snapshot is immutable and refcounted, so the
    // walk below is safe even while the main thread clears and refills the vector.
    for (const auto& lef : *snapshot) {
        for (const auto& file : lef->files) {
            if (file.type != LefFile::LUA_BYTECODE) continue; // a bundled DLL is not a Lua chunk
            if (iequals(file.name, name) || iequals(file.name, "modules." + name)) {
                // Aliasing: the File's owner (the whole LefFile) stays alive too.
                return std::shared_ptr<const File>(lef, &file);
            }
        }
    }
    return nullptr;
}

std::map<std::string, std::filesystem::path> LefFile::bundled_modules = {};

std::optional<LefFile> LefFile::load_from_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << path << std::endl;
        return std::nullopt;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_from_memory(std::string(data.begin(), data.end()));
}

std::optional<LefFile> LefFile::load_from_memory(const std::string &data) {
    auto fail = [](const char* why) -> std::optional<LefFile> {
        std::cerr << "Error: " << why << std::endl;
        return std::nullopt;
    };
    if (data.size() < sizeof(Header)) return fail("Invalid LEF file size");

    // Every field is copied out of the buffer and every length is checked
    // against what is left of it: a truncated or crafted .lef must be refused,
    // never read past its end.
    Header header;
    std::memcpy(&header, data.data(), sizeof(Header));
    if (header.version != VERSION_V1 && header.version != VERSION_V2) return fail("Invalid LEF file version");
    bool isV2 = header.version == VERSION_V2;
    if (header.fileHeader.numFiles == 0) return fail("No files in LEF file");

    size_t pos = sizeof(Header);
    auto fits = [&](unsigned long long n) { return pos <= data.size() && n <= data.size() - pos; };
    LefFile lefFile;

    for (auto i = 0; i < header.argHeader.numArgs; i++) {
        decltype(ArgHeader::Arg::length) length = 0;
        if (!fits(sizeof(length))) return fail("Truncated LEF argument");
        std::memcpy(&length, data.data() + pos, sizeof(length));
        pos += sizeof(ArgHeader::Arg);
        if (length == 0) return fail("Invalid argument length");
        if (!fits(length)) return fail("Truncated LEF argument");
        lefFile.args.emplace_back(data.data() + pos, length);
        pos += length;
    }

    if (header.fileHeader.firstFile > data.size()) return fail("Invalid LEF file offset");
    pos = (size_t)header.fileHeader.firstFile;
    for (auto i = 0; i < header.fileHeader.numFiles; i++) {
        unsigned char type = LUA_BYTECODE;
        decltype(FileHeader::File::len) len = 0;
        decltype(FileHeader::File::nameLen) nameLen = 0;
        if (isV2) {
            if (!fits(sizeof(FileHeader::FileV2))) return fail("Truncated LEF file entry");
            std::memcpy(&type, data.data() + pos + offsetof(FileHeader::FileV2, type), sizeof(type));
            std::memcpy(&len, data.data() + pos + offsetof(FileHeader::FileV2, len), sizeof(len));
            std::memcpy(&nameLen, data.data() + pos + offsetof(FileHeader::FileV2, nameLen), sizeof(nameLen));
            pos += sizeof(FileHeader::FileV2);
        } else {
            if (!fits(sizeof(FileHeader::File))) return fail("Truncated LEF file entry");
            std::memcpy(&len, data.data() + pos + offsetof(FileHeader::File, len), sizeof(len));
            std::memcpy(&nameLen, data.data() + pos + offsetof(FileHeader::File, nameLen), sizeof(nameLen));
            pos += sizeof(FileHeader::File);
        }
        if (len == 0 || nameLen == 0) return fail("Invalid file length");
        if (type != LUA_BYTECODE && type != DLL_BINARY) return fail("Invalid LEF file type");
        if (!fits(nameLen)) return fail("Truncated LEF file name");
        std::string name(data.data() + pos, nameLen);
        pos += nameLen;
        if (!fits(len)) return fail("Truncated LEF file data");
        lefFile.files.emplace_back(File{
            .name = std::move(name),
            .data = std::string(data.data() + pos, (size_t)len),
            .type = static_cast<FileType>(type)
        });
        pos += (size_t)len;
    }

    // The copy is deliberate: this function also RETURNS the payload it just
    // registered, and callers load their chunk out of that return value
    // (load_lua_memory). Moving it in here returned a moved-from LefFile and
    // faulted at the next luaL_loadbuffer (state.cpp).
    LefFile::register_loaded(lefFile);

    return lefFile;
}

namespace {

/// A bundled DLL's archive name must stay inside the modules directory:
/// "modules/...", relative, no drive or root, no `..` component. A crafted name
/// could otherwise write anywhere before a line of the payload runs.
bool safe_bundled_name(const std::string& name) {
    std::filesystem::path p(name);
    if (!name.starts_with("modules/") || p.has_root_name() || p.has_root_directory()) return false;
    // Per component too: a root name is only recognised in the FIRST one, so
    // "modules/C:x.dll" would otherwise pass and append as a drive-relative path.
    for (const auto& part : p) {
        if (part == ".." || part == "." || part.has_root_name() || part.string().find(':') != std::string::npos) return false;
    }
    return name.size() > std::string("modules/").size();
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

/// The version a bundled module is recorded under: its own module.json's
/// "version", else the exact version the project pins in modules/module.json,
/// else "local-<first 8 hex of the sha256 of <name>.dll>" - a build that was
/// never published still gets a stable folder of its own per build.
std::string bundle_version(const std::filesystem::path& modules_path, const std::string& name, const std::filesystem::path& dir) {
    auto read = [](const std::filesystem::path& file) -> nlohmann::json {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) return nlohmann::json::object();
        try { return nlohmann::json::parse(std::ifstream(file)); } catch (const std::exception&) {}
        return nlohmann::json::object();
    };
    auto own = read(dir / "module.json");
    if (own.is_object() && own.contains("version") && own["version"].is_string()
        && modules_dir::is_version(own["version"].get<std::string>()))
        return own["version"].get<std::string>();
    auto project = read(modules_path / "module.json");
    if (project.is_object() && project.contains("dependencies") && project["dependencies"].is_object()) {
        auto& deps = project["dependencies"];
        for (auto it = deps.begin(); it != deps.end(); ++it) {
            if (lowercase(it.key()) == lowercase(name) && it->is_string()
                && modules_dir::is_version(it->get<std::string>()))
                return it->get<std::string>();
        }
    }
    auto hash = lua_runtime::sha256(dir / (name + ".dll"));
    return "local-" + (hash.size() >= 8 ? hash.substr(0, 8) : std::string("00000000"));
}

} // namespace

bool LefFile::extract_bundled_dlls(const std::string& lef_data) {
    auto preview = LefFile::load_from_memory(lef_data);
    // load_from_memory registers what it parsed; this was only a preview.
    LefFile::clear_loaded();
    if (!preview) return false;

    // Every failure below is reported, never thrown out of main; the payload
    // still runs.
    const auto exe_dir = modules_dir::exe_dir();
    const auto app = modules_dir::app_dir().empty() ? exe_dir : modules_dir::app_dir();
    const auto home = app / "modules";
    for (const auto& f : preview->files) {
        if (f.type != LefFile::DLL_BINARY) continue;
        if (!safe_bundled_name(f.name)) {
            std::cerr << "Warning: refusing the bundled file \"" << f.name << "\": it would unpack outside "
                      << home.string() << std::endl;
            continue;
        }
        auto relative = std::filesystem::path(f.name).lexically_relative("modules");
        std::vector<std::filesystem::path> parts(relative.begin(), relative.end());
        auto target = home / relative;
        if (parts.size() == 1) {
            // modules/lua51.dll: the runtime. Not written when a verified one is
            // already where lua_runtime::ensure looks first (beside the exe, or
            // lxe's own ~\.lxe\bin). One that would be REFUSED is replaced, so a
            // stale or broken copy cannot keep a payload that carries a good one
            // from starting.
            if (lowercase(relative.filename().string()) == "lua51.dll") {
                if (lua_runtime::verify(exe_dir / "lua51.dll", nullptr)
                    || lua_runtime::verify(modules_dir::bin() / "lua51.dll", nullptr)) {
                    continue;
                }
                // The runtime is lxe's, not the app's: it goes to ~\.lxe\bin,
                // where every lxe and app looks for it (it is hash-verified
                // before it is loaded). Only when that is not writable, next to
                // the app.
                std::string error;
                if (lua_runtime::known_good_bytes(f.data)
                    && modules_dir::write_atomically(modules_dir::bin() / "lua51.dll", f.data, &error)) {
                    continue;
                }
                if (lua_runtime::verify(target, nullptr)) continue;
            }
        } else {
            // modules/<name>/<version>/<file> (an older payload has no version:
            // modules/<name>/<file>, unpacked to <name>\bundled\<file>). A bundled
            // module wins over an installed one of the same name, and is never
            // installed or updated by import() - even when it could not be
            // written below (an older copy is better than a stranger's module of
            // the same name).
            auto name = lowercase(parts[0].string());
            std::filesystem::path folder;
            if (parts.size() >= 3) {
                folder = home / parts[0] / parts[1];
            } else {
                folder = home / parts[0] / "bundled";
                target = folder / parts[1];
            }
            // An unpack that fails leaves an EMPTY entry: import() then reports
            // it, instead of loading whatever is on disk in that folder.
            std::string error;
            if (!modules_dir::write_atomically(target, f.data, &error)) {
                std::cerr << "Warning: could not unpack the bundled " << f.name << ": " << error << std::endl;
                LefFile::bundled_modules[name] = std::filesystem::path();
            } else if (!LefFile::bundled_modules.contains(name)) {
                LefFile::bundled_modules[name] = folder;
            }
            continue;
        }
        std::string error;
        if (!modules_dir::write_atomically(target, f.data, &error)) {
            std::cerr << "Warning: could not unpack the bundled " << f.name << ": " << error << std::endl;
        }
    }
    return true;
}

bool LefFile::store_as_lef(const std::string &outfile, const std::string& source, const std::string& main, const std::vector<std::string> &args, const std::vector<std::string> &bundle_modules, bool strip, bool verbose) {
    // TB-195: `lxe compile` creates states of its own to compile every .lua
    // source to bytecode (luaL_newstate + lua_dump below), so the runtime has to
    // be bound for EVERY output - a .lef archive as much as an .exe. The output
    // is only opened once every source compiled, so a missing runtime or a broken
    // source leaves no truncated file behind.
    {
        std::string runtime_error;
        if (!lua_runtime::ensure({}, &runtime_error)) {
            std::cerr << "Error: no usable Lua runtime, cannot compile." << std::endl
                      << "  " << runtime_error << std::endl;
            return false;
        }
    }
    auto out_ext = std::filesystem::path(outfile).extension().string();
    std::transform(out_ext.begin(), out_ext.end(), out_ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    bool is_exe = out_ext == ".exe";
    std::vector<File> files;
    int failed = 0;
    {
        auto path_fmt = +[](std::string path, bool lua) {
            while (path[0] == '.' || path[0] == '\\') path = path.substr(1);
            for (auto& c : path) {
                if (c == '\\') c = '/';
            }
            if (lua) {
                if (path.find(".lua") != std::string::npos)
                    path = path.substr(0, path.find(".lua"));
                while (path.find('/') != std::string::npos)
                    path = path.replace(path.find('/'), 1, ".");
                while (path.find('\\') != std::string::npos)
                    path = path.replace(path.find('\\'), 1, ".");
            }
            return path;
        };
        auto add_lua = [&](const std::filesystem::path& p, std::string n) {
            if (verbose) std::cout << "[+]" << n << " as ";
            n = path_fmt(n, true);
            if (verbose) std::cout << n << std::endl;
            std::ifstream input(p, std::ios::binary);
            if (!input.is_open()) {
                std::cerr << "Error: failed to open file: " << p << std::endl;
                failed++;
                return;
            }
            std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
            input.close();

            auto L = luaL_newstate();
            if (!L) {
                std::cerr << "Error: could not create a Lua state to compile " << p << std::endl;
                failed++;
                return;
            }

            if (luaL_loadbuffer(L, buffer.data(), buffer.size(), ("=" + n).c_str()))
            {
                const char* message = lua_tostring(L, -1);
                std::cerr << "Error: failed to load lua file: " << p << "\t" << (message ? message : "(no message)") << std::endl;
                lua_close(L);
                failed++;
                return;
            }

            luaL_Buffer buf;
            luaL_buffinit(L, &buf);
            if ((strip ? lua_dump_strip : lua_dump)(L, (lua_Writer)+[](lua_State* L, signed char* str, size_t len, struct luaL_Buffer* buf) -> int {
                luaL_addlstring(buf, (const char*)str, len);
                return 0;
            }, &buf))
            {
                // lua_dump pushes no error message; the writer above never fails.
                std::cerr << "Error: failed to dump lua file: " << p << std::endl;
                lua_close(L);
                failed++;
                return;
            }
            luaL_pushresult(&buf);
            size_t len = 0;
            files.push_back(File{
                .name = n,
                .data = { lua_tolstring(L, -1, &len), len }
            });

            lua_close(L);
        };
        std::function<void(std::filesystem::path, std::string)> add = [&](const std::filesystem::path& path, const std::string& n) -> void {
            if (std::filesystem::is_directory(path)) {
                if (path.stem().string()[0] != '$' && path.stem().string()[0] != '.')
                    for (auto& p : std::filesystem::directory_iterator(path)) {
                        add(p, p.path().string());
                    }
            } else if (path.extension() == ".lua") {
                add_lua(path, path.string());
            } else if (!path.string().starts_with(".\\modules\\") && path.extension() != ".lef" && path.extension() != ".exe") {
                // skip non-lua files
            }
        };
        std::filesystem::path path(source);
        if (std::filesystem::is_directory(path)) {
            auto cur_path = std::filesystem::current_path();
            std::filesystem::current_path(path);
            for (auto& p : std::filesystem::directory_iterator(".")) {
                add(p, p.path().string());
            }
            std::filesystem::current_path(cur_path);
        } else {
            add(path, path.string());
        }
        if (failed) {
            std::cerr << "Error: " << failed << " source file(s) failed to compile, " << outfile << " was not written." << std::endl;
            return false;
        }
        if (files.empty()) {
            std::cerr << "Error: no .lua sources found in " << source << ", " << outfile << " was not written." << std::endl;
            return false;
        }
        if (auto mainFile = std::find_if(files.begin(), files.end(), [&](const File& f) { return f.name == path_fmt(main, true); }); mainFile != files.end()) {
            std::rotate(files.begin(), mainFile, mainFile + 1);
        } else if (verbose)  {
            std::cout << "Main file " << main << " not found, using " << files[0].name << " as main" << std::endl;
        }
    }

    // TB-195: an .exe ALWAYS carries the runtime it will load at start-up, even
    // without -b: lxe.exe links no LuaJIT, so a compiled app that did not embed
    // modules/lua51.dll would have nothing to run on and could not even reach a
    // download on a locked-down machine. .lef output keeps the old rule (bundle
    // only what -b asked for).
    bool hasDlls = !bundle_modules.empty() || is_exe;
    if (hasDlls) {
        auto modules_path = std::filesystem::path("modules");
        // Always bundle lua51.dll when bundling any DLL module: the exe loads it
        // at run time (TB-195) and every module resolves `lua51.dll` to it.
        // Only a KNOWN-GOOD runtime is embedded (lua_runtime::known_good: one
        // of the pinned SHA-256s; LUAXE_ALLOW_UNVERIFIED_LUA51 lets this lxe RUN
        // on another build, never ship one - the artifact runs elsewhere, where
        // it would be refused): ./modules/lua51.dll when it is one, otherwise
        // the runtime this lxe is running on when THAT is one.
        std::filesystem::path lua51_path = modules_path / "lua51.dll";
        std::error_code ec;
        if (!std::filesystem::exists(lua51_path, ec)) {
            lua51_path = lua_runtime::path();
            if (verbose) std::cout << "[i] no " << (modules_path / "lua51.dll").string() << ", embedding " << lua51_path.string() << std::endl;
        } else if (!lua_runtime::known_good(lua51_path)) {
            std::cerr << "Warning: not embedding " << lua51_path.string() << ": sha256 " << lua_runtime::sha256(lua51_path)
                      << " is not a known-good LuaXE runtime; embedding the runtime this lxe runs on ("
                      << lua_runtime::path() << ") instead" << std::endl;
            lua51_path = lua_runtime::path();
        }
        if (!lua_runtime::known_good(lua51_path)) {
            std::cerr << "Error: the Lua runtime to embed (" << lua51_path.string() << ", sha256 "
                      << lua_runtime::sha256(lua51_path) << ") is not a known-good LuaXE runtime, " << outfile
                      << " was not written. Put a known-good lua51.dll in .\\modules (see kKnownGood in src/lua/lua_runtime.cpp)." << std::endl;
            return false;
        }
        {
            std::ifstream input(lua51_path, std::ios::binary);
            std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
            input.close();
            if (buffer.empty()) {
                std::cerr << "Error: could not read the Lua runtime " << lua51_path.string() << ", " << outfile << " was not written." << std::endl;
                return false;
            }
            files.push_back(File{
                .name = "modules/lua51.dll",
                .data = std::string(buffer.begin(), buffer.end()),
                .type = DLL_BINARY
            });
            if (verbose) std::cout << "[+dll] modules/lua51.dll (" << buffer.size() << " bytes, from " << lua51_path.string() << ")" << std::endl;
        }

        for (const auto& requested : bundle_modules) {
            // -b name or -b name@version. The files come from the project's
            // ./modules/<name>/ (any version given is the one recorded), or else
            // from lxe's store (~\.lxe\modules\<name>\<best match>\).
            std::string mod = requested, version;
            if (auto at = requested.find('@'); at != std::string::npos) {
                mod = requested.substr(0, at);
                version = requested.substr(at + 1);
            }
            // An explicit version is the store's: checkout bytes are never
            // labelled with a published version (only an unversioned -b name
            // takes ./modules/<name>/).
            auto mod_dir = modules_path / mod;
            if (!version.empty() || !std::filesystem::is_directory(mod_dir, ec)) {
                auto stored = modules_dir::best_installed(lowercase(mod), version.empty() ? "*" : version);
                mod_dir = stored;
                if (!stored.empty()) version = stored.filename().string();
                else if (!version.empty()) {
                    std::cerr << "Error: -b " << requested << ": no installed version matches (lxe install "
                              << requested << " first), " << outfile << " was not written." << std::endl;
                    return false;
                }
            }
            if (!std::filesystem::is_directory(mod_dir, ec)) {
                // Not a warning: a program that bundles a module never installs
                // it, so a missing one would be fetched from the registry at run
                // time - possibly a different module that has the same name.
                std::cerr << "Error: module directory " << mod_dir << " not found (-b " << requested
                          << ", nor in " << (modules_dir::store() / mod).string() << "), "
                          << outfile << " was not written." << std::endl;
                return false;
            }
            if (version.empty()) version = bundle_version(modules_path, mod, mod_dir);
            if (!modules_dir::is_version(version) && !version.starts_with("local-")) {
                std::cerr << "Error: -b " << requested << ": \"" << version << "\" is not a version, "
                          << outfile << " was not written." << std::endl;
                return false;
            }
            std::filesystem::recursive_directory_iterator walk(mod_dir, ec);
            if (ec) {
                std::cerr << "Error: cannot read " << mod_dir << " (" << ec.message() << "), " << outfile << " was not written." << std::endl;
                return false;
            }
            for (auto& entry : walk) {
                if (!entry.is_regular_file()) continue;
                // The module's whole folder: its DLLs and its own Lua files
                // (require("name.sub") loads them from the unpacked folder).
                auto ext = lowercase(entry.path().extension().string());
                if (ext != ".dll" && ext != ".lua") continue;
                auto rel_path = "modules/" + lowercase(mod) + "/" + version + "/"
                    + std::filesystem::relative(entry.path(), mod_dir).string();
                for (auto& c : rel_path) if (c == '\\') c = '/';
                std::ifstream input(entry.path(), std::ios::binary);
                std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
                input.close();
                files.push_back(File{
                    .name = rel_path,
                    .data = std::string(buffer.begin(), buffer.end()),
                    .type = DLL_BINARY
                });
                if (verbose) std::cout << "[+dll] " << rel_path << " (" << buffer.size() << " bytes)" << std::endl;
            }
        }
    }

    std::ofstream file(outfile, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << outfile << std::endl;
        return false;
    }
    if (is_exe) {
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(NULL, exe_path, MAX_PATH);
        std::ifstream input(exe_path, std::ios::binary);
        std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
        input.close();
        file.write(buffer.data(), buffer.size());
    }

    uint64_t totalArgSize = 0;
    for (const auto& arg : args)
        totalArgSize += sizeof(ArgHeader::Arg) + arg.size();

    decltype(Header::version) version = hasDlls ? VERSION_V2 : VERSION_V1;
    file.write(reinterpret_cast<const char *>(&version), sizeof(version));

    auto numFiles = static_cast<unsigned short>(files.size());
    file.write(reinterpret_cast<const char *>(&numFiles), sizeof(numFiles));

    decltype(FileHeader::firstFile) firstFile = sizeof(Header) + totalArgSize;
    file.write(reinterpret_cast<const char *>(&firstFile), sizeof(firstFile));

    auto numArgs = static_cast<unsigned short>(args.size());
    file.write(reinterpret_cast<const char *>(&numArgs), sizeof(numArgs));

    for (const auto& arg : args) {
        decltype(ArgHeader::Arg::length) length = arg.size();
        file.write(reinterpret_cast<const char*>(&length), sizeof(length));
        file.write(arg.data(), arg.size());
    }

    for (const auto& lf : files) {
        if (hasDlls) {
            unsigned char type = static_cast<unsigned char>(lf.type);
            file.write(reinterpret_cast<const char *>(&type), sizeof(type));
        }
        decltype(FileHeader::File::len) len = lf.data.size();
        decltype(FileHeader::File::nameLen) nameLen = lf.name.size();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(reinterpret_cast<const char *>(&nameLen), sizeof(nameLen));
        file.write(lf.name.data(), lf.name.size());
        file.write(lf.data.data(), lf.data.size());
    }
    file.close();
    if (!file) {
        std::cerr << "Error: failed to write " << outfile << std::endl;
        return false;
    }
    return true;
}