//
// Created by Nebelwolfi on 08/05/2024.
//
#include "../pch.h"
#include "update.h"
#include "../https/connection/API.h"
#include "../https/misc/md5.h"

void check_for_updates_and_update() {

    wchar_t ep[MAX_PATH];
    GetModuleFileNameW(NULL, ep, MAX_PATH);
    auto exe_path = std::filesystem::path(ep);

    API a;
    if (auto md5_online = a.WebRequest(L"luaxe.dev", "/md5"); !md5_online.empty()) {
        std::ifstream input(exe_path, std::ios::binary);
        while (!input.is_open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            input.open(exe_path, std::ios::binary);
        }
        std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
        input.close();

        std::string exe = std::string(buffer.data(), buffer.data() + buffer.size());
        std::string md5_local = md5(exe);

        if (md5_local != md5_online) {
            std::cout << "Update available, downloading..." << std::endl;
            a.DownloadFile("luaxe.dev", "/get", exe_path.string());
            printf("Update complete, it will take effect on next launch\n");
        } else {
            printf("No updates available\n");
        }
    } else {
        printf("Failed to check for updates\n");
    }
}

void initialize_directory_with_default_project() {
    bool already_initialized = std::filesystem::exists("main.lua") || std::filesystem::exists("premain.lua");
    API a;
    if (!std::filesystem::exists("main.lua") && !a.DownloadFile("luaxe.dev", "/init/main.lua", (std::filesystem::current_path() / "main.lua").string())) {
        printf("Failed to download main.lua\n");
        return;
    }
    if (!std::filesystem::exists("premain.lua") && !a.DownloadFile("luaxe.dev", "/init/premain.lua", (std::filesystem::current_path() / "premain.lua").string())) {
        printf("Failed to download premain.lua\n");
        return;
    }
    bool already_initialized_doc = std::filesystem::exists("__doc.lua");
    if (!a.DownloadFile("luaxe.dev", "/init/__doc.lua", (std::filesystem::current_path() / "__doc.lua").string())) {
        printf("Failed to download __doc.lua\n");
        return;
    }
    if (!already_initialized)
        printf("Project initialized\n");
    else if (!already_initialized_doc)
        printf("Downloaded __doc.lua\n");
    else
        printf("Updated __doc.lua\n");
}