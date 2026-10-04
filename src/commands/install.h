//
// Created by Nebelwolfi on 21/05/2024.
//

#ifndef LUAXE_INSTALL_H
#define LUAXE_INSTALL_H

#include "../json.hpp"
#include "../https/connection/API.h"
#include "../https/misc/md5.h"
#include "../lua/modules_dir.h"

// Modules are installed into lxe's versioned STORE (src/lua/modules_dir.h):
//
//     %USERPROFILE%\.lxe\modules\<name>\<version>\<files>
//
// shared by every lxe and every app. A version folder is written in one move
// (downloaded into a temp folder beside it, checked against the registry's
// md5s, then renamed into place), so a folder that exists is complete and a
// running program never sees half a module. Nothing in a version folder is
// ever overwritten: another version is another folder.
//
// A project records which version it uses in ./modules/module.json
// ({"dependencies": {"name": "1.2.3"}}); `lxe install` / `lxe upgrade` are the
// project commands that maintain it.

/// A file name the registry sends must stay inside its module's folder.
static bool safe_module_file(const std::string& file_name) {
    std::filesystem::path p(file_name);
    if (file_name.empty() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) {
        if (part == ".." || part.string().find(':') != std::string::npos) return false;
    }
    return true;
}

/// Percent-encodes a query value (RFC 3986 unreserved characters stay).
static std::string url_encode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static bool safe_module_name(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\:@ ") != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    return name.front() != '.' && name.back() != '.';
}

static std::string file_md5(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return "";
    std::string data((std::istreambuf_iterator<char>(input)), {});
    return ::md5(data);
}

/// The registry's record for name@request ({"name","version","files":[{name,md5}],
/// "dependencies"}), or a null json (offline, unknown module, bad answer).
static nlohmann::json registry_record(const std::string& name, const std::string& request, std::ostream& out) {
    std::string response;
    try {
        API a;
        response = a.WebRequest(L"luaxe.dev", "/api/v1?action=list&module=" + url_encode(name) + "&version=" + url_encode(request));
    } catch (const std::exception&) {
        response.clear();
    }
    if (response.empty()) return nlohmann::json();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(response);
    } catch (const std::exception&) {
        return nlohmann::json();
    }
    if (!j.is_object()) return nlohmann::json();
    if (j.contains("error")) {
        out << "[-] " << name << "@" << request << ": "
            << (j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump()) << std::endl;
        return nlohmann::json();
    }
    if (!j.contains("version") || !j["version"].is_string() || !modules_dir::is_version(j["version"].get<std::string>()))
        return nlohmann::json();
    return j;
}

// ---- "latest", remembered for a day -----------------------------------------
// What the registry calls the latest version is asked at most once a day per
// module (<store>\<name>\latest.json), so `import("x")` in a source run does not
// cost a request on every start, and works offline from the last answer.

static constexpr long long kLatestTtlSeconds = 24 * 60 * 60;

static std::string remembered_latest(const std::string& name, bool fresh_only) {
    std::error_code ec;
    auto file = modules_dir::store() / name / "latest.json";
    if (!std::filesystem::exists(file, ec)) return "";
    try {
        auto j = nlohmann::json::parse(std::ifstream(file));
        if (!j.is_object() || !j.contains("version") || !j["version"].is_string()) return "";
        auto checked = j.contains("checked") && j["checked"].is_number() ? j["checked"].get<long long>() : 0;
        if (fresh_only && std::time(nullptr) - checked > kLatestTtlSeconds) return "";
        return j["version"].get<std::string>();
    } catch (const std::exception&) {
        return "";
    }
}

static void remember_latest(const std::string& name, const std::string& version) {
    nlohmann::json j = { { "version", version }, { "checked", (long long)std::time(nullptr) } };
    std::string error;
    modules_dir::write_atomically(modules_dir::store() / name / "latest.json", j.dump(), &error);
}

