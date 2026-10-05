#pragma once

#include <map>
#include <vector>
#include <string>

#include <Windows.h>

struct FunctionCall
{
    DWORD64 Address;
    std::string ModuleName;
    std::string FunctionName;
    std::string FileName;
    int         LineNumber;

public:
    FunctionCall() :
            Address(0),
            ModuleName(""),
            FunctionName(""),
            FileName(""),
            LineNumber(0)
    {
    }

public:
    static std::string GetFileName(const std::string& fullpath)
    {
        size_t index = fullpath.find_last_of('\\');
        if (index == std::string::npos)
        {
            return fullpath;
        }

        return fullpath.substr(index + 1);
    }
};

class StackTracer
{
private:
    // Per report, never shared (TB-399 item 2): these used to be STATIC, so two
    // threads building a report at once appended into one stream, and every
    // report also repeated every earlier one.
    std::string m_message;
public:
    static std::string GetExceptionStackTrace(LPEXCEPTION_POINTERS e);

    StackTracer(void);

    // Always return EXCEPTION_EXECUTE_HANDLER after getting the call stack
    LONG ExceptionFilter(LPEXCEPTION_POINTERS e);

    // return the exception message along with call stacks
    const char* GetExceptionMsg();

    // Return exception code and call stack data structure so that 
    // user could customize their own message format
    DWORD GetExceptionCode();
    std::vector<FunctionCall> GetExceptionCallStack();

    ~StackTracer(void);

    // The main function to handle exception
    LONG __stdcall HandleException(LPEXCEPTION_POINTERS e);

    // Work through the stack upwards to get the entire call stack
    void TraceCallStack(CONTEXT* pContext);

    DWORD m_dwExceptionCode;
    uintptr_t m_dwExceptionAddress;

    std::vector<FunctionCall> m_vecCallStack;

    typedef std::map<DWORD, const char*> CodeDescMap;
    CodeDescMap m_mapCodeDesc;

    DWORD m_dwMachineType; // Machine type matters when trace the call stack (StackWalk64)

};

// TB-399 item 2: the ONE way lxe reports a crash.
//
// dbghelp is single-threaded (SymInitialize, StackWalk64, SymFromAddr and
// SymCleanup are documented as not thread safe), and lxe's vectored handler runs
// on EVERY faulting thread. Before this gate, two threads faulting at once both
// ran SymInitialize/StackWalk64/SymCleanup on one global tracer: the process
// hung inside the REPORTER (measured: 8 workers faulting together, see
// tests\tb399_crash_report.ps1) instead of printing a stack and dying.
namespace crash_report {
    /// How long a faulting thread waits for another thread's report to finish
    /// before it gives up on a stack of its own (it still prints one line).
    constexpr DWORD kWaitForReporterMs = 5000;

    /// Reports `e` with a full stack, serialized process-wide:
    ///  - one thread at a time walks a stack; another faulting thread waits at
    ///    most kWaitForReporterMs for it, then prints a single line and returns;
    ///  - a fault on a thread that is ALREADY inside report() - in the reporter,
    ///    in dbghelp, or in the wait - never re-enters it (no recursion, no
    ///    self-deadlock): it returns at once;
    ///  - the same exception is reported once, even when both the vectored
    ///    handler and the run loop's __except filter see it.
    /// Output goes straight to the stderr HANDLE (WriteFile): no CRT stream lock
    /// a crashed thread may still hold. Returns true when this call printed a
    /// full report.
    bool report(LPEXCEPTION_POINTERS e, const char* header);

    /// For `__except (crash_report::filter(GetExceptionInformation()))`:
    /// reports (once) and returns EXCEPTION_EXECUTE_HANDLER.
    LONG filter(LPEXCEPTION_POINTERS e);

    namespace detail {
        /// The thread id that currently owns the reporter's dbghelp walk
        /// (0 = nobody). HandleException walks only for the owner; anyone else
        /// prints a note instead of running unsynchronized.
        DWORD gate_owner();
    }

    // Writes one line to the raw stderr handle (WriteFile, no CRT lock).
    void note(const char* line);
}