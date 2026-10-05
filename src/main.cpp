#include "pch.h"
#include "src/lua/modules_dir.h"
#include "src/lua/relaunch.h"
#include <version.h>
#include "commands/register.h"
#include "commands/update.h"
#include "commands/compile.h"
#include "commands/run.h"
#include "commands/install.h"
#include "lua/state.h"
#include "lua/lua_runtime.h"
#include <luaxe/bind.h>
#include "cli.h"
#include "path.h"

// TB-191: the anchor behind src/lxe_exports.def. lxe.exe must carry a valid,
// minimal export directory (exactly this one name): LuaJIT's SusGetProcAddress
// walks the EXE's own exports for every ffi.C lookup, and with no export
// directory it reads DOS-header garbage and faults at lua_close. This symbol
// does nothing and must never be called; no lua_* stub may ever be exported
// beside it (ffi.C must keep resolving those in lua51.dll, not lxe).
// The dllexport below is what emits the export entry (CMake/Ninja does not
// pass a .def to the linker for an executable target); the .def only pins
// the PRIVATE attribute and documents the contract.
extern "C" __declspec(dllexport) void luaxe_export_anchor(void) {}

int main(int argc, char** argv) {
    // Before anything else: a launcher's relaunch handshake is read and removed
    // from this process's environment, so nothing it starts inherits it.
    relaunch::read_handshake();
    AddDllDirectory((std::filesystem::current_path() / "modules").wstring().c_str());

    // TB-195: ONE Lua runtime. lxe.exe links no LuaJIT; the runtime is loaded
    // at run time, before ANY lua_* call. The require lives in the two places
    // that can reach Lua - load_lua_state_and_run() and LefFile::store_as_lef()
    // - so a command that never touches Lua (`luaxe help`, `luaxe version`,
    // `luaxe register`) neither downloads nor needs a runtime at all. The first
    // call wins; the second is a no-op.
    std::string runtime_error;
    auto require_lua_runtime = [&runtime_error](const std::filesystem::path& bundled) {
        if (lua_runtime::ensure(bundled, &runtime_error)) return;
        std::cerr << "Error: no usable Lua runtime." << std::endl
                  << "  " << runtime_error << std::endl;
        std::exit(1);
    };

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

            // Pre-parse the LEF to extract bundled DLLs before Lua runs; they
            // unpack next to this exe (<exe dir>\modules\<name>\<version>\).
            modules_dir::set_app_dir(modules_dir::exe_dir());
            if (!LefFile::extract_bundled_dlls(std::string(buffer.begin(), buffer.end()))) {
                std::cerr << "Error: this executable's payload is not a valid .lef (the file is damaged)" << std::endl;
                return 1;
            }

            // TB-195: the payload's own lua51.dll (unpacked into <exe>\modules
            // above when that had none) is one of the runtime's candidates, so
            // bind it before the state that runs this payload is created.
            require_lua_runtime({});

            load_lua_state_and_run([&](lua_State* L) {
                load_lef_memory(L, std::string(buffer.begin(), buffer.end()));
                if (lua::pcall(L, 0, 0) != LUA_OK) {
                    std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
                    lua_pop(L, 1);
                }
            }, true);
            // env.relaunch: the payload runs again (this exe, no prefix).
            if (relaunch::requested()) relaunch::perform(L"");
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
            return 0;
        }

        std::cout << "No arguments provided. Try \"luaxe help\"." << std::endl;
        return 0;
    }

    // disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);

    // `lxe file.lef [args]` / `lxe file.lua [args]` is `lxe run file [args]`: what
    // the .lef file association, a file dropped on the exe and a bare
    // `lxe app.lef` send. parse_and_run(_, 0) reads the file from argv[1].
    if (is_script_path(argv[1])) {
        parse_and_run(std::cout, 0);
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

    return 0;
}