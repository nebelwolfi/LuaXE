//
// The modules directory - see modules_dir.h.
//
#include "src/pch.h"
#include "src/lua/modules_dir.h"
#include <shlobj.h>

namespace modules_dir {

std::filesystem::path exe_dir() {
    // A path longer than MAX_PATH must not silently become the working directory:
    // that would let the cwd choose which runtime and modules get loaded.
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
    if (!length || length == buffer.size()) return std::filesystem::current_path();
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

namespace {

std::filesystem::path environment_path(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return {};
    value.resize(length);
    return std::filesystem::path(value);
}

std::filesystem::path g_app_dir;

} // namespace

std::filesystem::path lxe_home() {
    // LXE_HOME must be absolute: a relative one would resolve against the cwd.
    auto configured = environment_path(L"LXE_HOME");
    if (!configured.empty() && configured.is_absolute()) return configured;
    // The user's REAL profile folder (from the account, not %USERPROFILE%): a
    // process that points USERPROFILE at an isolated home - a test, an app run
    // in a sandbox home - still shares the one store. Its version folders are
    // immutable, so sharing them is safe, and it keeps such a run from
    // downloading every module again into a throwaway home.
    PWSTR known = nullptr;
    std::filesystem::path profile;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &known)) && known)
        profile = std::filesystem::path(known);
    if (known) CoTaskMemFree(known);
    if (profile.empty() || !profile.is_absolute()) profile = environment_path(L"USERPROFILE");
    if (profile.empty() || !profile.is_absolute()) profile = exe_dir(); // no profile: beside lxe
    return profile / ".lxe";
}

std::filesystem::path bin() { return lxe_home() / "bin"; }
std::filesystem::path store() { return lxe_home() / "modules"; }

std::filesystem::path app_dir() { return g_app_dir; }
void set_app_dir(const std::filesystem::path& dir) { g_app_dir = dir; }

namespace { std::filesystem::path g_script_dir; }
void set_script_dir(const std::filesystem::path& dir) { g_script_dir = dir; }

std::vector<std::filesystem::path> source_roots() {
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    auto add = [&](const std::filesystem::path& base) {
        if (base.empty()) return;
        auto root = base / "modules";
        if (!std::filesystem::is_directory(root, ec)) { ec.clear(); return; }
        auto canonical = std::filesystem::weakly_canonical(root, ec);
        if (ec) { ec.clear(); canonical = root; }
        for (const auto& known : roots) {
            if (std::filesystem::equivalent(known, canonical, ec)) { ec.clear(); return; }
            ec.clear();
        }
        roots.push_back(canonical);
    };
    add(std::filesystem::current_path(ec));
    add(g_script_dir);
    add(exe_dir());
    return roots;
}

// ---- versions ----------------------------------------------------------------

