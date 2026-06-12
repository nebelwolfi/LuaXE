#ifndef LUAXE_SRC_A_H
#define LUAXE_SRC_A_H

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <system_error>
#include <cstdlib>   // std::getenv
#ifdef _WIN32
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace detail {

#ifdef _WIN32
    inline std::wstring widen_env_path() {
        DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (needed == 0) return L"";
        std::wstring buf(needed, L'\0');
        DWORD written = GetEnvironmentVariableW(L"PATH", &buf[0], needed);
        buf.resize(written);
        return buf;
    }
    inline std::wstring expand_vars(std::wstring s) {
        DWORD needed = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        if (needed == 0) return s;
        std::wstring out(needed, L'\0');
        DWORD written = ExpandEnvironmentStringsW(s.c_str(), &out[0], needed);
        out.resize(written ? written - 1 : 0); // drop trailing NUL count
        return out;
    }
    inline void trim_ws(std::wstring& s) {
        auto is_ws = [](wchar_t c){ return c==L' ' || c==L'\t' || c==L'\r' || c==L'\n'; };
        size_t a = 0, b = s.size();
        while (a < b && is_ws(s[a])) ++a;
        while (b > a && is_ws(s[b-1])) --b;
        s = s.substr(a, b - a);
    }
    inline std::wstring unquote(std::wstring s) {
        trim_ws(s);
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
            s = s.substr(1, s.size() - 2);
            trim_ws(s);
        }
        return s;
    }
    inline std::wstring to_lower(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](wchar_t c){ return static_cast<wchar_t>(::towlower(c)); });
        return s;
    }
#else
    inline std::string narrow_env_path() {
    const char* p = std::getenv("PATH");
    return p ? std::string(p) : std::string();
}
inline void trim_ws(std::string& s) {
    auto is_ws = [](char c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; };
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b-1])) --b;
    s = s.substr(a, b - a);
}
#endif

    inline fs::path normalize(const fs::path& p) {
        std::error_code ec;
        fs::path q = fs::weakly_canonical(p, ec); // resolves existing prefix, cleans . and ..
        if (ec) {
            q = fs::absolute(p, ec);
            if (ec) q = p.lexically_normal(); // purely lexical fallback
        }
        return q;
    }

#ifdef _WIN32
    inline bool lexical_equal_ci(const fs::path& a, const fs::path& b) {
        auto sa = to_lower(a.generic_wstring());
        auto sb = to_lower(b.generic_wstring());
        return sa == sb;
    }
#else
    inline bool lexical_equal_cs(const fs::path& a, const fs::path& b) {
    return a.generic_string() == b.generic_string();
}
#endif

} // namespace detail

// Returns true if `dir` is present among PATH entries.
bool is_path_in_PATH(const std::filesystem::path& dir) {
    using namespace detail;
    const fs::path normDir = normalize(dir);

#ifdef _WIN32
    const wchar_t delim = L';';
    std::wstring path = widen_env_path();
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(delim, start);
        std::wstring token = (end == std::wstring::npos)
                             ? path.substr(start)
                             : path.substr(start, end - start);
        start = (end == std::wstring::npos) ? path.size() + 1 : end + 1;

        token = unquote(expand_vars(token));
        trim_ws(token);
        if (token.empty()) continue;

        fs::path entry = normalize(fs::path(token));

        // Prefer a filesystem-aware equivalence check when both exist.
        std::error_code ec;
        if (fs::exists(entry, ec) && fs::exists(normDir, ec) &&
            fs::equivalent(entry, normDir, ec) && !ec) {
            return true;
        }

        // Fall back to case-insensitive lexical compare on Windows.
        if (lexical_equal_ci(entry, normDir)) return true;
    }
#else
    const char delim = ':';
    std::string path = narrow_env_path();
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(delim, start);
        std::string token = (end == std::string::npos)
                              ? path.substr(start)
                              : path.substr(start, end - start);
        start = (end == std::string::npos) ? path.size() + 1 : end + 1;

        trim_ws(token);
        if (token.empty()) continue; // empty PATH element => CWD; not equal to an absolute dir

        fs::path entry = normalize(fs::path(token));

        std::error_code ec;
        if (fs::exists(entry, ec) && fs::exists(normDir, ec) &&
            fs::equivalent(entry, normDir, ec) && !ec) {
            return true;
        }
        // POSIX: lexical compare is case-sensitive
        if (lexical_equal_cs(entry, normDir)) return true;
    }
#endif
    return false;
}

