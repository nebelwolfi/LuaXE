//
// Created by Nebelwolfi on 21/05/2024.
//

#ifndef LUAXE_INSTALL_H
#define LUAXE_INSTALL_H

#include "../json.hpp"
#include "../https/connection/API.h"
#include "../https/misc/md5.h"
#include "../lua/modules_dir.h"

// Installs go into a MODULES ROOT: <root>/<module>/<file>, with the root's
// dependency list in <root>/module.json. `lxe install` (a project command)
// passes the project's own "modules"; import() passes the folder it resolved
// (the project's, for a module the project already has, otherwise the modules
// directory - see src/lua/modules_dir.h).

/// A file name the registry sends must stay inside its module's folder.
static bool safe_module_file(const std::string& file_name) {
    std::filesystem::path p(file_name);
    if (file_name.empty() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return true;
}

/// Download straight into a temp file beside `target`, then swap it in
/// atomically: another process loading the module never sees half a DLL, and a
/// DLL some process has loaded is renamed aside instead of failing the install.
static bool download_module_file(const std::string& url, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    auto temp = target;
    temp += "." + std::to_string(GetCurrentProcessId()) + ".download";
    std::filesystem::remove(temp, ec);
    bool ok = false;
    try {
        API a;
        ok = a.DownloadFile("luaxe.dev", url, temp.string());
    } catch (const std::exception&) {
        ok = false; // DownloadFile parses Content-Length and can throw on a bad response
    }
    if (!ok) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::string error;
    if (!modules_dir::replace_with(target, temp, &error)) {
        std::filesystem::remove(temp, ec);
        std::cerr << "Error: " << error << std::endl;
        return false;
    }
    return true;
}

static bool update_file(const std::filesystem::path& root, std::string name, std::string version, std::string file_name, std::string md5, std::ostream& out) {
    if (!safe_module_file(file_name)) {
        out << "    " << file_name << " [REFUSED: outside the module folder]" << std::endl;
        return false;
    }
    auto path = root / name / std::filesystem::path(file_name);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream input(path, std::ios::binary);
        std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
        input.close();
        auto local_md5 = ::md5(std::string(buffer.data(), buffer.data() + buffer.size()));
        if (md5 == local_md5) {
            return true;
        }
    }
    out << "    " << file_name;
    if (download_module_file("/api/v1?action=download&module=" + name + "&version=" + version + "&file=" + file_name, path)) {
        out << " [OK]" << std::endl;
        return true;
    }
    out << " [FAIL]" << std::endl;
    return false;
}

