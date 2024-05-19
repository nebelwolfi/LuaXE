#include <pch.h>
#include <version.h>
#include <commands/register.h>
#include <commands/update.h>
#include <commands/compile.h>
#include <commands/run.h>
#include <lua/state.h>
#include <luaxe/bind.h>

namespace CommandLineInterface {
    struct Option {
        std::string name;
        std::string description;
        std::function<void(std::ostream&, int)> exec; // returns true if the program should exit
        std::string extended_description;
        std::vector<std::string> aliases;
    };

    std::vector<Option> Options = {
        {
            "version",
            LuaXE_VERSION,
            [](std::ostream& out, int i)
            {
                out << "LuaXE [Version " LuaXE_VERSION "]" << std::endl;
            }
        },
        {
            "help",
            "",
            [](std::ostream& out, int i)
            {
                if (i + 1 < __argc) {
                    for (auto&& opt : Options) {
                        if (opt.name == __argv[i + 1]) {
                            if (!opt.extended_description.empty())
                                out << "luaxe " << opt.name << opt.extended_description << std::endl;
                            else
                                out << "luaxe " << opt.name << " - " << opt.description << std::endl;
                            return;
                        }
                    }
                }

                out << "luaxe [options]\n" << std::endl;
                for (auto&& opt : Options) if (opt.name != "help") out << std::left << std::setw(4) << std::left << " " << std::setw(12) << opt.name << opt.description << std::endl;
                out << "\nIf you need help with a specific command, type \"luaxe help <command>\"." << std::endl;
            }, "bruh", {"h", "?", "-h", "/?"}
        },
        {
            "update",
            "check for updates, and update",
            [](std::ostream& out, int i)
            {
                check_for_updates_and_update();
            }
        },
        {
            "register",
            "registers the current exe as .lef file extension handler",
            [](std::ostream& out, int i)
            {
                register_as_dot_lef_handler();
            }
        },
        {
            "init",
            "initializes a directory with a default project",
            [](std::ostream& out, int i)
            {
                initialize_directory_with_default_project();
            }
        },
        {
            "run",
            "syntax:\n\t\t\tluaxe run file.lua/.lef\n\t\t\tluaxe run file [args]",
            parse_and_run,
            " file [args]"
               "\n\nfile can be a .lua or .lef file"
               "\n\nargs:"
               "\n\t<args> - pass arguments to the script\n\t",
            {"r"}
        },
        {
            "compile",
            "syntax:\n\t\t\tluaxe compile file.lua\n\t\t\tluaxe compile source [options] [--args]",
            parse_and_compile,
            " source [options] [--args]"
               "\n\nsource can be a file or directory"
               "\n\noptions:"
               "\n\t-o <output> - output file, .lef or .exe"
               "\n\t-s - strip debug info"
               "\n\t-l - pack as lef instead of exe, output file extension overrides this"
               "\n\t-v - verbose"
               "\n\t-m <main> - main file if source is a directory"
               "\n\t\tif directory and -m is not specified it will look for 'main.lua'"
               "\n\t-- <args> - pass arguments to the script\n\t",
            {"c"}
        },
    };

    bool IsOption(const char* str, const Option& opt) {
        if (str == opt.name) return true;
        return std::any_of(opt.aliases.begin(), opt.aliases.end(), [str](const std::string& alias) { return str == alias; });
    }
}

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