/// Installs name@request into the store (a version, a range, "latest" or "" =
/// latest) with its dependencies, and returns the version folder - or an empty
/// path when it could not (offline, no such version, a failed download).
/// `refresh` asks the registry for "latest" even when the day-old answer is fresh.
static std::filesystem::path install_into_store(const std::string& name, std::string request, std::ostream& out,
                                                bool refresh = false, int depth = 0) try {
    if (depth > 16) {
        out << "[-] refusing to install " << name << ": the dependency chain is deeper than 16 (a cycle?)" << std::endl;
        return {};
    }
    if (!safe_module_name(name)) {
        out << "[-] refusing to install \"" << name << "\": not a module name" << std::endl;
        return {};
    }
    if (request.empty() || request == "*") request = "latest";
    const bool latest = request == "latest";

    // Already there: an exact version, a range an installed version satisfies,
    // or "latest" when the day-old answer names an installed version.
    if (modules_dir::is_version(request)) {
        auto dir = modules_dir::store() / name / request;
        if (!modules_dir::installed_versions(name).empty() && modules_dir::best_installed(name, request) == dir) return dir;
    } else if (latest) {
        if (!refresh) {
            auto known = remembered_latest(name, true);
            if (!known.empty()) {
                auto dir = modules_dir::best_installed(name, known);
                if (!dir.empty()) return dir;
            }
        }
    } else {
        auto dir = modules_dir::best_installed(name, request);
        if (!dir.empty()) return dir;
    }

    auto j = registry_record(name, request, out);
    if (j.is_null()) {
        out << "[-] could not install " << name << "@" << request << " (offline, or no such module)" << std::endl;
        return {};
    }
    const std::string version = j["version"].get<std::string>();
    // The registry falls back to its latest for a version or range it does not
    // have; that is not what was asked for.
    if (!latest && !modules_dir::satisfies(version, request)) {
        out << "[-] " << name << "@" << request << " is not published (the registry has " << version << ")" << std::endl;
        return {};
    }

    if (j.contains("dependencies") && j["dependencies"].is_object()) {
        for (auto&& [dep_name, dep_range] : j["dependencies"].items()) {
            auto range = dep_range.is_string() ? dep_range.get<std::string>() : "latest";
            if (install_into_store(dep_name, range, out, false, depth + 1).empty()) return {};
        }
    }

    const auto target = modules_dir::store() / name / version;
    std::error_code ec;
    if (!modules_dir::best_installed(name, version).empty()) {
        if (latest) remember_latest(name, version);
        return target;
    }

    out << "[+] installing " << name << "@" << version << " into " << target.string() << std::endl;
    auto staging = modules_dir::store() / name / ("." + version + "." + std::to_string(GetCurrentProcessId()) + ".tmp");
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        out << "[-] could not create " << staging.string() << " (" << ec.message() << ")" << std::endl;
        return {};
    }
    bool ok = true;
    if (j.contains("files") && j["files"].is_array()) {
        for (auto&& file : j["files"]) {
            if (!file.is_object() || !file.contains("name") || !file["name"].is_string()) { ok = false; break; }
            std::string file_name = file["name"].get<std::string>();
            // An md5 is mandatory: a file the registry does not vouch for never
            // enters the store every lxe and app loads native code from.
            std::string md5 = file.contains("md5") && file["md5"].is_string() ? file["md5"].get<std::string>() : "";
            if (md5.empty()) {
                out << "    " << file_name << " [REFUSED: the registry gave no md5]" << std::endl;
                ok = false;
                break;
            }
            if (!safe_module_file(file_name)) {
                out << "    " << file_name << " [REFUSED: outside the module folder]" << std::endl;
                ok = false;
                break;
            }
            auto path = staging / std::filesystem::path(file_name);
            std::filesystem::create_directories(path.parent_path(), ec);
            out << "    " << file_name;
            bool downloaded = false;
            try {
                API a;
                downloaded = a.DownloadFile("luaxe.dev", "/api/v1?action=download&module=" + url_encode(name) + "&version="
                    + url_encode(version) + "&file=" + url_encode(file_name), path.string());
            } catch (const std::exception&) {
                downloaded = false;
            }
            if (!downloaded || file_md5(path) != md5) {
                out << (downloaded ? " [FAIL: checksum]" : " [FAIL]") << std::endl;
                ok = false;
                break;
            }
            out << " [OK]" << std::endl;
        }
    }
    if (!ok) {
        std::filesystem::remove_all(staging, ec);
        return {};
    }
    // One move publishes the whole version. Losing the race to another process
    // that installed the same version is fine: its folder is the same module.
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0)) {
        std::filesystem::remove_all(staging, ec);
        if (modules_dir::best_installed(name, version).empty()) {
            out << "[-] could not publish " << target.string() << " (Windows error " << GetLastError() << ")" << std::endl;
            return {};
        }
    }
    // Remembered only once that version really is installed.
    if (latest) remember_latest(name, version);
    return target;
} catch (const std::exception& why) {
    // A registry answer with a missing or mistyped field must not take the
    // process down (`lxe install` has no Lua frame to catch it).
    out << "[-] failed to install " << name << ": unexpected registry answer (" << why.what() << ")" << std::endl;
    return {};
}

