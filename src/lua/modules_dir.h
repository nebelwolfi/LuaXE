//
// Where lxe keeps its runtime and modules.
//
//   %LXE_HOME%  (default %USERPROFILE%\.lxe)
//     bin\                    lxe.exe and lua51.dll of the installed lxe; the
//                             runtime is cached here when it is downloaded
//     modules\<name>\<version>\
//                             the shared module store: what import() and
//                             `lxe install` fetch from luaxe.dev, one folder per
//                             version, for every lxe and every app
//
//   <app dir>\modules\<name>\<version>\
//                             what an app CARRIES (`lxe compile -b`): unpacked
//                             next to the app - the .lef, or the compiled exe -
//                             so no other project can load it by accident, and
//                             it always wins over the store for that app
//
// Never the working directory for a compiled program: its cwd is the user's
// workspace, and a DLL found there is a DLL somebody could have planted. A
// source run (`lxe run main.lua`) additionally honours flat
// <root>\modules\<name>\ folders (the layout before the store existed), in
// order: the cwd (the project), the running script's own folder (a checkout
// started from elsewhere), and the lxe executable's folder (a developer lxe
// with staged modules).
//
#ifndef LUAXE_MODULES_DIR_H
#define LUAXE_MODULES_DIR_H

#include <filesystem>
#include <string>
#include <vector>

namespace modules_dir {

/// Directory of the running executable (never the cwd, even when the path is
/// longer than MAX_PATH; falls back to the cwd only if Windows cannot answer).
std::filesystem::path exe_dir();

/// %LXE_HOME%, or <the account's profile folder>\.lxe (not %USERPROFILE%, which a
/// sandboxed run may point elsewhere: the store is shared and immutable).
std::filesystem::path lxe_home();
/// <lxe home>\bin - the installed lxe and its runtime.
std::filesystem::path bin();
/// <lxe home>\modules - the shared, versioned module store.
std::filesystem::path store();

/// The running app's own folder (the .lef's, or the compiled exe's), set before
/// any Lua runs; empty for a plain `lxe run x.lua`.
std::filesystem::path app_dir();
void set_app_dir(const std::filesystem::path& dir);

/// The folder of the .lua script a source run executes; empty otherwise.
void set_script_dir(const std::filesystem::path& dir);
/// The flat <root>\modules folders a source run looks in (see above), existing
/// ones only, without duplicates.
std::vector<std::filesystem::path> source_roots();

// ---- versions (semver: MAJOR.MINOR.PATCH[-pre][+build]) ----------------------

/// True when `text` is a plain version ("1.0.0", "v1.2.3" is not).
bool is_version(const std::string& text);
/// <0, 0, >0 like strcmp; both must be versions.
int compare_versions(const std::string& left, const std::string& right);
/// Does `version` satisfy `range`? Ranges: "" or "*" (any), "1.2.3" (exact),
/// ">=1.0.0", ">1", "<=", "<", "=", "^1.2.3", "~1.2.3", space-separated
/// comparators (all must hold) and "||" alternatives. Partial versions ("1",
/// "1.2") fill the missing parts with 0; a bare partial version matches its
/// prefix ("1.2" = ">=1.2.0 <1.3.0").
bool satisfies(const std::string& version, const std::string& range);
/// The versions installed under <store>\<name>, highest first.
std::vector<std::string> installed_versions(const std::string& name);
/// <store>\<name>\<highest installed version satisfying range>, or empty.
std::filesystem::path best_installed(const std::string& name, const std::string& range);

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
