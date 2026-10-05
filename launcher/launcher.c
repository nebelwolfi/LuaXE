//
// lxe-launcher: a few-KB stand-in executable for a .lef application.
//
// Installed as <name>.exe next to lxe.exe and <name>.lef, it runs
//
//     "<dir>\lxe.exe" "<dir>\<name>.lef" <its own arguments, verbatim>
//
// so an app shipped as a .lef still has a command of its own (girl.exe ->
// girl.lef) without carrying a copy of lxe. What the app bundles unpacks next
// to the .lef (<dir>\modules\<name>\<version>\), what it installs goes to lxe's
// store (%USERPROFILE%\.lxe\modules\<name>\<version>\).
//
// The layout: <name>.lef sits in the launcher's own folder. lxe.exe is looked
// for, in order: beside the launcher, in lxe's own install
// (%LXE_HOME%\bin, default %USERPROFILE%\.lxe\bin - where luaxe.dev's
// install.ps1 puts it), then the first lxe.exe on PATH. <name> is the
// launcher's file name without a trailing ".exe".
//
// What it guarantees:
//   - the arguments reach the app byte for byte: the tail of its own command
//     line is appended unparsed, so no quoting is redone (lxe dispatches on
//     argv[1] being a .lef BEFORE it reads any option, so an app argument can
//     never be taken for an lxe command);
//   - the console, the standard handles and Ctrl+C belong to the app: the
//     launcher hands its std handles on explicitly, ignores Ctrl+C/Ctrl+Break
//     and, when the console is closed, waits for the app instead of dying
//     first, so the app gets the system's whole close grace period;
//   - the app's exit code is the launcher's exit code;
//   - an app that asks to be relaunched (env.relaunch, src/relaunch_protocol.h)
//     is started again in this same console, with the arguments it handed over
//     and with lxe.exe and <name>.lef resolved afresh (an update may have just
//     replaced either);
//   - killing the launcher kills the app (a job object with kill-on-close),
//     while processes the app starts are NOT in that job (silent breakaway),
//     so its detached children survive exactly as they would without it. This
//     part is best effort: a launcher that is itself in a job which refuses a
//     nested one just runs the app without it.
//
// Built without the C runtime (/NODEFAULTLIB, entry point `entry`): kernel32
// only. No stack buffer may exceed 4 KB: that would need __chkstk.
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/relaunch_protocol.h"

// The compiler may emit calls to memset/memcpy for struct initialisation; with
// no CRT linked they have to exist here. VOLATILE byte loops: clang recognises
// any plain copy/fill loop (and even __stosb) as memset/memcpy and would turn
// these very functions into calls to themselves.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(memset)
#pragma function(memcpy)
#endif
void* __cdecl memset(void* dst, int value, size_t size) {
    volatile unsigned char* out = (volatile unsigned char*)dst;
    while (size--) *out++ = (unsigned char)value;
    return dst;
}
void* __cdecl memcpy(void* dst, const void* src, size_t size) {
    volatile unsigned char* out = (volatile unsigned char*)dst;
    const unsigned char* in = (const unsigned char*)src;
    while (size--) *out++ = *in++;
    return dst;
}

static HANDLE g_child = NULL;

static size_t wlen(const wchar_t* text) {
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

static wchar_t* append(wchar_t* out, const wchar_t* text, size_t length) {
    memcpy(out, text, length * sizeof(wchar_t));
    return out + length;
}

static wchar_t lower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

static void say(const wchar_t* text) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err == NULL || err == INVALID_HANDLE_VALUE) return;
    if (!WriteConsoleW(err, text, (DWORD)wlen(text), &written, NULL)) {
        // Redirected: UTF-8 bytes, and no CR (a file gets plain "\n" lines).
        char buffer[2048];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, text, (int)wlen(text), buffer, sizeof(buffer), NULL, NULL);
        if (bytes > 0) WriteFile(err, buffer, (DWORD)bytes, &written, NULL);
    }
}

