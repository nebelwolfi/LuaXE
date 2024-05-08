//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_REGISTER_H
#define LUAXE_REGISTER_H

static void register_as_dot_lef_handler() {
    // associate .lef files to launch with this exe
    wchar_t ep[MAX_PATH];
    GetModuleFileNameW(NULL, ep, MAX_PATH);
    auto exe_path = std::filesystem::path(ep);

    // create registry key 1 : HKEY_CURRENT_USER\Software\Classes\.lef
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.lef", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        printf("Failed to create registry key\n");

        return;
    }
    // set default value to "luaxe"
    if (RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)L"luaxe", 10) != ERROR_SUCCESS) {
        printf("Failed to set registry key value\n");

        return;
    }
    RegCloseKey(hKey);

    // create registry key 2 : HKEY_CURRENT_USER\Software\Classes\luaxe\shell\open\command
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\luaxe\\shell\\open\\command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        printf("Failed to create registry key\n");

        return;
    }
    // set default value to "luaxe.exe %1"
    if (RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)(std::wstring(L"\"") + exe_path.wstring() + L"\" \"%1\"").c_str(), (std::wstring(L"\"") + exe_path.wstring() + L"\" \"%1\"").size() * sizeof(wchar_t)) != ERROR_SUCCESS) {
        printf("Failed to set registry key value\n");

        return;
    }
    RegCloseKey(hKey);

    printf("Registered .lef files to launch with this exe\r\n");
}

#endif //LUAXE_REGISTER_H
