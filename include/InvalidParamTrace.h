#pragma once
// SpectroCraft - Invalid parameter tracing (MSVC _STL_VERIFY / CRT checks).
//
// In Debug builds, std::vector operator[] out-of-range accesses call the CRT
// invalid-parameter handler before showing the "Debug Assertion Failed"
// dialog. This hook captures and symbolizes the current stack at that exact
// moment so we can find the offending index access without a debugger.

#ifdef _WIN32

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#undef max
#undef min
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment (lib, "dbghelp.lib")

namespace scInvalidParam
{
    static void printSymbolizedStack()
    {
        const HANDLE proc = GetCurrentProcess();
        // Explicit search path: the PDBs sit next to the Debug exe.
        char exepath[512] = { 0 };
        GetModuleFileNameA (GetModuleHandleA (nullptr), exepath, sizeof (exepath));
        char* slash = std::strrchr (exepath, '\\');
        if (slash != nullptr) * (slash + 1) = 0;
        SymInitialize (proc, exepath, TRUE);
        SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

        void* stack[64];
        const unsigned n = CaptureStackBackTrace (0, 64, stack, nullptr);
        std::fprintf (stderr, "---- invalid-parameter stack (%u frames) ----\n", n);
        for (unsigned i = 0; i < n; ++i)
        {
            const DWORD64 addr = reinterpret_cast<DWORD64> (stack[i]);
            char name[1024] = { 0 };
            SYMBOL_INFO* sym = static_cast<SYMBOL_INFO*> (
                std::calloc (1, sizeof (SYMBOL_INFO) + 512));
            DWORD64 base = 0;
            if (sym != nullptr)
            {
                sym->SizeOfStruct = sizeof (SYMBOL_INFO);
                sym->MaxNameLen = 511;
            }
            if (sym != nullptr && SymFromAddr (proc, addr, &base, sym))
            {
                char lineInfo[256] = "";
                DWORD disp = 0;
                IMAGEHLP_LINE64 li {};
                li.SizeOfStruct = sizeof (li);
                if (SymGetLineFromAddr64 (proc, addr, &disp, &li))
                    std::snprintf (lineInfo, sizeof (lineInfo), "  [%s:%u]",
                                  li.FileName ? li.FileName : "?", li.LineNumber);
                std::fprintf (stderr, "#%u %s%s\n", i, sym->Name, lineInfo);
            }
            else
            {
                std::fprintf (stderr, "#%u 0x%llX\n", i, (unsigned long long) addr);
            }
            if (sym != nullptr) std::free (sym);
        }
        std::fflush (stderr);
    }

    static void handler (const wchar_t* expression, const wchar_t* function,
                         const wchar_t* file, unsigned int line, uintptr_t)
    {
        std::fwprintf (stderr, L"[invalid-parameter] expr=%ls func=%ls file=%ls line=%u\n",
                       expression ? expression : L"?",
                       function ? function : L"?",
                       file ? file : L"?", line);
        printSymbolizedStack();
        // Stop the process right after capturing: no dialog needed.
        TerminateProcess (GetCurrentProcess(), 42);
    }

    inline void install()
    {
        std::setvbuf (stderr, nullptr, _IONBF, 0);
        _set_invalid_parameter_handler (&handler);
    }
}

#endif
