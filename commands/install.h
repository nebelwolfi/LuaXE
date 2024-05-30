//
// Created by Nebelwolfi on 21/05/2024.
//

#ifndef LUAXE_INSTALL_H
#define LUAXE_INSTALL_H

#include "json.hpp"
#include "https/connection/API.h"
#include "https/misc/md5.h"

static void update_file(std::string name, std::string version, std::string file_name, std::string md5, std::ostream& out) {
    std::string path = "modules/" + name + "/" + file_name;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream input(path, std::ios::binary);
        std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
        input.close();
        auto local_md5 = ::md5(std::string(buffer.data(), buffer.data() + buffer.size()));
        if (md5 == local_md5) {
            return;
        }
    }
    API a;
    out << "    " << file_name;
    if (a.DownloadFile("luaxe.dev", "/api/v1?action=download&module=" + name + "&version=" + version + "&file=" + file_name, path)) {
        out << " [OK]" << std::endl;
    } else {
        out << " [FAIL]" << std::endl;
    }
}

static void install_by_string(nlohmann::json& json, std::ostream& out, std::string name) {
    std::string version = "latest";
    if (auto pos = name.find('@'); pos != std::string::npos) {
        version = name.substr(pos + 1);
        name = name.substr(0, pos);
    }
    // {"name":"html","version":"1.0.0","files":[{"name":"html.dll","size":1201152,"date":"2024-05-17T20:59:04.769Z","md5":"d0255f2300e2207363c759d78ad7eb48"}],"dependencies":{"gui":">=1.0.0"}}
    API a;
    if (auto response = a.WebRequest(L"luaxe.dev", "/api/v1?action=list&module=" + name + "&version=" + version); !response.empty()) {
        auto j = nlohmann::json::parse(response);
        if (j.contains("error")) {
            out << j["error"].get<std::string>() << std::endl;
            return;
        }
        if (!std::filesystem::exists("modules/" + name))
            out << "[+] installing " << name << "@" << j["version"] << std::endl;
        if (!json.contains("dependencies")) {
            json["dependencies"] = nlohmann::json::object();
        }
        for (auto&& [dep_name, dep_version] : j["dependencies"].items()) {
            install_by_string(json, out, dep_name + "@" + dep_version.get<std::string>());
        }
        json["dependencies"][name] = j["version"];
        std::filesystem::create_directories("modules/" + name);
        for (auto&& [_, file] : j["files"].items()) {
            update_file(name, version, file["name"], file["md5"], out);
        }
    } else {
        out << "[-] failed to install " << name << "@" << version << std::endl;
    }
}

static void install(std::ostream& out, int i) {
    if (__argc <= i + 1) {
        std::error_code ec;
        if (std::filesystem::exists("modules/module.json", ec)) {
            auto json = nlohmann::json::parse(std::ifstream("modules/module.json"));
            if (json.contains("dependencies")) {
                for (auto&& [name, version] : json["dependencies"].items()) {
                    std::string cmd = "luaxe install " + name + "@\"" + version.get<std::string>() + "\"";
                    std::cout << cmd << std::endl;
                    system(cmd.c_str());
                }
            }
        }
        return;
    }
    std::string name = __argv[i + 1];

    nlohmann::json json;
    std::error_code ec;
    if (std::filesystem::exists("modules/module.json", ec)) {
        json = nlohmann::json::parse(std::ifstream("modules/module.json"));
    } else {
        json = nlohmann::json::object();
    }

    install_by_string(json, out, name);

    std::ofstream("modules/module.json") << json.dump(4);
}

static void upgrade(std::ostream& out, int i) {
    if (__argc <= i + 1) {
        std::error_code ec;
        if (std::filesystem::exists("modules/module.json", ec)) {
            auto json = nlohmann::json::parse(std::ifstream("modules/module.json"));
            if (json.contains("dependencies")) {
                for (auto&& [name, version] : json["dependencies"].items()) {
                    std::string cmd = "luaxe install " + name + "@\"latest\"";
                    std::cout << cmd << std::endl;
                    system(cmd.c_str());
                }
            }
        }
        return;
    }

    std::string name = __argv[i + 1];

    nlohmann::json json;
    std::error_code ec;
    if (std::filesystem::exists("modules/module.json", ec)) {
        json = nlohmann::json::parse(std::ifstream("modules/module.json"));
    } else {
        json = nlohmann::json::object();
    }

    if (json.contains("dependencies") && json["dependencies"].contains(name)) {
        json["dependencies"].erase(name);
    }

    install_by_string(json, out, name);

    std::ofstream("modules/module.json") << json.dump(4);
}

static void install_module(std::string name) {
    nlohmann::json json;
    std::error_code ec;
    if (std::filesystem::exists("modules/module.json", ec)) {
        json = nlohmann::json::parse(std::ifstream("modules/module.json"));
    } else {
        json = nlohmann::json::object();
    }

    install_by_string(json, std::cout, name);

    std::ofstream("modules/module.json") << json.dump(4);
}

#endif //LUAXE_INSTALL_H