/// Returns false when the module (or a dependency) could not be installed.
/// `depth` guards a dependency cycle in a registry record.
static bool install_by_string(nlohmann::json& json, std::ostream& out, std::string name, const std::filesystem::path& root, int depth = 0) try {
    if (depth > 16) {
        out << "[-] refusing to install " << name << ": the dependency chain is deeper than 16 (a cycle?)" << std::endl;
        return false;
    }
    std::string version = "latest";
    if (auto pos = name.find('@'); pos != std::string::npos) {
        version = name.substr(pos + 1);
        name = name.substr(0, pos);
    }
    if (!safe_module_file(name) || name.find_first_of("/\\") != std::string::npos) {
        out << "[-] refusing to install \"" << name << "\": not a module name" << std::endl;
        return false;
    }
    // {"name":"html","version":"1.0.0","files":[{"name":"html.dll","size":1201152,"date":"2024-05-17T20:59:04.769Z","md5":"d0255f2300e2207363c759d78ad7eb48"}],"dependencies":{"gui":">=1.0.0"}}
    std::string response;
    try {
        API a;
        response = a.WebRequest(L"luaxe.dev", "/api/v1?action=list&module=" + name + "&version=" + version);
    } catch (const std::exception&) {
        response.clear();
    }
    nlohmann::json j;
    if (!response.empty()) {
        try {
            j = nlohmann::json::parse(response);
        } catch (const std::exception&) {
            j = nlohmann::json();
        }
    }
    if (!j.is_object()) {
        out << "[-] failed to install " << name << "@" << version << std::endl;
        return false;
    }
    if (j.contains("error")) {
        out << (j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump()) << std::endl;
        return false;
    }
    bool ok = true;
    std::error_code ec;
    if (!std::filesystem::exists(root / name, ec))
        out << "[+] installing " << name << "@" << j["version"] << " into " << root.string() << std::endl;
    if (!json.contains("dependencies")) {
        json["dependencies"] = nlohmann::json::object();
    }
    if (j.contains("dependencies")) {
        for (auto&& [dep_name, dep_version] : j["dependencies"].items()) {
            ok = install_by_string(json, out, dep_name + "@" + dep_version.get<std::string>(), root, depth + 1) && ok;
        }
    }
    json["dependencies"][name] = j["version"];
    std::filesystem::create_directories(root / name, ec);
    if (j.contains("files")) {
        for (auto&& [_, file] : j["files"].items()) {
            ok = update_file(root, name, version, file["name"], file["md5"], out) && ok;
        }
    }
    return ok;
} catch (const std::exception& why) {
    // A registry answer with a missing or mistyped field must not take the
    // process down (`lxe install` has no Lua frame to catch it).
    out << "[-] failed to install " << name << ": unexpected registry answer (" << why.what() << ")" << std::endl;
    return false;
}

/// <root>/module.json, or an empty object when there is none (or it is unreadable).
static nlohmann::json read_module_json(const std::filesystem::path& root) {
    std::error_code ec;
    auto file = root / "module.json";
    if (std::filesystem::exists(file, ec)) {
        try {
            return nlohmann::json::parse(std::ifstream(file));
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

static void install(std::ostream& out, int i) {
    if (__argc <= i + 1) {
        // Every dependency of the project, in this process (never through a
        // shell: the names and versions come from a file in the cwd).
        const std::filesystem::path root = "modules";
        nlohmann::json json = read_module_json(root);
        if (json.contains("dependencies") && json["dependencies"].is_object()) {
            auto dependencies = json["dependencies"];
            for (auto&& [name, version] : dependencies.items()) {
                std::string pinned = version.is_string() ? version.get<std::string>() : "latest";
                install_by_string(json, out, name + "@" + pinned, root);
            }
            write_module_json(root, json);
        }
        return;
    }
    std::string name = __argv[i + 1];

    // `lxe install` is a PROJECT command: it installs into ./modules and records
    // the dependency in ./modules/module.json.
    const std::filesystem::path root = "modules";
    nlohmann::json json = read_module_json(root);
    install_by_string(json, out, name, root);
    write_module_json(root, json);
}

static void upgrade(std::ostream& out, int i) {
    if (__argc <= i + 1) {
        const std::filesystem::path root = "modules";
        nlohmann::json json = read_module_json(root);
        if (json.contains("dependencies") && json["dependencies"].is_object()) {
            auto dependencies = json["dependencies"];
            for (auto&& [name, version] : dependencies.items()) {
                install_by_string(json, out, name + "@latest", root);
            }
            write_module_json(root, json);
        }
        return;
    }

    std::string name = __argv[i + 1];

    const std::filesystem::path root = "modules";
    nlohmann::json json = read_module_json(root);

    if (json.contains("dependencies") && json["dependencies"].contains(name)) {
        json["dependencies"].erase(name);
    }

    install_by_string(json, out, name, root);
    write_module_json(root, json);
}

/// import()'s automatic install: into `root` (a modules root, see above).
/// False when it did not work (the caller retries on a later import).
static bool install_module(std::string name, const std::filesystem::path& root) {
    nlohmann::json json = read_module_json(root);
    bool ok = install_by_string(json, std::cout, name, root);
    write_module_json(root, json);
    return ok;
}

#endif //LUAXE_INSTALL_H
