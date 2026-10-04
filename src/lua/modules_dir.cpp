//
// The modules directory - see modules_dir.h.
//
#include "src/pch.h"
#include "src/lua/modules_dir.h"

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

std::filesystem::path home() {
    return exe_dir() / "modules";
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
