//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LEF_H
#define LUAXE_LEF_H

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
    static void store_as_lef(const std::string& outfile, const std::string& source, const std::string& main, const std::vector<std::string>& args, const std::vector<std::string>& bundle_modules, bool strip, bool verbose);

    static std::vector<LefFile> loaded;
    static std::filesystem::path bundled_dll_dir;
};

#endif //LUAXE_LEF_H
