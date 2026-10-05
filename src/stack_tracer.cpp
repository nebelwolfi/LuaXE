#include "pch.h"
#include "stack_tracer.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <tchar.h>

#pragma warning(push)
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)

const int CALLSTACK_DEPTH = 48;

// Translate exception code to description
#define CODE_DESCR(code) CodeDescMap::value_type(code, #code)

StackTracer::StackTracer(void)
        :m_dwExceptionCode(0)
{
    // Get machine type
    m_dwMachineType = 0;

    m_dwMachineType = IMAGE_FILE_MACHINE_AMD64;

    // Exception code description
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_ACCESS_VIOLATION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_DATATYPE_MISALIGNMENT));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_BREAKPOINT));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_SINGLE_STEP));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_ARRAY_BOUNDS_EXCEEDED));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_DENORMAL_OPERAND));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_DIVIDE_BY_ZERO));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_INEXACT_RESULT));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_INVALID_OPERATION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_OVERFLOW));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_STACK_CHECK));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_FLT_UNDERFLOW));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_INT_DIVIDE_BY_ZERO));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_INT_OVERFLOW));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_PRIV_INSTRUCTION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_IN_PAGE_ERROR));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_ILLEGAL_INSTRUCTION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_NONCONTINUABLE_EXCEPTION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_STACK_OVERFLOW));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_INVALID_DISPOSITION));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_GUARD_PAGE));
    m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_INVALID_HANDLE));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_GUARD_PAGE_VIOLATION));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_DATATYPE_MISALIGNMENT));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_BREAKPOINT));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_SINGLE_STEP));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_LONGJUMP));
    m_mapCodeDesc.insert(CODE_DESCR(STATUS_UNWIND_CONSOLIDATE));
    m_mapCodeDesc.insert(CODE_DESCR(DBG_EXCEPTION_NOT_HANDLED));
    //m_mapCodeDesc.insert(CODE_DESCR(EXCEPTION_POSSIBLE_DEADLOCK));
    // Any other exception code???
}

StackTracer::~StackTracer(void)
{
}

std::string StackTracer::GetExceptionStackTrace(LPEXCEPTION_POINTERS e)
{
    StackTracer tracer;
    tracer.HandleException(e);

    return tracer.GetExceptionMsg();
}

LONG StackTracer::ExceptionFilter(LPEXCEPTION_POINTERS e)
{
    return crash_report::filter(e);
}

// ---------------------------------------------------------------------------
// crash_report:: the process-wide crash reporter gate (TB-399 item 2).
// ---------------------------------------------------------------------------
namespace crash_report {
namespace detail {
namespace {

    // One report at a time. The HANDLE behind it also wakes a faulting thread
    // when the owner's report finishes; a fault inside the owner itself (in the
    // reporter, or in dbghelp) never waits on it - see gate_depth.
    struct Gate {
        std::mutex mutex;
        std::condition_variable done;
        bool busy = false;
    };
    // Immortal on purpose (allocated once, never destroyed): a fault during
    // static destruction - or from a thread outliving main - must not use a
    // destroyed mutex.
    Gate& gate() {
        static Gate* g = new Gate();
        return *g;
    }

    // Which exception each thread is already inside reporting (nullptr when it
    // is not). A vectored handler runs on the FAULTING thread, so one slot per
    // thread is exact: asecond fault on the same thread means the reporter
    // itself faulted (or the fault interrupted the wait/report below).
    thread_local const EXCEPTION_RECORD* in_report = nullptr;
    // The thread id that currently OWNS the reporter (0 = nobody). Set while
    // the mutex is held and read by HandleException's gate assert: a direct
    // caller walks dbghelp only when it IS the owner.
    std::atomic<DWORD> gate_owner_id{0};
    DWORD gate_owner() { return gate_owner_id.load(std::memory_order_acquire); }
    // Reentrancy through the __except filter on the same thread: crash_report
    // already ran from the vectored handler for this exception, so the filter
    // must not report (or dbghelp-walk) it a second time. The filter owns no
    // vectored reentry of its own, so a plain bool is exact.
    thread_local bool filter_reentered = false;

    // stderr as a raw HANDLE, written with WriteFile. A crashed thread may hold
    // the CRT's stdout/stderr lock (the old code used std::cerr from the faulting
    // thread itself), and locking it again from the handler deadlocks.
    void write_stderr(const char* text) {
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (!err || err == INVALID_HANDLE_VALUE) return;
        size_t left = strlen(text);
        while (left > 0) {
            DWORD wrote = 0;
            if (!WriteFile(err, text, (DWORD)(left > 0x7fffffff ? 0x7fffffff : left), &wrote, nullptr)
                || wrote == 0) {
                return;
            }
            text += wrote;
            left -= wrote;
        }
    }

