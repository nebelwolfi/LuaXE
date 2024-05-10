//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_IMPORT_H
#define LUAXE_IMPORT_H

#include "https/connection/API.h"
#include "https/misc/json.hpp"
#include "https/misc/md5.h"
#include <sys/utime.h>

extern "C" int ll_loadfunc(lua_State *L, const char *path, const char *name, int r);

int lua_pushandstore(lua_State *L, const char *name) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "modules");
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
    return 1;
}

std::unordered_map<std::string, std::mutex> fs_mutexes;

static __forceinline bool ichar_equals(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

static int import(lua_State* L) {
    std::string name = lua_tostring(L, 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    std::string version = "latest";
    if (lua_isstring(L, 2))
        version = lua_tostring(L, 2);
    {
        // check if its already in package.modules
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "modules");
        lua_pushvalue(L, 1);
        lua_gettable(L, -2);
        if (!lua_isnil(L, -1)) {
            return 1;
        }
        lua_pop(L, 3);
    }

    lua_remove(L, 1); // remove name
    if (!lua_isnone(L, 1))
        lua_remove(L, 1); // remove version
    auto argc = lua_gettop(L);
    auto modulename = name;
    auto bIsDll = true, bHasDot = false;
    if (modulename.find('.') != std::string::npos) {
        modulename = modulename.substr(0, modulename.find("."));
        bIsDll = false;
        bHasDot = true;
    }
    bool bShouldUpdate = true;
    if (version == "local" || lua::env::is_compiled()) {
        bShouldUpdate = bIsDll && !std::filesystem::exists(std::filesystem::path("modules") / modulename);
    }
    bool bFailedUpdate = false;
    if (bShouldUpdate)
        try {
            API a;
            //printf("Importing module: luaxe.%s\n", name.c_str());
            if (auto modulejson = a.WebRequest(L"luaxe.dev", "/module.php?name=" + modulename + "&version=" + version); !modulejson.empty()) {
                auto module = nlohmann::json::parse(modulejson, nullptr, false);
                /*[{"name":"libcrypto-3-x64.dll","stat":1697787512,"size":5146112,"hash":"34e221f7e4616b0310c2db47c5d30d62"},{"name":"libssl-3-x64.dll","stat":1697787511,"size":777728,"hash":"296959ba144b01b55b998542ec14a34a"},{"name":"mysqlcppconn8-2-vs14.dll","stat":1697787513,"size":4025856,"hash":"3e2fa5a254515965e2f2030653dc31e5"},{"name":"mysqlcppconn-9-vs14.dll","stat":1697787515,"size":9179136,"hash":"3fb819df2896ba8d596a85ffa8873dd0"}]*/
                std::filesystem::create_directories("modules");
                bIsDll = module[0]["type"].get<std::string>() == "dll";
                for (auto idx = 1; idx < module.size(); idx ++) {
                    auto&& file = module[idx];
                    auto fname = file["name"].get<std::string>();
                    auto ostat = file["stat"].get<uint64_t>();
                    auto size = file["size"].get<uint64_t>();
                    auto hash = file["hash"].get<std::string>();
                    auto path = std::filesystem::path("modules") / modulename / fname;
                    auto nameinmodule = "modules." + modulename + "." + fname;
                    std::replace(nameinmodule.begin(), nameinmodule.end(), '/', '.');
                    if (nameinmodule.ends_with(".lua"))
                        nameinmodule = nameinmodule.substr(0, nameinmodule.size() - 4);
                    //printf("Name in module: %s\n", nameinmodule.c_str());
                    for (auto&& lefs : LefFile::loaded)
                    for (auto&& cfile : lefs.files) {
                        if (std::ranges::equal(cfile.name, nameinmodule, ichar_equals)) {
                            //printf("Skipping file: %s (%.2fMB)\n", fname.c_str(), size / 1024.0 / 1024.0);
                            goto skip;
                        }
                    }
                    if (std::filesystem::exists(path)) {
                        auto fsize = std::filesystem::file_size(path);
                        struct stat buffer;
                        ::stat(path.string().c_str(), &buffer);
                        if (fsize == size && (buffer.st_mtime == ostat || buffer.st_ctime > ostat)) {
                            std::ifstream input(path, std::ios::binary);
                            std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
                            input.close();
                            auto fhash = md5(std::string(buffer.data(), buffer.data() + buffer.size()));
                            if (fhash == hash) {
                                continue;
                            }
                        }
                        printf("Updating file: %s (%.2fMB)\n", fname.c_str(), size / 1024.0 / 1024.0);
                    } else
                        printf("Downloading file: %s (%.2fMB)\n", fname.c_str(), size / 1024.0 / 1024.0);
                    if (!a.DownloadFile("luaxe.dev", "/modules/" + modulename + "/" + fname, path.string()))
                    {
                        lua_pushstring(L, ("Failed to download file: " + fname + " for module: " + modulename).c_str());
                        lua_error(L);
                        return 0;
                    } else {
                        struct utimbuf new_times;
                        struct stat buffer;
                        ::stat(path.string().c_str(), &buffer);
                        new_times.actime = buffer.st_atime;
                        new_times.modtime = ostat;
                        utime(path.string().c_str(), &new_times);
                    }
                    skip:
                }
            } else {
                printf("Failed to update module: %s\n", name.c_str());
                bFailedUpdate = true;
            }
        } catch (std::exception& e) {
            printf("Failed to update module: %s (%s)\n", modulename.c_str(), e.what());
            bFailedUpdate = true;
        }
    if (bIsDll && !bHasDot && (bFailedUpdate || !bShouldUpdate)) { // might not be dll after all
        auto ec = std::error_code();
        bIsDll = std::filesystem::exists(std::filesystem::path("modules") / modulename / (modulename + ".dll"), ec);
    }
    //printf("Importing module: %s | bIsDll: %d | bHasDot: %d | moduleentry: %s\n", req.c_str(), bIsDll, bHasDot, moduleentry.c_str());
    if (!bIsDll && !bHasDot) {
        //printf("!> Imported but not loaded lua module: %s\n", req.c_str());
        return 0;
    }
    lua_getglobal(L, "arg");
    lua_newtable(L);
    for (auto i = 1; i <= argc; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
    if (bIsDll)
    {
        {
            // package.cpath = package.cpath + ";%dir%/modules/" + name + "/?.dll";
            lua_getglobal(L, "package");
            lua_getfield(L, -1, "cpath");
            std::string cpath = lua_tostring(L, -1);
            lua_pop(L, 1);
            cpath += ";" + std::filesystem::current_path().string() + "\\modules\\" + name + "\\?.dll";
            lua_pushstring(L, cpath.c_str());
            lua_setfield(L, -2, "cpath");
            lua_pop(L, 1);
        }
        if (!std::filesystem::exists(std::filesystem::current_path() / "modules" / "lua51.dll")) {
            API a;
            if (!a.DownloadFile("luaxe.dev", "/modules/lua51.dll", (std::filesystem::current_path() / "modules" / "lua51.dll").string())) {
                lua_pushstring(L, "Failed to download lua51.dll");
                lua_error(L);
                return 0;
            }
        }
        //SetDllDirectoryA((std::filesystem::current_path().string() + "\\modules\\" + name).c_str());
        AddDllDirectory((std::filesystem::current_path() / "modules" / name).wstring().c_str());
        //printf("Loading dll module: %s\n", name.c_str());
        if (ll_loadfunc(L, ((std::filesystem::current_path() / "modules" / name / name).string() + ".dll").c_str(), name.c_str(), 0)) {
            lua_error(L);
            return 0;
        }
        lua_call(L, 0, 1);
    } else {
        //printf("Loading lua module: %s\n", name.c_str());
        lua_getglobal(L, "require");
        lua_pushstring(L, name.c_str());
        lua_call(L, 1, 1);
    }
    lua_pushvalue(L, -2);
    lua_setglobal(L, "arg");
    return bIsDll ? lua_pushandstore(L, name.c_str()) : 1;
}

static int import_open(lua_State* L) {
    lua_pushcfunction(L, import);
    lua_setglobal(L, "import");
    return 0;
}

#endif //LUAXE_IMPORT_H