// ---- the project's ./modules/module.json -------------------------------------

/// <root>/module.json, or an empty object when there is none (or it is unreadable).
static nlohmann::json read_module_json(const std::filesystem::path& root) {
    std::error_code ec;
    auto file = root / "module.json";
    if (std::filesystem::exists(file, ec)) {
        try {
            auto j = nlohmann::json::parse(std::ifstream(file));
            if (j.is_object()) return j;
        } catch (const std::exception&) {
        }
    }
    return nlohmann::json::object();
}

static void write_module_json(const std::filesystem::path& root, const nlohmann::json& json) {
    std::string error;
    if (!modules_dir::write_atomically(root / "module.json", json.dump(4), &error))
        std::cerr << "Warning: " << error << std::endl;
}

/// The version the project in the cwd pins for `name` (./modules/module.json),
/// or "" when it pins none.
static std::string project_pin(const std::string& name) {
    auto json = read_module_json("modules");
    if (!json.contains("dependencies") || !json["dependencies"].is_object()) return "";
    auto& deps = json["dependencies"];
    for (auto it = deps.begin(); it != deps.end(); ++it) {
        std::string key = it.key();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (key == name && it->is_string()) return it->get<std::string>();
    }
    return "";
}

static void pin(nlohmann::json& json, const std::string& name, const std::string& version) {
    if (!json.contains("dependencies") || !json["dependencies"].is_object()) json["dependencies"] = nlohmann::json::object();
    json["dependencies"][name] = version;
}

/// `lxe install [name[@version-or-range]]`: into the store; the project
/// (./modules/module.json) records the exact version it got. Without a name,
/// every dependency the project records.
static void install(std::ostream& out, int i) {
    const std::filesystem::path root = "modules";
    nlohmann::json json = read_module_json(root);
    if (__argc <= i + 1) {
        if (json.contains("dependencies") && json["dependencies"].is_object()) {
            auto dependencies = json["dependencies"];
            for (auto&& [name, version] : dependencies.items()) {
                std::string wanted = version.is_string() ? version.get<std::string>() : "latest";
                auto dir = install_into_store(name, wanted, out);
                if (dir.empty()) continue;
                out << "    " << name << " " << dir.filename().string() << std::endl;
            }
        }
        return;
    }
    std::string name = __argv[i + 1], request = "latest";
    if (auto at = name.find('@'); at != std::string::npos) {
        request = name.substr(at + 1);
        name = name.substr(0, at);
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto dir = install_into_store(name, request, out);
    if (dir.empty()) return;
    pin(json, name, dir.filename().string());
    write_module_json(root, json);
    out << "    " << name << " " << dir.filename().string() << " (recorded in " << (root / "module.json").string() << ")" << std::endl;
}

/// `lxe upgrade [name]`: the registry's latest into the store, and the project
/// records it.
static void upgrade(std::ostream& out, int i) {
    const std::filesystem::path root = "modules";
    nlohmann::json json = read_module_json(root);
    std::vector<std::string> names;
    if (__argc <= i + 1) {
        if (json.contains("dependencies") && json["dependencies"].is_object())
            for (auto&& [name, _] : json["dependencies"].items()) names.push_back(name);
    } else {
        names.push_back(__argv[i + 1]);
    }
    // The project records the new versions only when there is a project (a
    // ./modules folder) - `lxe upgrade x` elsewhere just refreshes the store.
    std::error_code ec;
    const bool project = std::filesystem::is_directory(root, ec);
    bool changed = false;
    for (auto name : names) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto dir = install_into_store(name, "latest", out, true);
        if (dir.empty()) continue;
        if (project) { pin(json, name, dir.filename().string()); changed = true; }
        out << "    " << name << " " << dir.filename().string() << std::endl;
    }
    if (changed) write_module_json(root, json);
}

#endif //LUAXE_INSTALL_H