static void say_number(DWORD value) {
    wchar_t digits[16];
    int at = 15;
    digits[at] = 0;
    do { digits[--at] = (wchar_t)(L'0' + value % 10); value /= 10; } while (value && at > 0);
    say(digits + at);
}

// error: a GetLastError() code to show, or 0 for none.
static void fail(const wchar_t* what, const wchar_t* path, DWORD error) {
    say(L"lxe-launcher: ");
    say(what);
    if (path) { say(L" "); say(path); }
    if (error) { say(L" (Windows error "); say_number(error); say(L")"); }
    say(L"\n");
    ExitProcess(1);
}

// The command line after the program name, the way Windows splits it: blanks
// before the name are skipped, and the name ends at the closing quote when it
// starts with one (program names cannot contain quotes), else at the first
// blank. A parent that leaves the program name out of the command line makes
// its first argument the "name" - that is how Windows itself would read it.
static const wchar_t* arguments_tail(const wchar_t* line) {
    while (*line == L' ' || *line == L'\t') line++;
    if (*line == L'"') {
        line++;
        while (*line && *line != L'"') line++;
        if (*line == L'"') line++;
    } else {
        while (*line && *line != L' ' && *line != L'\t') line++;
    }
    while (*line == L' ' || *line == L'\t') line++;
    return line;
}

// Ctrl+C / Ctrl+Break are the app's. Closing the console, logging off or
// shutting down: wait for the app, so the system's grace period is the app's
// and the launcher (whose exit would kill the app through the job) is not the
// first to go.
static BOOL WINAPI on_console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) return TRUE;
    if (g_child) WaitForSingleObject(g_child, INFINITE);
    return TRUE;
}

