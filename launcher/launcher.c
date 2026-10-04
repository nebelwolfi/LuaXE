//
// lxe-launcher: a few-KB stand-in executable for a .lef application.
//
// Installed as <name>.exe next to lxe.exe and <name>.lef, it runs
//
//     "<dir>\lxe.exe" "<dir>\<name>.lef" <its own arguments, verbatim>
//
// so an app shipped as a .lef still has a command of its own (girl.exe ->
// girl.lef) without carrying a copy of lxe. The modules directory is lxe's
// <dir>\modules - the same folder the launcher sits in.
//
// The layout: <name>.lef sits in the launcher's own folder, and so does
// lxe.exe (luaxe.dev's install.ps1 and LuaHarness' build_lef.lua lay it out);
// when it does not, the first lxe.exe on PATH runs the app instead. <name> is
// the launcher's file name without a trailing ".exe". The app's modules
// directory is always the folder of the lxe.exe that runs it.
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
    if (GetEnvironmentVariableW(L"PATH", list, size + 1) == 0) return NULL;
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
        if (is_file(candidate)) return candidate;
    }
    return NULL;
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

    if (!is_file(lef)) fail(L"no application at", lef, 0);
    if (!is_file(runtime)) {
        wchar_t* found = lxe_on_path(heap);
        if (!found) fail(L"no LuaXE runtime: lxe.exe is neither on PATH nor at", runtime, 0);
        runtime = found;
    }

    // "<runtime>" "<lef>" <tail>
    const wchar_t* tail = arguments_tail(GetCommandLineW());
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

    SetConsoleCtrlHandler(on_console_event, TRUE);

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
    if (!CreateProcessW(runtime, command, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &process)) {
        fail(L"could not start", runtime, GetLastError());
    }
    if (job && !AssignProcessToJobObject(job, process.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }
    g_child = process.hProcess;
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    ExitProcess(code);
}
