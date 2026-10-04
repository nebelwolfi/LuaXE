//
// The modules directory: <directory of the running executable>\modules.
//
// ONE place for every native module a process installs, unpacks and loads. A
// standalone lxe uses <lxe dir>\modules; an app installed as its own executable
// (girl.exe in %USERPROFILE%\.girl\bin) gets its own <app dir>\modules. It is
// never the working directory: for a compiled program the cwd is the user's
// workspace, and a DLL found there is a DLL somebody could have planted.
//
// A source run (`lxe run main.lua` inside a project) still looks at the
// project's own <cwd>\modules first - that is the project's dependency folder,
// managed by `lxe install` - but installs what it does not have into the
// modules directory, never into whatever directory it was started from.
//
#ifndef LUAXE_MODULES_DIR_H
#define LUAXE_MODULES_DIR_H

#include <filesystem>
#include <string>

namespace modules_dir {

/// Directory of the running executable (never the cwd, even when the path is
/// longer than MAX_PATH; falls back to the cwd only if Windows cannot answer).
std::filesystem::path exe_dir();

/// <exe dir>\modules - the modules directory.
std::filesystem::path home();

/// True when `path` holds exactly `data`.
bool same_contents(const std::filesystem::path& path, const std::string& data);

/// Put `data` at `target` atomically: written to a per-process temp file, then
/// moved into place. A target that a running process has mapped (a loaded DLL
/// cannot be overwritten on Windows, but it can be renamed) is first renamed
/// aside to <target>.old-<pid>-<n> and swept by a later call. Creates the parent
/// directories. A target that already holds `data` is left alone.
bool write_atomically(const std::filesystem::path& target, const std::string& data, std::string* error);

/// Same as write_atomically, for a file already on disk (`source` is moved, not
/// copied; it must be on the same volume as `target`).
bool replace_with(const std::filesystem::path& target, const std::filesystem::path& source, std::string* error);

/// Delete the <name>.old-* files write_atomically left beside `target` once no
/// process maps them any more (a file still in use is skipped silently).
void sweep_aside(const std::filesystem::path& target);

} // namespace modules_dir

#endif // LUAXE_MODULES_DIR_H