// An inheritable duplicate of a standard handle, or the value itself when it
// is not a handle at all (no console, a GUI parent).
static HANDLE inheritable(DWORD which) {
    HANDLE handle = GetStdHandle(which), copy = NULL;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return handle;
    if (DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return copy;
    return handle;
}

static int is_file(const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// is_file, waiting out an update's rename-aside-then-move-in (up to ~2 s).
static int wait_for_file(const wchar_t* path) {
    for (int attempt = 0; attempt < 40; attempt++) {
        if (is_file(path)) return 1;
        Sleep(50);
    }
    return 0;
}

// The first lxe.exe on PATH, or NULL. Walked here rather than with
// SearchPathW, which also tries the CURRENT directory: for an app the cwd is
// the user's workspace, and an lxe.exe planted there must never be the one
// that runs. For the same reason a relative PATH entry (resolved against the
// cwd) is skipped. Quotes around an entry are allowed, as cmd allows them.
static wchar_t* lxe_on_path(HANDLE heap) {
    DWORD size = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (size == 0) return NULL;
    wchar_t* list = (wchar_t*)HeapAlloc(heap, 0, (size + 1) * sizeof(wchar_t));
    wchar_t* candidate = (wchar_t*)HeapAlloc(heap, 0, (size + 16) * sizeof(wchar_t));
    if (!list || !candidate) return NULL;
    if (GetEnvironmentVariableW(L"PATH", list, size + 1) == 0) { HeapFree(heap, 0, list); HeapFree(heap, 0, candidate); return NULL; }
    const wchar_t* at = list;
    while (*at) {
        const wchar_t* start = at;
        while (*at && *at != L';') at++;
        const wchar_t* end = at;
        if (*at == L';') at++;
        while (start < end && (*start == L' ' || *start == L'\t' || *start == L'"')) start++;
        while (end > start && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'"')) end--;
        while (end > start && (end[-1] == L'\\' || end[-1] == L'/')) end--;
        if (end <= start) continue;
        // Absolute only: "C:\..." or a UNC "\\server\share\...".
        int absolute = (end - start >= 3 && start[1] == L':' && (start[2] == L'\\' || start[2] == L'/'))
            || (end - start >= 2 && start[0] == L'\\' && start[1] == L'\\');
        if (!absolute) continue;
        wchar_t* cursor = append(candidate, start, (size_t)(end - start));
        cursor = append(cursor, L"\\lxe.exe", 8);
        *cursor = 0;
        if (is_file(candidate)) { HeapFree(heap, 0, list); return candidate; }
    }
    HeapFree(heap, 0, list);
    HeapFree(heap, 0, candidate);
    return NULL;
}

// <LXE_HOME or USERPROFILE\.lxe>\bin\lxe.exe when it exists, else NULL.
static wchar_t* lxe_in_home(HANDLE heap) {
    const wchar_t* names[2] = { L"LXE_HOME", L"USERPROFILE" };
    for (int i = 0; i < 2; i++) {
        DWORD size = GetEnvironmentVariableW(names[i], NULL, 0);
        if (size == 0) continue;
        wchar_t* path = (wchar_t*)HeapAlloc(heap, 0, (size + 32) * sizeof(wchar_t));
        if (!path) return NULL;
        DWORD length = GetEnvironmentVariableW(names[i], path, size);
        if (length == 0 || length >= size) continue;
        // absolute only: a relative value would resolve against the cwd
        int absolute = (length >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
            || (length >= 2 && path[0] == L'\\' && path[1] == L'\\');
        if (!absolute) continue;
        while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) length--;
        wchar_t* cursor = path + length;
        if (i == 1) cursor = append(cursor, L"\\.lxe", 5);
        cursor = append(cursor, L"\\bin\\lxe.exe", 12);
        *cursor = 0;
        if (is_file(path)) return path;
        HeapFree(heap, 0, path);
        // LXE_HOME set but without lxe: still try the profile default
    }
    return NULL;
}

static wchar_t* append_number(wchar_t* out, DWORD value) {
    wchar_t digits[12];
    int at = 11;
    digits[at] = 0;
    do { digits[--at] = (wchar_t)(L'0' + value % 10); value /= 10; } while (value && at > 0);
    return append(out, digits + at, (size_t)(11 - at));
}

// 16 hex digits from the clocks, the pid and the sequence: not a secret, a name
// for one child, so a stale or foreign handshake file is never mistaken for it.
static void make_nonce(wchar_t* out, unsigned sequence) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    unsigned long long value = (unsigned long long)counter.QuadPart ^ GetTickCount64()
        ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ ((unsigned long long)sequence << 48);
    value *= 0x9E3779B97F4A7C15ull;
    for (int i = 15; i >= 0; i--) {
        unsigned nibble = (unsigned)(value & 15);
        out[i] = (wchar_t)(nibble < 10 ? L'0' + nibble : L'a' + nibble - 10);
        value >>= 4;
    }
    out[16] = 0;
}

// The tail a relaunching child handed over (<nonce>\n<tail>, UTF-8), or NULL
// when there is no such file, it is too large, or its nonce is not this child's.
static wchar_t* read_relaunch(HANDLE heap, const wchar_t* file, const wchar_t* nonce) {
    HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(handle, NULL);
    if (size == INVALID_FILE_SIZE || size > LXE_RELAUNCH_MAX_BYTES) { CloseHandle(handle); return NULL; }
    char* data = (char*)HeapAlloc(heap, 0, size + 1);
    DWORD read = 0;
    if (!data || !ReadFile(handle, data, size, &read, NULL) || read != size) {
        if (data) HeapFree(heap, 0, data);
        CloseHandle(handle);
        return NULL;
    }
    CloseHandle(handle);
    data[size] = 0;
    DWORD i = 0;
    while (nonce[i] && i < size && (wchar_t)(unsigned char)data[i] == nonce[i]) i++;
    if (nonce[i] != 0 || i >= size || data[i] != '\n') { HeapFree(heap, 0, data); return NULL; }
    const char* rest = data + i + 1;
    int bytes = (int)(size - i - 1);
    int chars = bytes ? MultiByteToWideChar(CP_UTF8, 0, rest, bytes, NULL, 0) : 0;
    wchar_t* tail = (wchar_t*)HeapAlloc(heap, 0, ((size_t)chars + 1) * sizeof(wchar_t));
    if (!tail) { HeapFree(heap, 0, data); return NULL; }
    if (chars) MultiByteToWideChar(CP_UTF8, 0, rest, bytes, tail, chars);
    tail[chars] = 0;
    HeapFree(heap, 0, data);
    return tail;
}