namespace {

struct Version {
    long long part[3] = {0, 0, 0};
    int given = 0;         // how many numeric parts were written (1..3)
    std::string pre;       // pre-release, "" for a release
    bool ok = false;
};

Version parse(std::string text) {
    Version v;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text = text.substr(1);
    if (auto plus = text.find('+'); plus != std::string::npos) text = text.substr(0, plus);
    if (auto dash = text.find('-'); dash != std::string::npos) {
        v.pre = text.substr(dash + 1);
        text = text.substr(0, dash);
    }
    size_t at = 0;
    while (v.given < 3) {
        if (at >= text.size() || !std::isdigit((unsigned char)text[at])) return v;
        long long value = 0;
        while (at < text.size() && std::isdigit((unsigned char)text[at])) {
            value = value * 10 + (text[at] - '0');
            if (value > 1000000000LL) return v;
            at++;
        }
        v.part[v.given++] = value;
        if (at == text.size()) break;
        if (text[at] != '.') return v;
        at++;
    }
    v.ok = at == text.size();
    return v;
}

int compare(const Version& a, const Version& b) {
    for (int i = 0; i < 3; i++) {
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    }
    // A release sorts after its pre-releases; pre-releases compare as text.
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    return a.pre < b.pre ? -1 : (a.pre > b.pre ? 1 : 0);
}

Version bump(Version v, int index) {
    // The smallest version above every version that shares v's first parts.
    v.part[index]++;
    for (int i = index + 1; i < 3; i++) v.part[i] = 0;
    v.pre.clear();
    return v;
}

bool satisfies_one(const Version& v, std::string comparator) {
    while (!comparator.empty() && comparator.front() == ' ') comparator.erase(comparator.begin());
    // Any release; a pre-release only satisfies a range that names one.
    if (comparator.empty() || comparator == "*" || comparator == "x" || comparator == "latest") return v.pre.empty();
    std::string op;
    while (!comparator.empty() && std::strchr("<>=^~", comparator.front())) {
        op += comparator.front();
        comparator.erase(comparator.begin());
    }
    Version want = parse(comparator);
    if (!want.ok) return false;
    // A pre-release only satisfies a range that names a pre-release itself.
    if (!v.pre.empty() && want.pre.empty()) return false;
    int c = compare(v, want);
    if (op == ">=") return c >= 0;
    if (op == ">") return c > 0;
    if (op == "<=") return c <= 0;
    if (op == "<") return c < 0;
    if (op == "^") {
        // ^1.2.3 := >=1.2.3 <2.0.0; ^0.2.3 := <0.3.0; ^0.0.3 := <0.0.4
        int index = want.part[0] != 0 ? 0 : (want.part[1] != 0 || want.given < 3 ? 1 : 2);
        if (want.given == 1) index = 0;
        return c >= 0 && compare(v, bump(want, index)) < 0;
    }
    if (op == "~") {
        // ~1.2.3 := >=1.2.3 <1.3.0; ~1 := <2.0.0
        int index = want.given >= 2 ? 1 : 0;
        return c >= 0 && compare(v, bump(want, index)) < 0;
    }
    // "" or "=": exact for a full version, a prefix match for a partial one.
    if (want.given == 3) return c == 0;
    return c >= 0 && compare(v, bump(want, want.given - 1)) < 0;
}

} // namespace

bool is_version(const std::string& text) {
    if (text.empty() || !std::isdigit((unsigned char)text[0])) return false;
    auto v = parse(text);
    return v.ok && v.given == 3;
}

int compare_versions(const std::string& left, const std::string& right) {
    // Something that is not a version sorts below every version.
    auto a = parse(left), b = parse(right);
    if (a.ok != b.ok) return a.ok ? 1 : -1;
    return compare(a, b);
}

bool satisfies(const std::string& version, const std::string& range) {
    Version v = parse(version);
    if (!v.ok) return false;
    size_t start = 0;
    while (true) {
        size_t bar = range.find("||", start);
        std::string alternative = range.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        // every space-separated comparator of the alternative must hold
        bool all = true;
        size_t at = 0;
        bool any = false;
        while (at <= alternative.size()) {
            size_t space = alternative.find(' ', at);
            std::string token = alternative.substr(at, space == std::string::npos ? std::string::npos : space - at);
            at = space == std::string::npos ? alternative.size() + 1 : space + 1;
            if (token.empty()) continue;
            // ">= 1.0.0": an operator alone joins the next token
            if (token.find_first_not_of("<>=^~") == std::string::npos && at <= alternative.size()) {
                size_t next = alternative.find(' ', at);
                token += alternative.substr(at, next == std::string::npos ? std::string::npos : next - at);
                at = next == std::string::npos ? alternative.size() + 1 : next + 1;
            }
            any = true;
            if (!satisfies_one(v, token)) { all = false; break; }
        }
        if (all && (any || v.pre.empty())) return true;
        if (bar == std::string::npos) return false;
        start = bar + 2;
    }
}

