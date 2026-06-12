#ifndef LUAXE_SRC_PATH_H
#define LUAXE_SRC_PATH_H

bool is_path_in_PATH(const std::filesystem::path& dir);
enum class PathScope { User, Machine }; // HKCU or HKLM (admin needed for Machine)
bool add_path_to_windows_PATH_persistent(const std::filesystem::path& dir, PathScope scope = PathScope::User);

#endif //LUAXE_SRC_PATH_H