    void write_line(const char* line) {
        write_stderr(line);
        write_stderr("\n");
    }

    std::string describe(DWORD code, uintptr_t addr) {
        char buf[96];
        sprintf_s(buf, "0x%X at 0x%p", code, (void*)addr);
        return buf;
    }

} // namespace

} // namespace detail

void note(const char* line) {
    detail::write_line(line);
}

DWORD detail::gate_owner() { return detail::gate_owner_id.load(std::memory_order_acquire); }

bool report(LPEXCEPTION_POINTERS e, const char* header) {
    if (!e || !e->ExceptionRecord) return false;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    const uintptr_t addr = (uintptr_t)e->ExceptionRecord->ExceptionAddress;

    // A fault while THIS thread is already reporting one: the reporter (or
    // dbghelp under it) faulted. Never re-enter: print one line and leave.
    // A fault inside the bounded WAIT below lands here too, for the same reason.
    if (detail::in_report) {
        std::string line = std::string(header ? header : "An uncaught exception occurred.")
            + " (while reporting " + detail::describe(detail::in_report->ExceptionCode,
                                              (uintptr_t)detail::in_report->ExceptionAddress) + ")";
        detail::write_line(line.c_str());
        return false;
    }

    detail::Gate& g = detail::gate();
    {
        std::unique_lock lock(g.mutex);
        if (g.busy) {
            // Another thread owns the reporter. Wait for its report to finish
            // (that wakes us), but BOUNDED: the owner may itself be past saving.
            // A fault inside this wait re-enters report() on this same thread
            // and takes the in_report branch above, so no wait can recurse here.
            if (!g.done.wait_for(lock, std::chrono::milliseconds(kWaitForReporterMs),
                                 [&] { return !g.busy; })) {
                std::string line = std::string(header ? header : "An uncaught exception occurred.")
                    + " (another thread is already reporting a crash; waited "
                    + std::to_string(kWaitForReporterMs) + " ms)";
                detail::write_line(line.c_str());
                return false;
            }
            // The owner finished: its report is already on stderr. Say so once
            // and leave dying to the OS - a second full stack would interleave
            // with nothing, but the process is going down anyway.
            std::string line = std::string(header ? header : "An uncaught exception occurred.")
                + " (" + detail::describe(code, addr) + "; stack reported by another thread)";
            detail::write_line(line.c_str());
            return false;
        }
        g.busy = true;
    }

    detail::in_report = e->ExceptionRecord;
    detail::gate_owner_id.store(GetCurrentThreadId(), std::memory_order_release);
    std::string full;
    {
        // The walk itself: one thread at a time, on a tracer nobody else sees.
        // HandleException asserts the gate is held (gate_depth above), so a
        // future direct caller cannot run dbghelp unsynchronized by accident.
        StackTracer tracer;
        tracer.HandleException(e);
        if (header) {
            full += header;
            full += "\n";
        }
        full += tracer.GetExceptionMsg();
    }
    detail::write_stderr(full.c_str());
    if (full.empty() || full.back() != '\n') detail::write_stderr("\n");
    detail::in_report = nullptr;
    detail::gate_owner_id.store(0, std::memory_order_release);

    {
        std::lock_guard lock(g.mutex);
        g.busy = false;
    }
    g.done.notify_all();
    return true;
}

LONG filter(LPEXCEPTION_POINTERS e) {
    // The vectored handler runs FIRST (it is installed with
    // AddVectoredExceptionHandler, which precedes any __except filter on the
    // unwound frames): when it saw this exception it already reported it.
    // Re-reporting from the filter would walk a second stack for the same fault;
    // that is only skipped when the exception record is the one THIS thread is
    // already reporting, or the filter was re-entered on this thread.
    //
    // A DIFFERENT thread's fault reaching this thread's filter (its own vectored
    // pass already ran on ITS thread first) still reports: its owner may have
    // skipped it after the bounded wait, and filter() goes through report(),
    // which serializes again.
    //
    // NOTE: in_report is set only between gate acquisition and release. A fault
    // whose owner already FINISHED (released the gate) reaches the filter with
    // in_report == nullptr and a busy == false gate, so filter() reports it
    // again - once. That is the double-report path the old code had for every
    // main-thread fault (vectored report + __except report): the run-loop call
    // site below now reports through crash_report once instead.
    if (detail::filter_reentered) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    if (e && e->ExceptionRecord && e->ExceptionRecord == detail::in_report) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    detail::filter_reentered = true;
    report(e, "An exception occurred.");
    detail::filter_reentered = false;
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace crash_report


const char* StackTracer::GetExceptionMsg()
{
    // Built fresh for every report into this tracer's own string: no static
    // stream shared between threads or accumulated across reports.
    std::ostringstream out;

    // Exception Code
    CodeDescMap::iterator itc = m_mapCodeDesc.find(m_dwExceptionCode);

    char Code[72];
    sprintf_s(Code, "0x%X", m_dwExceptionCode);

    out << "Exception Code: " << Code << "\n";
    out << "Exception Address: " << std::hex << m_dwExceptionAddress << std::dec << "\n";

    if (itc != m_mapCodeDesc.end())
    {
        out << "Exception: " << itc->second << "\n";
    }

    for (auto&& it : m_vecCallStack)
    {
        out << (it.ModuleName.empty() ? "UnkModule" : it.ModuleName) << " at 0x" << std::hex << it.Address << std::dec;
        if (!it.FunctionName.empty() || it.LineNumber != 0)
            out << " @ " << it.FunctionName;
        if (!it.FileName.empty() || it.LineNumber != 0)
            out << " <> " << it.FileName << " : " << it.LineNumber;
        out << "\n";
    }

    m_message = out.str();

    return m_message.c_str();
}

DWORD StackTracer::GetExceptionCode()
{
    return m_dwExceptionCode;
}

std::vector<FunctionCall> StackTracer::GetExceptionCallStack()
{
    return m_vecCallStack;
}

LONG __stdcall StackTracer::HandleException(LPEXCEPTION_POINTERS e)
{
    m_dwExceptionCode = e->ExceptionRecord->ExceptionCode;
    m_dwExceptionAddress = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    m_vecCallStack.clear();

    // May only run inside crash_report::report (it serializes dbghelp, which is
    // documented as not thread safe). Anyone else reaches here on a path that
    // bypassed the gate: say so on stderr instead of walking unsynchronized.
    if (crash_report::detail::gate_owner() != GetCurrentThreadId()) {
        char buf[160];
        sprintf_s(buf, "crash report reached outside the reporter gate (code 0x%X); skipping stack",
                  m_dwExceptionCode);
        crash_report::note(buf);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const HANDLE hProcess = GetCurrentProcess();

    // Initializes the symbol handler
    if (!SymInitialize(hProcess, NULL, TRUE))
    {
        SymCleanup(hProcess);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Work through the call stack upwards.
    TraceCallStack(e->ContextRecord);

    SymCleanup(hProcess);

    return(EXCEPTION_EXECUTE_HANDLER);
}

// Work through the stack to get the entire call stack
void StackTracer::TraceCallStack(CONTEXT* pContext)
{
    // Initialize stack frame
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(STACKFRAME));

#if defined(_WIN64)
    sf.AddrPC.Offset = pContext->Rip;
    sf.AddrStack.Offset = pContext->Rsp;
    sf.AddrFrame.Offset = pContext->Rbp;
#elif defined(WIN32)
    sf.AddrPC.Offset = pContext->Eip;
    sf.AddrStack.Offset = pContext->Esp;
    sf.AddrFrame.Offset = pContext->Ebp;
#endif
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;

    if (0 == m_dwMachineType)
        return;

    // Walk through the stack frames.
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();
    while (StackWalk64(m_dwMachineType, hProcess, hThread, &sf, pContext, 0, SymFunctionTableAccess64, SymGetModuleBase64, 0))
    {
        if (sf.AddrFrame.Offset <= 0x1ff)
            continue;

        if (sf.AddrFrame.Offset == 0 || m_vecCallStack.size() >= CALLSTACK_DEPTH)
            break;

        // 1. Get function name at the address
        const int nBuffSize = (sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR) + sizeof(ULONG64) - 1) / sizeof(ULONG64);
        ULONG64 symbolBuffer[nBuffSize];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbolBuffer;

        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        FunctionCall curCall;
        curCall.Address = sf.AddrPC.Offset;

        DWORD64 moduleBase = SymGetModuleBase64(hProcess, sf.AddrPC.Offset);
        char ModuleName[MAX_PATH];
        if (moduleBase && GetModuleFileNameA((HINSTANCE)moduleBase, ModuleName, MAX_PATH))
        {
            curCall.ModuleName = FunctionCall::GetFileName(ModuleName);
        }

        DWORD64 dwSymDisplacement = 0;
        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &dwSymDisplacement, pSymbol))
        {
            curCall.FunctionName = std::string(pSymbol->Name);
        }

        //2. get line and file name at the address
        IMAGEHLP_LINE64 lineInfo = { sizeof(IMAGEHLP_LINE64) };
        DWORD dwLineDisplacement = 0;

        if (SymGetLineFromAddr64(hProcess, sf.AddrPC.Offset, &dwLineDisplacement, &lineInfo))
        {
            curCall.FileName = FunctionCall::GetFileName(std::string(lineInfo.FileName));
            curCall.LineNumber = lineInfo.LineNumber;
        }

        // Call stack stored
        m_vecCallStack.push_back(curCall);
    }
}