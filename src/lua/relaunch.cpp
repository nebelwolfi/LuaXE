//
// env.relaunch - see relaunch.h and src/relaunch_protocol.h.
//
#include "src/pch.h"
#include "src/lua/relaunch.h"
#include "src/relaunch_protocol.h"
#include "src/lua/modules_dir.h"

namespace relaunch {

namespace {

std::wstring g_file;            // the handshake this process was started with
std::string g_nonce;
bool g_requested = false;
std::vector<std::string> g_args;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring env_value(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return {};
    value.resize(length);
    return value;
}

// Removes a variable from every copy of the environment: the Win32 block that
// child processes inherit, and the CRT's wide and narrow ones (getenv). An
// EMPTY value deletes with _(w)putenv_s; a NULL one aborts the process in
// ucrtbase.
void unset(const wchar_t* name) {
    SetEnvironmentVariableW(name, nullptr);
    _wputenv_s(name, L"");
    auto narrow_name = narrow(name);
    _putenv_s(narrow_name.c_str(), "");
    // lxe links its C runtime statically, but the Lua runtime (lua51.dll) uses
    // the shared ucrtbase.dll, which keeps environment copies of its own - and
    // that is the one Lua's os.getenv reads. Only touched when it is already
    // loaded: a copy made later starts from the (now clean) Win32 block.
    if (HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll")) {
        using wput = int(__cdecl*)(const wchar_t*, const wchar_t*);
        using put = int(__cdecl*)(const char*, const char*);
        if (auto f = (wput)GetProcAddress(ucrt, "_wputenv_s")) f(name, L"");
        if (auto f = (put)GetProcAddress(ucrt, "_putenv_s")) f(narrow_name.c_str(), "");
    }
}

std::wstring tail_of(const std::vector<std::string>& args) {
    std::wstring tail;
    for (const auto& arg : args) {
        if (!tail.empty()) tail += L' ';
        tail += quote(widen(arg));
    }
    return tail;
}

unsigned g_nonce_sequence = 0;

std::string new_nonce() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    unsigned long long value = (unsigned long long)counter.QuadPart ^ ((unsigned long long)GetCurrentProcessId() << 32)
        ^ GetTickCount64() ^ ((unsigned long long)++g_nonce_sequence << 48);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", value * 0x9E3779B97F4A7C15ull);
    return buffer;
}

// <nonce>\n<tail> -> the tail, when the file exists and carries `nonce`.
bool read_request(const std::wstring& file, const std::string& nonce, std::wstring* tail) {
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::path(file), ec);
    if (ec || size > LXE_RELAUNCH_MAX_BYTES) return false;
    std::ifstream input(std::filesystem::path(file), std::ios::binary);
    if (!input) return false;
    std::string data((std::istreambuf_iterator<char>(input)), {});
    input.close();
    if (data.size() > LXE_RELAUNCH_MAX_BYTES) return false;
    auto newline = data.find('\n');
    if (newline == std::string::npos || data.substr(0, newline) != nonce) return false;
    *tail = widen(data.substr(newline + 1));
    return true;
}

HANDLE g_current_child = nullptr;

BOOL WINAPI nested_console_event(DWORD event) {
    // Ctrl+C / Ctrl+Break are the program's; a console close waits for it so
    // the system's grace period is the program's, not this waiting parent's.
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) return TRUE;
    if (g_current_child) WaitForSingleObject(g_current_child, INFINITE);
    return TRUE;
}

