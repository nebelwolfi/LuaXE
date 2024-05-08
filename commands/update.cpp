//
// Created by Nebelwolfi on 08/05/2024.
//
#include <pch.h>
#include "update.h"
#include "https/connection/API.h"
#include "https/misc/md5.h"

void check_for_updates_and_update() {

    wchar_t ep[MAX_PATH];
    GetModuleFileNameW(NULL, ep, MAX_PATH);
    auto exe_path = std::filesystem::path(ep);

    API a;
    if (auto md5_online = a.WebRequest(L"luaxe.dev", "/md5.php"); !md5_online.empty()) {
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
            a.DownloadFile("luaxe.dev", "/luaxe.exe", exe_path.string());
            printf("Update complete, it will take effect on next launch\n");
        } else {
            printf("No updates available\n");
        }
    } else {
        printf("Failed to check for updates\n");
    }
}

void initialize_directory_with_default_project() {

    if (std::filesystem::exists("main.lua") || std::filesystem::exists("premain.lua")) {
        printf("Project already initialized\n");
        return;
    }
    API a;
    if (!a.DownloadFile("luaxe.dev", "/init/main.lua", (std::filesystem::current_path() / "main.lua").string())) {
        printf("Failed to download main.lua\n");
        return;
    }
    if (!a.DownloadFile("luaxe.dev", "/init/premain.lua", (std::filesystem::current_path() / "premain.lua").string())) {
        printf("Failed to download premain.lua\n");
        return;
    }
    if (!a.DownloadFile("luaxe.dev", "/init/__doc.lua", (std::filesystem::current_path() / "__doc.lua").string())) {
        printf("Failed to download __doc.lua\n");
        return;
    }
    printf("Project initialized\n");
}