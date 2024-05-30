#include <pch.h>
#include <version.h>
#include <commands/register.h>
#include <commands/update.h>
#include <commands/compile.h>
#include <commands/run.h>
#include <commands/install.h>
#include <lua/state.h>
#include <luaxe/bind.h>
#include "cli.h"

int main(int argc, char** argv) {
    AddDllDirectory((std::filesystem::current_path() / "modules").wstring().c_str());
    // check for already compiled exe
    {
        auto mod = (uintptr_t)GetModuleHandleA(nullptr);
        auto dos = (PIMAGE_DOS_HEADER)mod;
        auto nt = (PIMAGE_NT_HEADERS)(mod + dos->e_lfanew);
        auto opt = (PIMAGE_OPTIONAL_HEADER)&nt->OptionalHeader;
        auto size = opt->SizeOfHeaders;
        for (auto&& section : std::span((PIMAGE_SECTION_HEADER)(mod + dos->e_lfanew + sizeof(IMAGE_NT_HEADERS)), nt->FileHeader.NumberOfSections)) {
            size += section.SizeOfRawData;
        }
        std::error_code ec;
        auto fs_size = std::filesystem::file_size(__argv[0], ec);
        if (!ec && size < fs_size) {
            wchar_t exe_path[MAX_PATH];
            GetModuleFileNameW(NULL, exe_path, MAX_PATH);
            std::ifstream input(exe_path, std::ios::binary);
            input.seekg(size);
            std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
            input.close();

            load_lua_state_and_run([&](lua_State* L) {
                load_lef_memory(L, std::string(buffer.begin(), buffer.end()));
                if (lua::pcall(L, 0, 0) != LUA_OK) {
                    std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
                    lua_pop(L, 1);
                }
            }, true);
            return 0;
        }
    }

    // check for no arguments
    if (argc == 1) {
        std::cout << "No arguments provided. Try \"luaxe help\"." << std::endl;
        return 0;
    }

    // check if first arg is a .lef file
    if (argc == 2 && std::filesystem::path(argv[1]).extension() == ".lef") {
        // run the .lef file
        load_lua_state_and_run([&](lua_State* L) {
            load_lef_file(L, argv[1]);
            if (lua::pcall(L, 0, 0) != LUA_OK) {
                std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
            }
        }, true);
        return 0;
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    // check for options
    for (int i = 1; i < argc; i++) {
        for (auto& opt : CommandLineInterface::Options) {
            if (CommandLineInterface::IsOption(argv[i], opt)) {
                opt.exec(std::cout, i);
                return 0;
            }
        }
    }

    std::cout << "No valid options provided. Try \"luaxe help\"." << std::endl;
    system("pause");

    return 0;
}