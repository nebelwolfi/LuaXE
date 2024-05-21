//
// Created by Nebelwolfi on 08/05/2024.
//
#include <pch.h>
#include "lef.h"

std::vector<LefFile> LefFile::loaded = {};

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
    if (data.size() < sizeof(Header)) {
        std::cerr << "Error: Invalid LEF file size" << std::endl;
        return std::nullopt;
    }

    const auto* header = reinterpret_cast<const Header*>(data.data());
    if (header->version != 0xd0d0) {
        std::cerr << "Error: Invalid LEF file version" << std::endl;
        return std::nullopt;
    }

    LefFile lefFile;

    if (header->fileHeader.numFiles == 0) {
        std::cerr << "Error: No files in LEF file" << std::endl;
        return std::nullopt;
    }

    const char* pos = data.data() + sizeof(Header);
    for (auto i = 0; i < header->argHeader.numArgs; i++) {
        const auto* arg = reinterpret_cast<const ArgHeader::Arg*>(pos);
        if (arg->length == 0) {
            std::cerr << "Error: Invalid argument length" << std::endl;
            return std::nullopt;
        }
        lefFile.args.emplace_back(arg->data, arg->length);
        pos += sizeof(ArgHeader::Arg) + arg->length;
    }

    //printf("first file: %d\n", header->fileHeader.firstFile);

    pos = data.data() + header->fileHeader.firstFile;
    for (auto i = 0; i < header->fileHeader.numFiles; i++) {
        const auto* file = reinterpret_cast<const FileHeader::File*>(pos);
        if (file->len == 0 || file->nameLen == 0) {
            std::cerr << "Error: Invalid file length" << std::endl;
            return std::nullopt;
        }
        lefFile.files.emplace_back(File{
            .name = std::string(file->data, file->nameLen),
            .data = std::string(file->data + file->nameLen, file->len)
        });
        //printf("file %d: %d %d | name: %x => %s\n", i, file->len, file->nameLen, file->data - data.data(), lefFile.files.back().name.c_str());
        //printf("namelen: %d | data: %x => %s\n", file->nameLen, file->data + file->nameLen - data.data(), file->data + file->nameLen);
        pos += sizeof(FileHeader::File) + file->len + file->nameLen;
    }

    //printf("Loaded %d files\n", lefFile.files.size());
    //printf("first file: %s\n", lefFile.files[0].data.c_str());

    LefFile::loaded.push_back(lefFile);

    return lefFile;
}

void LefFile::store_as_lef(const std::string &outfile, const std::string& source, const std::string& main, const std::vector<std::string> &args, bool strip, bool verbose) {
    std::ofstream file(outfile, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << outfile << std::endl;
        return;
    }
    if (outfile.find(".exe") != std::string::npos) {
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(NULL, exe_path, MAX_PATH);
        std::ifstream input(exe_path, std::ios::binary);
        std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
        input.close();
        file.write(buffer.data(), buffer.size());
    }
    std::vector<File> files;
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
                std::cout << "Failed to open file: " << p << std::endl;
                return;
            }
            std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
            input.close();

            auto L = luaL_newstate();

            if (luaL_loadbuffer(L, buffer.data(), buffer.size(), ("=" + n).c_str()))
            {
                std::cout << "Failed to load lua file: " << p << "\t" << lua_tostring(L, -1) << std::endl;
                return;
            }

            luaL_Buffer buf;
            luaL_buffinit(L, &buf);
            if ((strip ? lua_dump_strip : lua_dump)(L, (lua_Writer)+[](lua_State* L, signed char* str, size_t len, struct luaL_Buffer* buf) -> int {
                luaL_addlstring(buf, (const char*)str, len);
                return 0;
            }, &buf))
            {
                std::cout << "Failed to dump lua file: " << p << "\t" << lua_tostring(L, -1) << std::endl;
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
                // add_f(path, path.string()); // skip non-lua files
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
        if (auto mainFile = std::find_if(files.begin(), files.end(), [&](const File& f) { return f.name == path_fmt(main, true); }); mainFile != files.end()) {
            std::rotate(files.begin(), mainFile, mainFile + 1);
        } else if (verbose)  {
            std::cout << "Main file " << main << " not found, using " << files[0].name << " as main" << std::endl;
        }
    }
    uint64_t totalArgSize = 0;
    for (const auto& arg : args) {
        totalArgSize += sizeof(ArgHeader::Arg) + arg.size();
    }
    decltype(FileHeader::firstFile) firstFile = sizeof(Header) + totalArgSize;
    {
        decltype(Header::version) version = 0xd0d0;
        file.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }
    {
        auto numFiles = static_cast<unsigned short>(files.size());
        file.write(reinterpret_cast<const char *>(&numFiles), sizeof(numFiles));
        file.write(reinterpret_cast<const char *>(&firstFile), sizeof(firstFile));
    }
    {
        auto numArgs = static_cast<unsigned short>(args.size());
        file.write(reinterpret_cast<const char *>(&numArgs), sizeof(numArgs));
    }

    for (const auto& arg : args) {
        decltype(ArgHeader::Arg::length) length = arg.size();
        file.write(reinterpret_cast<const char*>(&length), sizeof(length));
        file.write(arg.data(), arg.size());
    }

    uint64_t dataOff = firstFile + sizeof(FileHeader::File) * files.size() + 1;
    for (const auto& lf : files) {
        decltype(FileHeader::File::len) len = lf.data.size();
        decltype(FileHeader::File::nameLen) nameLen = lf.name.size();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(reinterpret_cast<const char *>(&nameLen), sizeof(nameLen));
        file.write(lf.name.data(), lf.name.size());
        file.write(lf.data.data(), lf.data.size());
        dataOff += sizeof(FileHeader::File) + lf.data.size() + lf.name.size();
    }
}