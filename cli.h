#ifndef LUAXE_CLI_H
#define LUAXE_CLI_H

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
                    }, "", {"h", "?", "-h", "/?"}
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
                       {"c", "build"}
            },
            {
                    "install",
                    "syntax:\n\t\t\tluaxe install module\n\t\t\tluaxe install module [version]",
                    install,
                       " [module@version]"
                       "\n\nif no module is specified it will install all dependencies from the current directory"
                       "\n\nmodule - name of the module to install"
                       "\n\nversion - version of the module to install, if not specified it will install the latest version\n\t",
                    {"add", "i", "in", "ins", "inst", "insta", "instal", "isnt", "isnta", "isntal", "isntall"}
            },
            {
                    "upgrade",
                    "syntax:\n\t\t\tluaxe upgrade\n\t\t\tluaxe upgrade module",
                    upgrade,
                       " [module]"
                       "\n\nif no module is specified it will upgrade all modules"
                       "\n\nmodule - name of the module to upgrade\n\t",
                    {"up", "u", "upg", "upgr", "upgra", "upgrad"}
            }
    };

    bool IsOption(const char* str, const Option& opt) {
        if (str == opt.name) return true;
        return std::any_of(opt.aliases.begin(), opt.aliases.end(), [str](const std::string& alias) { return str == alias; });
    }
}

#endif //LUAXE_CLI_H