HANDLE inheritable(DWORD which) {
    HANDLE handle = GetStdHandle(which), copy = nullptr;
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return handle;
    if (DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return copy;
    return handle;
}

// Closes a duplicate inheritable() made (never the original handle).
void close_duplicate(HANDLE copy, DWORD which) {
    if (copy && copy != INVALID_HANDLE_VALUE && copy != GetStdHandle(which)) CloseHandle(copy);
}

// An update swaps files by renaming the old one aside and moving the new one
// in: for a moment the path does not exist. Wait that out (up to ~2 s).
bool wait_for_file(const std::wstring& file) {
    for (int attempt = 0; attempt < 40; attempt++) {
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        Sleep(50);
    }
    return false;
}

// The executable to run next: this one's own path while a file is there (an
// update renames a running lxe.exe aside and puts the new one at the same
// path), else the installed lxe in %LXE_HOME%\bin. Never the other way round: a
// development lxe must not silently relaunch into the installed runtime.
std::wstring next_executable() {
    std::wstring own(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, own.data(), (DWORD)own.size());
    if (length > 0 && length < own.size()) {
        own.resize(length);
        if (wait_for_file(own)) return own;
    } else {
        own.clear();
    }
    auto installed = modules_dir::bin() / "lxe.exe";
    std::error_code ec;
    if (std::filesystem::exists(installed, ec)) {
        std::wcerr << L"lxe: " << (own.empty() ? std::wstring(L"this lxe") : own)
                   << L" is gone; relaunching with the installed " << installed.wstring() << std::endl;
        return installed.wstring();
    }
    return own;
}

[[noreturn]] void run_nested(const std::wstring& prefix, std::wstring tail) {
    SetConsoleCtrlHandler(nested_console_event, TRUE);
    // The burst rule, the same as launcher.c: the relaunches inside the window
    // (this one included) may not exceed LXE_RELAUNCH_MAX_BURST.
    std::vector<ULONGLONG> recent;
    unsigned sequence = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        while (!recent.empty() && now - recent.front() > LXE_RELAUNCH_WINDOW_MS) recent.erase(recent.begin());
        if (recent.size() >= LXE_RELAUNCH_MAX_BURST) {
            std::cerr << "lxe: the program asked to relaunch more than " << LXE_RELAUNCH_MAX_BURST << " times in "
                      << (LXE_RELAUNCH_WINDOW_MS / 1000) << " s; stopping (a relaunch loop)" << std::endl;
            std::exit(1);
        }
        recent.push_back(now);
        auto exe = next_executable();
        std::wstring command = L"\"" + exe + L"\"";
        if (!prefix.empty()) command += L" " + prefix;
        if (!tail.empty()) command += L" " + tail;

        // The handshake for THIS child, in the block it inherits.
        std::error_code ec;
        auto temp = std::filesystem::temp_directory_path(ec);
        auto file = (ec ? modules_dir::exe_dir() : temp)
            / (L"lxe-relaunch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence) + L".txt");
        DeleteFileW(file.c_str());
        auto nonce = new_nonce();
        SetEnvironmentVariableW(LXE_RELAUNCH_FILE_VAR, file.c_str());
        SetEnvironmentVariableW(LXE_RELAUNCH_NONCE_VAR, widen(nonce).c_str());

        // Killing this parent kills the child; what the child starts is not
        // in the job (silent breakaway), exactly like launcher.c.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(job);
                job = nullptr;
            }
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inheritable(STD_INPUT_HANDLE);
        startup.hStdOutput = inheritable(STD_OUTPUT_HANDLE);
        startup.hStdError = inheritable(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        std::wstring mutable_command = command;
        BOOL started = CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        DWORD start_error = GetLastError();
        SetEnvironmentVariableW(LXE_RELAUNCH_FILE_VAR, nullptr);
        SetEnvironmentVariableW(LXE_RELAUNCH_NONCE_VAR, nullptr);
        close_duplicate(startup.hStdInput, STD_INPUT_HANDLE);
        close_duplicate(startup.hStdOutput, STD_OUTPUT_HANDLE);
        close_duplicate(startup.hStdError, STD_ERROR_HANDLE);
        if (!started) {
            std::wcerr << L"lxe: could not relaunch " << exe << L" (Windows error " << start_error << L")" << std::endl;
            if (job) CloseHandle(job);
            std::exit(1);
        }
        if (job && !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
        g_current_child = process.hProcess;
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        std::wstring next_tail;
        bool again = code == LXE_RELAUNCH_EXIT && read_request(file.wstring(), nonce, &next_tail);
        DeleteFileW(file.c_str());
        g_current_child = nullptr;
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        if (!again) std::exit((int)code);
        tail = next_tail;
    }
}

} // namespace

void read_handshake() {
    auto file = env_value(LXE_RELAUNCH_FILE_VAR);
    auto nonce = env_value(LXE_RELAUNCH_NONCE_VAR);
    unset(LXE_RELAUNCH_FILE_VAR);
    unset(LXE_RELAUNCH_NONCE_VAR);
    if (!file.empty() && !nonce.empty()) {
        g_file = file;
        g_nonce = narrow(nonce);
    }
}

const char* mode() { return g_file.empty() ? "nested" : "launcher"; }

// Main thread only (env.relaunch from the program's own state).
void request(const std::vector<std::string>& args) {
    g_args = args;
    g_requested = true;
}

bool requested() { return g_requested; }

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0; ; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { backslashes++; i++; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

void perform(const std::wstring& prefix) {
    std::cout.flush();
    std::cerr.flush();
    auto tail = tail_of(g_args);
    if (!g_file.empty()) {
        std::string data = g_nonce + "\n" + narrow(tail);
        bool written = false;
        {
            std::ofstream out(std::filesystem::path(g_file), std::ios::binary | std::ios::trunc);
            out.write(data.data(), (std::streamsize)data.size());
            out.close();
            written = (bool)out;
        }
        // _Exit, not exit: the state is closed and everything is flushed, and an
        // atexit handler or static destructor must not be able to turn this
        // exit code into another one (the relaunch would be lost silently).
        if (written) {
            std::fflush(nullptr);
            std::_Exit((int)LXE_RELAUNCH_EXIT);
        }
        // The launcher's file cannot be written: run the next one ourselves.
        std::cerr << "lxe: could not hand the relaunch to the launcher; relaunching in place" << std::endl;
    }
    run_nested(prefix, tail);
}

} // namespace relaunch