std::vector<std::string> installed_versions(const std::string& name) {
    std::vector<std::string> versions;
    std::error_code ec;
    auto dir = store() / name;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) { ec.clear(); continue; }
        auto version = it->path().filename().string();
        if (!is_version(version)) continue;
        // a version folder that holds nothing (an install that died) is no install
        std::filesystem::directory_iterator inner(it->path(), ec);
        if (ec || inner == std::filesystem::directory_iterator()) { ec.clear(); continue; }
        versions.push_back(version);
    }
    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        return compare_versions(a, b) > 0;
    });
    return versions;
}

std::filesystem::path best_installed(const std::string& name, const std::string& range) {
    for (const auto& version : installed_versions(name)) {
        if (satisfies(version, range)) return store() / name / version;
    }
    return {};
}

bool same_contents(const std::filesystem::path& path, const std::string& data) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size != data.size()) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string buffer(65536, '\0');
    size_t offset = 0;
    while (offset < data.size()) {
        input.read(buffer.data(), (std::streamsize)std::min(buffer.size(), data.size() - offset));
        auto read = (size_t)input.gcount();
        if (read == 0 || std::memcmp(buffer.data(), data.data() + offset, read) != 0) return false;
        offset += read;
    }
    return true;
}

namespace {

std::filesystem::path temp_beside(const std::filesystem::path& target) {
    static std::atomic<unsigned> counter{0};
    auto temp = target;
    temp += "." + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".tmp";
    return temp;
}

std::string windows_error(DWORD code) {
    return "Windows error " + std::to_string(code);
}

} // namespace

void sweep_aside(const std::filesystem::path& target) {
    std::error_code ec;
    auto dir = target.parent_path();
    auto prefix = target.filename().wstring() + L".old-";
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        auto name = it->path().filename().wstring();
        if (name.starts_with(prefix)) DeleteFileW(it->path().c_str()); // fails while mapped: fine
    }
}

bool replace_with(const std::filesystem::path& target, const std::filesystem::path& source, std::string* error) {
    // Every caller (unpack and install alike) cleans up what earlier swaps left.
    sweep_aside(target);
    if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DWORD first = GetLastError();
    // A DLL some process has loaded cannot be replaced, but it can be renamed:
    // move it aside (that process keeps its mapping) and put the new file in.
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        // A free aside name: a leftover from a crashed run under a recycled pid
        // must not turn into a failed swap.
        static std::atomic<unsigned> counter{0};
        std::filesystem::path aside;
        for (int attempt = 0; attempt < 64; attempt++) {
            aside = target;
            aside += ".old-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
                + "-" + std::to_string(counter++);
            if (!std::filesystem::exists(aside, ec)) break;
        }
        if (!MoveFileExW(target.c_str(), aside.c_str(), 0)) {
            if (error) *error = "could not replace " + target.string() + " (" + windows_error(first)
                + "), nor move it aside (" + windows_error(GetLastError()) + ")";
            return false;
        }
        {
            if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            DWORD second = GetLastError();
            // Another process may have won the race and put its copy there.
            if (!std::filesystem::exists(target, ec)) MoveFileExW(aside.c_str(), target.c_str(), 0);
            if (error) *error = "could not move " + source.string() + " to " + target.string() + " (" + windows_error(second) + ")";
            return false;
        }
    }
    if (error) *error = "could not replace " + target.string() + " (" + windows_error(first) + ")";
    return false;
}

bool write_atomically(const std::filesystem::path& target, const std::string& data, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create " + target.parent_path().string() + " (" + ec.message() + ")";
        return false;
    }
    sweep_aside(target); // also when nothing needs writing: an earlier swap's leftover
    if (same_contents(target, data)) return true;
    auto temp = temp_beside(target);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), (std::streamsize)data.size());
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            if (error) *error = "could not write " + temp.string();
            return false;
        }
    }
    // Two processes unpacking the same payload race here; whichever lands
    // second finds the right bytes already in place.
    if (same_contents(target, data)) {
        std::filesystem::remove(temp, ec);
        return true;
    }
    std::string why;
    if (replace_with(target, temp, &why)) return true;
    std::filesystem::remove(temp, ec);
    if (same_contents(target, data)) return true;
    if (error) *error = why;
    return false;
}

} // namespace modules_dir