inline bool add_path_to_process_PATH(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;

#ifdef _WIN32
    // Get PATH (UTF-16)
    DWORD need = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path = (need ? std::wstring(need, L'\0') : std::wstring());
    if (need) {
        DWORD got = GetEnvironmentVariableW(L"PATH", &path[0], need);
        path.resize(got);
    }
    // Append if not already present (simple textual check to keep this short)
    // Use semicolon on Windows
    std::wstring add = dir.wstring();
    if (!path.empty() && path.back() != L';') path.push_back(L';');
    // Crude duplicate guard: only append if not found as a full token
    if (path.find(add) == std::wstring::npos) path += add;

    return SetEnvironmentVariableW(L"PATH", path.c_str()) != 0;
#else
    // POSIX: use colon
    const char* oldp = std::getenv("PATH");
    std::string path = oldp ? std::string(oldp) : std::string();
    std::string add = dir.string();

    // Crude duplicate guard
    auto contains_token = [&](const std::string& hay, const std::string& needle) {
        // Check (:|^)<needle>(:|$)
        if (hay == needle) return true;
        if (hay.rfind(needle + ":", 0) == 0) return true;                 // at start
        if (hay.find(":" + needle + ":") != std::string::npos) return true; // middle
        if (!hay.empty() && hay.size() >= needle.size() &&
            hay.compare(hay.size()-needle.size(), needle.size(), needle) == 0 &&
            hay[hay.size()-needle.size()-1] == ':') return true;          // end
        return false;
    };

    if (!path.empty()) {
        if (!contains_token(path, add)) path += ":" + add;
    } else {
        path = add;
    }

    // setenv overwrites when third arg is 1
    return setenv("PATH", path.c_str(), 1) == 0;
#endif
}

enum class PathScope { User, Machine }; // HKCU or HKLM (admin needed for Machine)

namespace detail_winpath {
    inline bool read_registry_path(PathScope scope, std::wstring& value, DWORD& type) {
        HKEY root = (scope == PathScope::User) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        const wchar_t* subkey = (scope == PathScope::User)
                                ? L"Environment"
                                : L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

        HKEY h;
        if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;

        DWORD size = 0;
        LONG rc = RegGetValueW(h, nullptr, L"Path",
                               RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                               &type, nullptr, &size);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) { RegCloseKey(h); return false; }

        value.clear();
        if (rc == ERROR_SUCCESS && size) {
            value.resize(size / sizeof(wchar_t));
            rc = RegGetValueW(h, nullptr, L"Path",
                              RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                              &type, &value[0], &size);
            if (rc != ERROR_SUCCESS) { RegCloseKey(h); return false; }
            if (!value.empty() && value.back() == L'\0') value.pop_back();
        }
        RegCloseKey(h);
        return true;
    }

    inline bool write_registry_path(PathScope scope, const std::wstring& value, DWORD type) {
        HKEY root = (scope == PathScope::User) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        const wchar_t* subkey = (scope == PathScope::User)
                                ? L"Environment"
                                : L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

        HKEY h;
        REGSAM access = KEY_SET_VALUE;
        if (RegOpenKeyExW(root, subkey, 0, access, &h) != ERROR_SUCCESS) return false;

        DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        LONG rc = RegSetValueExW(h, L"Path", 0, type ? type : REG_EXPAND_SZ,
                                 reinterpret_cast<const BYTE*>(value.c_str()), bytes);
        RegCloseKey(h);
        if (rc != ERROR_SUCCESS) return false;

        // Notify other apps
        DWORD_PTR res = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            reinterpret_cast<LPARAM>(L"Environment"),
                            SMTO_ABORTIFHUNG, 5000, &res);
        return true;
    }

    inline void trim(std::wstring& s) {
        auto is_ws = [](wchar_t c){ return c==L' '||c==L'\t'||c==L'\r'||c==L'\n'; };
        size_t a=0,b=s.size();
        while (a<b && is_ws(s[a])) ++a;
        while (b>a && is_ws(s[b-1])) --b;
        s = s.substr(a,b-a);
    }

    inline std::wstring unquote(std::wstring s) {
        trim(s);
        if (s.size()>=2 && s.front()==L'"' && s.back()==L'"') {
            s = s.substr(1, s.size()-2);
            trim(s);
        }
        return s;
    }

    inline bool token_equals(const std::wstring& a, const std::wstring& b) {
        // Case-insensitive, ignore trailing backslash differences
        auto norm = [](std::wstring x){
            if (!x.empty() && (x.back()==L'\\' || x.back()==L'/')) x.pop_back();
            for (auto& ch : x) ch = towlower(ch);
            return x;
        };
        return norm(a) == norm(b);
    }
}

// Adds `dir` to persistent PATH at User or Machine scope (returns false on error).
bool add_path_to_windows_PATH_persistent(const std::filesystem::path& dir, PathScope scope = PathScope::User) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
        return false;

    // Read existing PATH from registry
    std::wstring current; DWORD type = 0;
    if (!detail_winpath::read_registry_path(scope, current, type)) return false;

    // Split on ';' and avoid duplicates
    std::wstring to_add = dir.wstring();
    bool already = false;
    size_t start = 0;
    while (start <= current.size()) {
        size_t end = current.find(L';', start);
        std::wstring tok = (end==std::wstring::npos) ? current.substr(start)
                                                     : current.substr(start, end-start);
        start = (end==std::wstring::npos) ? current.size()+1 : end+1;
        tok = detail_winpath::unquote(tok);
        if (tok.empty()) continue;
        if (detail_winpath::token_equals(tok, to_add)) { already = true; break; }
    }
    if (already) {
        // Sync current process PATH anyway so the change is visible immediately here.
        // (Even if it was already present persistently.)
        add_path_to_process_PATH(dir); // see function above; ignore result
        return true;
    }

    // Append
    if (!current.empty() && current.back() != L';') current.push_back(L';');
    current += to_add;

    // Write back and notify
    if (!detail_winpath::write_registry_path(scope, current, type ? type : REG_EXPAND_SZ))
        return false;

    // Also update this process now
    add_path_to_process_PATH(dir);
    return true;
}

#endif //LUAXE_SRC_A_H