// Closes an inheritable duplicate inheritable() made (never the original).
static void close_duplicate(HANDLE copy, DWORD which) {
    HANDLE original = GetStdHandle(which);
    if (copy && copy != INVALID_HANDLE_VALUE && copy != original) CloseHandle(copy);
}

void __stdcall entry(void) {
    HANDLE heap = GetProcessHeap();
    DWORD capacity = 32768;
    wchar_t* self = (wchar_t*)HeapAlloc(heap, 0, capacity * sizeof(wchar_t));
    if (!self) fail(L"out of memory", NULL, 0);
    DWORD length = GetModuleFileNameW(NULL, self, capacity);
    if (length == 0 || length >= capacity) fail(L"cannot name its own executable", NULL, GetLastError());

    // <dir>\  and  <name> = the file name without a trailing ".exe".
    size_t slash = length;
    while (slash > 0 && self[slash - 1] != L'\\' && self[slash - 1] != L'/') slash--;
    size_t stem_end = length;
    if (length - slash > 4 && self[length - 4] == L'.' && lower(self[length - 3]) == L'e'
        && lower(self[length - 2]) == L'x' && lower(self[length - 1]) == L'e') {
        stem_end = length - 4;
    }

    // "<dir>\lxe.exe" and "<dir>\<name>.lef"
    wchar_t* runtime = (wchar_t*)HeapAlloc(heap, 0, (slash + 16) * sizeof(wchar_t));
    wchar_t* lef = (wchar_t*)HeapAlloc(heap, 0, (stem_end + 8) * sizeof(wchar_t));
    if (!runtime || !lef) fail(L"out of memory", NULL, 0);
    wchar_t* cursor = append(runtime, self, slash);
    cursor = append(cursor, L"lxe.exe", 7);
    *cursor = 0;
    cursor = append(lef, self, stem_end);
    cursor = append(cursor, L".lef", 4);
    *cursor = 0;

    // `beside` keeps "<dir>\lxe.exe": it is re-resolved on every start, since
    // an update may put an lxe there (or take it away) while this one waits.
    wchar_t* beside = runtime;
    const wchar_t* tail = arguments_tail(GetCommandLineW());
    wchar_t* relaunched_tail = NULL;

    SetConsoleCtrlHandler(on_console_event, TRUE);

    // Relaunch bookkeeping: the handshake file, and the burst guard.
    wchar_t* handshake = (wchar_t*)HeapAlloc(heap, 0, (MAX_PATH + 64) * sizeof(wchar_t));
    if (!handshake) fail(L"out of memory", NULL, 0);
    ULONGLONG recent[LXE_RELAUNCH_MAX_BURST + 1];
    int recent_count = 0;
    unsigned sequence = 0;

    wchar_t* found = NULL;
    for (;;) {
        // An update swaps files by renaming the old one aside and moving the new
        // one in: for a moment the path does not exist. Wait that out (~2 s).
        if (!wait_for_file(lef)) fail(L"no application at", lef, 0);
        if (found) { HeapFree(heap, 0, found); found = NULL; }
        // lxe.exe beside the launcher, else lxe's install, else PATH - retried
        // for ~2 s, since the one that is normally found may be mid-swap.
        runtime = NULL;
        for (int attempt = 0; attempt < 40 && !runtime; attempt++) {
            if (is_file(beside)) {
                runtime = beside;
            } else {
                found = lxe_in_home(heap);
                if (!found) found = lxe_on_path(heap);
                if (found) runtime = found;
                else Sleep(50);
            }
        }
        if (!runtime) fail(L"no LuaXE runtime: lxe.exe is not in %USERPROFILE%\\.lxe\\bin, on PATH or at", beside, 0);

        // "<runtime>" "<lef>" <tail>
        size_t size = wlen(runtime) + wlen(lef) + wlen(tail) + 8;
        wchar_t* command = (wchar_t*)HeapAlloc(heap, 0, size * sizeof(wchar_t));
        if (!command) fail(L"out of memory", NULL, 0);
        cursor = command;
        cursor = append(cursor, L"\"", 1);
        cursor = append(cursor, runtime, wlen(runtime));
        cursor = append(cursor, L"\" \"", 3);
        cursor = append(cursor, lef, wlen(lef));
        cursor = append(cursor, L"\"", 1);
        if (*tail) {
            cursor = append(cursor, L" ", 1);
            cursor = append(cursor, tail, wlen(tail));
        }
        *cursor = 0;

        // The handshake for THIS child: a fresh file and nonce. The child takes
        // both out of its own environment, so nothing it starts sees them.
        wchar_t nonce[20];
        make_nonce(nonce, sequence);
        DWORD temp_length = GetTempPathW(MAX_PATH, handshake);
        if (temp_length == 0 || temp_length > MAX_PATH) {
            temp_length = (DWORD)slash;
            append(handshake, self, slash);
        }
        cursor = handshake + temp_length;
        cursor = append(cursor, L"lxe-relaunch-", 13);
        cursor = append_number(cursor, GetCurrentProcessId());
        cursor = append(cursor, L"-", 1);
        cursor = append_number(cursor, ++sequence);
        cursor = append(cursor, L".txt", 4);
        *cursor = 0;
        DeleteFileW(handshake);
        SetEnvironmentVariableW(LXE_RELAUNCH_FILE_VAR, handshake);
        SetEnvironmentVariableW(LXE_RELAUNCH_NONCE_VAR, nonce);

        // The app dies with the launcher; whatever the app starts does not.
        HANDLE job = CreateJobObjectW(NULL, NULL);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            memset(&limits, 0, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                CloseHandle(job);
                job = NULL;
            }
        }

        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        memset(&startup, 0, sizeof(startup));
        memset(&process, 0, sizeof(process));
        startup.cb = sizeof(startup);
        // The std handles are handed on EXPLICITLY, as inheritable duplicates: a
        // parent that redirected them with non-inheritable handles would otherwise
        // leave the app with handle values that mean nothing in its process.
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inheritable(STD_INPUT_HANDLE);
        startup.hStdOutput = inheritable(STD_OUTPUT_HANDLE);
        startup.hStdError = inheritable(STD_ERROR_HANDLE);
        BOOL started = CreateProcessW(runtime, command, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &process);
        DWORD start_error = GetLastError();
        SetEnvironmentVariableW(LXE_RELAUNCH_FILE_VAR, NULL);
        SetEnvironmentVariableW(LXE_RELAUNCH_NONCE_VAR, NULL);
        close_duplicate(startup.hStdInput, STD_INPUT_HANDLE);
        close_duplicate(startup.hStdOutput, STD_OUTPUT_HANDLE);
        close_duplicate(startup.hStdError, STD_ERROR_HANDLE);
        HeapFree(heap, 0, command);
        if (!started) fail(L"could not start", runtime, start_error);
        if (job && !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = NULL;
        }
        // Console close waits on the CURRENT child.
        g_child = process.hProcess;
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        wchar_t* next = NULL;
        if (code == LXE_RELAUNCH_EXIT) next = read_relaunch(heap, handshake, nonce);
        DeleteFileW(handshake);
        g_child = NULL;
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        if (!next) ExitProcess(code);

        // A relaunch. More than the burst allows inside the window is a loop.
        ULONGLONG now = GetTickCount64();
        int kept = 0;
        for (int i = 0; i < recent_count; i++) {
            if (now - recent[i] <= LXE_RELAUNCH_WINDOW_MS) recent[kept++] = recent[i];
        }
        recent_count = kept;
        if (recent_count >= LXE_RELAUNCH_MAX_BURST) {
            fail(L"the application asked to relaunch too often; stopping", lef, 0);
        }
        recent[recent_count++] = now;
        if (relaunched_tail) HeapFree(heap, 0, relaunched_tail);
        relaunched_tail = next;
        tail = relaunched_tail;
    }
}
