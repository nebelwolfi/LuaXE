#include "pch.h"
#include "stack_tracer.h"
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
    return HandleException(e);
}


std::ostringstream StackTracer::m_ostringstream;
std::string StackTracer::last_error;
const char* StackTracer::GetExceptionMsg()
{
    // Exception Code
    CodeDescMap::iterator itc = m_mapCodeDesc.find(m_dwExceptionCode);

    char Code[72];
    sprintf_s(Code, "0x%X", m_dwExceptionCode);

    m_ostringstream << "Exception Code: " << Code << "\n";
    m_ostringstream << "Exception Address: " << std::hex << m_dwExceptionAddress << std::dec << "\n";

    if (itc != m_mapCodeDesc.end())
    {
        m_ostringstream << "Exception: " << itc->second << "\n";
    }

    //m_ostringstream<<"\n------------------------------------------------------------------\n\n";
    //m_ostringstream << std::setfill(' ') << std::setw(sizeof(int)*2);

    for (auto&& it : m_vecCallStack)
    {
        m_ostringstream << (it.ModuleName.empty() ? "UnkModule" : it.ModuleName) << " at 0x" << std::hex << it.Address << std::dec;
        if (!it.FunctionName.empty() || it.LineNumber != 0)
            m_ostringstream << " @ " << it.FunctionName;
        if (!it.FileName.empty() || it.LineNumber != 0)
            m_ostringstream << " <> " << it.FileName << " : " << it.LineNumber;
        m_ostringstream << "\n";
    }

    last_error = m_ostringstream.str();

    return last_error.c_str();
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

    HANDLE hProcess = INVALID_HANDLE_VALUE;

    // Initializes the symbol handler
    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
    {
        SymCleanup(hProcess);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Work through the call stack upwards.
    TraceCallStack(e->ContextRecord);

    // ...
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