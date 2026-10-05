//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LEF_H
#define LUAXE_LEF_H

#include <optional>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct LefFile {
    static constexpr unsigned short VERSION_V1 = 0xd0d0;
    static constexpr unsigned short VERSION_V2 = 0xd0d1;

    enum FileType : unsigned char {
        LUA_BYTECODE = 0,
        DLL_BINARY = 1,
    };

#pragma pack(push, 1)
    struct ArgHeader {
        struct Arg {
            unsigned short length = 0;
            char data[0];
        };
        unsigned short numArgs = 0;
    };
    struct FileHeader {
        struct File {
            unsigned long long len = 0;
            unsigned long nameLen = 0;
            char data[0];
        };
        struct FileV2 {
            unsigned char type = 0;
            unsigned long long len = 0;
            unsigned long nameLen = 0;
            char data[0];
        };
        unsigned short numFiles = 0;
        unsigned long long firstFile = 0;
    };
    struct Header {
        unsigned short version;
        FileHeader fileHeader;
        ArgHeader argHeader;
    };
#pragma pack(pop)

    struct File { std::string name; std::string data; FileType type = LUA_BYTECODE; };
    std::vector<File> files;
    std::vector<std::string> args;

    static std::optional<LefFile> load_from_file(const std::string& path);
    static std::optional<LefFile> load_from_memory(const std::string& data);
    /// Compiles `source` into `outfile` (.lef, or an .exe when outfile ends in .exe).
    /// Returns false (after printing why) when nothing usable was written.
    static bool store_as_lef(const std::string& outfile, const std::string& source, const std::string& main, const std::vector<std::string>& args, const std::vector<std::string>& bundle_modules, bool strip, bool verbose);

    /// Unpacks the DLLs a payload bundles (-b) NEXT TO THE APP:
    /// <app dir>\modules\<name>\<version>\ (modules_dir::app_dir(): the .lef's
    /// folder, or the compiled exe's), so no other project loads them by
    /// accident, and records name -> folder in bundled_modules: import() loads
    /// exactly those copies and never installs or updates them. The bundled
    /// lua51.dll goes to <app dir>\modules\lua51.dll, and only when no verified
    /// runtime is beside the exe or in ~\.lxe\bin.
    /// Shared by a compiled .exe and `lxe run x.lef`. Call it BEFORE the first
    /// Lua state exists. Does nothing for a payload with no bundled DLLs.
    /// Returns false only when lef_data is not a readable LEF payload (a failed
    /// unpack is reported and the payload runs without that module).
    static bool extract_bundled_dlls(const std::string& lef_data);

    // TB-399 item 1: the payloads this process has loaded, as ONE immutable
    // snapshot behind a mutex - never a bare vector.
    //
    // It is written on the main thread (load_from_memory registers it, the restart
    // loop and extract_bundled_dlls clear it) and read from EVERY state, worker
    // threads included: new_state installs the package loader closure that walks
    // it, and a worker still running when its state is closed is DETACHED rather
    // than joined (modules/Thread's __gc), so such a reader keeps walking while
    // the main thread clears and refills the vector. With a bare vector that is a
    // use-after-free of every std::string under it - 5/5 ACCESS_VIOLATION in
    // tests/tb-399-item1-lef-loaded.ps1.
    //
    // register/clear replace the whole snapshot (copy-on-write), and a reader
    // holds a shared_ptr for as long as it touches a payload, so a clear can
    // never free bytes a reader is reading. Read through find_chunk, never the
    // vector.
    static void register_loaded(const LefFile& file);
    static void clear_loaded();
    /// The chunk `name` names (or "modules.<name>"), held so its bytes stay alive
    /// while the caller loads them, or nullptr when no loaded payload has it.
    static std::shared_ptr<const File> find_chunk(const std::string& name);

    /// Lowercase name -> folder of every module the running payload carries.
    static std::map<std::string, std::filesystem::path> bundled_modules;
};

#endif //LUAXE_LEF_H
