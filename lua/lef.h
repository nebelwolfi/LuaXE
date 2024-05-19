//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_LEF_H
#define LUAXE_LEF_H

struct LefFile {
#pragma pack(push, 1)
    struct ArgHeader {
        struct Arg {
            unsigned short length = 0;
            char data[1]{};
        };
        unsigned short numArgs = 0;
        Arg args[1]{};
    };
    struct FileHeader {
        struct File {
            unsigned long long len = 0;
            unsigned long nameLen = 0;
            char data[1]{};
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

    struct File { std::string name; std::string data; };
    std::vector<File> files;
    std::vector<std::string> args;

    static std::optional<LefFile> load_from_file(const std::string& path);
    static std::optional<LefFile> load_from_memory(const std::string& data);
    static void store_as_lef(const std::string& outfile, const std::string& source, const std::string& main, const std::vector<std::string>& args, bool strip, bool verbose);

    static std::vector<LefFile> loaded;
};

#endif //LUAXE_LEF_H
