#include "pch.h"
#include <version.h>
#include "commands/register.h"
#include "commands/update.h"
#include "commands/compile.h"
#include "commands/run.h"
#include "commands/install.h"
#include "lua/state.h"
#include <luaxe/bind.h>
#include "cli.h"
#include "path.h"

int main(int argc, char** argv) {
    AddDllDirectory((std::filesystem::current_path() / "modules").wstring().c_str());

    // argv might be missing .exe extension
    char exe_path[256] = { 0 };
    GetModuleFileNameA(nullptr, exe_path, 256);

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
        auto fs_size = std::filesystem::file_size(exe_path, ec);
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
        if (DWORD list[1024] = {0}; 1 == GetConsoleProcessList(list, 1024)) {
            // likely opened by double-click
            auto printed_header = false;
            auto print_header = [&] {
                if (printed_header) return;
                printed_header = true;
                CommandLineInterface::Options[0].exec(std::cout, 0);
                std::cout << "\nIt looks like you opened luaxe.exe directly.\n";
            };
            auto did_confirm = []{
                int response = getchar();
                return response == 'Y' || response == 'y';
            };
            if (!is_path_in_PATH(std::filesystem::current_path())) {
                print_header();
                std::cout << "Do you want to add LuaXE to your PATH environment variable? (Y/N): ";
                if (did_confirm()) {
                    if (add_path_to_windows_PATH_persistent(std::filesystem::current_path(), PathScope::User)) {
                        std::cout << "Successfully added LuaXE to your PATH." << std::endl;
                    } else {
                        std::cout << "Failed to add LuaXE to your PATH." << std::endl;
                    }
                }
            }
            if (!is_registered_as_dot_lef_handler()) {
                print_header();
                std::cout << "Do you want to register LuaXE as the handler for .lef files? (Y/N): ";
                if (did_confirm()) {
                    if (register_as_dot_lef_handler()) {
                        std::cout << "Successfully registered LuaXE as .lef handler." << std::endl;
                    } else {
                        std::cout << "Failed to register LuaXE as .lef handler." << std::endl;
                    }
                }
            }
            //register_as_dot_lef_handler();
            if (printed_header)
                system("pause");
            return 0;
        }

        std::cout << "No arguments provided. Try \"luaxe help\"." << std::endl;
        return 0;
    }

    // todo check if first arg is a .lef or .lua file, requires fixup of argv to insert "run" at index 1

    // disable buffering
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