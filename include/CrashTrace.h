#pragma once
// SpectroCraft - Minimal Windows crash tracing (SEH + dbghelp).
// Prints a call stack to stderr when the process crashes, so we can diagnose
// segfaults in this environment without a full debugger.
//
// IMPORTANT: on machines with the NVIDIA virtual GPU driver used here, that
// driver periodically raises STATUS_FATAL_APP_EXIT (0x406D1388) on one of its
// own threads and handles it with its own SEH frame. A vectored handler must
// NOT intercept-and-terminate those events (doing so kills an otherwise
// healthy process); seHandler passes them through instead.

#ifdef _WIN32

// JUCE only includes the native Windows headers from juce_core.cpp, so this
// may be the first <windows.h> include in a translation unit. Guard against
// windef.h's min/max macros leaking into later code (std::max etc.).
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#undef max
#undef min
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment (lib, "dbghelp.lib")
#pragma comment (lib, "psapi.lib")

namespace scCrash
{
    struct ModuleInfo
    {
        char name[128] = { 0 };
        DWORD64 base = 0;
        size_t size = 0;
    };

    static HMODULE g_modules[128] = { 0 };
    static ModuleInfo g_mods[128];

    static void collectModules()
    {
        if (g_modules[0])
            return;
        DWORD need = 0;
        EnumProcessModules (GetCurrentProcess(), nullptr, 0, &need);
        const int n = (int) std::min<size_t> (128, need / sizeof (HMODULE));
        if (EnumProcessModules (GetCurrentProcess(), g_modules, need, &need))
        {
            for (int i = 0; i < n; ++i)
            {
                char path[512] = { 0 };
                GetModuleFileNameA (g_modules[i], path, sizeof (path));
                const char* slash = std::strrchr (path, '\\');
                std::snprintf (g_mods[i].name, sizeof (g_mods[i].name), "%s",
                               slash ? slash + 1 : path);
                g_mods[i].base = (DWORD64) g_modules[i];

                MODULEINFO mi {};
                if (GetModuleInformation (GetCurrentProcess(), g_modules[i], &mi, sizeof (mi)))
                    g_mods[i].size = mi.SizeOfImage;
            }
        }
    }

    static const ModuleInfo* findModule (DWORD64 addr)
    {
        for (int i = 0; i < 128 && g_modules[i] != nullptr; ++i)
        {
            if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size)
                return &g_mods[i];
        }
        return nullptr;
    }

    static void printAddr (const char* label, DWORD64 addr)
    {
        const ModuleInfo* m = findModule (addr);
        if (m != nullptr)
            std::fprintf (stderr, "%s %s + 0x%llX\n", label, m->name,
                          (unsigned long long) (addr - m->base));
        else
            std::fprintf (stderr, "%s 0x%llX (no module)\n", label,
                          (unsigned long long) addr);
    }

    // While a thread is suspended, read its stack from Rsp upward and print
    // every value that lands inside a loaded module (a poor-man's call chain).
    static void scanThreadStack (const CONTEXT& c)
    {
        uint8_t buf [65536];
        SIZE_T got = 0;
        if (!ReadProcessMemory (GetCurrentProcess(), (LPCVOID) c.Rsp, buf,
                                sizeof (buf), &got))
            return;

        int printed = 0;
        DWORD64 last = 0;
        for (SIZE_T i = 0; i + 8 <= got && printed < 32; i += 8)
        {
            DWORD64 v = 0;
            std::memcpy (&v, buf + i, 8);
            const ModuleInfo* m = findModule (v);
            if (m == nullptr || v == last)
                continue;
            last = v;
            std::fprintf (stderr, "    [%3zu] %s + 0x%llX\n", i / 8, m->name,
                          (unsigned long long) (v - m->base));
            ++printed;
        }
    }

    static void dumpOtherThreads (bool verbose = true)
    {
        collectModules();
        // Suspend every other thread and report where it was executing.
        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;

        THREADENTRY32 te {};
        te.dwSize = sizeof (te);
        const DWORD selfId = GetCurrentThreadId();
        int count = 0;
        if (Thread32First (snap, &te))
        {
            do
            {
                if (te.th32ThreadID == selfId ||
                    te.th32OwnerProcessID != GetCurrentProcessId())
                    continue;
                HANDLE th = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                        FALSE, te.th32ThreadID);
                if (th == nullptr)
                    continue;
                SuspendThread (th);
                CONTEXT c {};
                c.ContextFlags = CONTEXT_FULL;
                const bool ok = GetThreadContext (th, &c) != 0;
                if (ok)
                {
                    printAddr ("[thread", (DWORD64) c.Rip),
                        std::fprintf (stderr, " id %lu]\n", te.th32ThreadID);
                    if (verbose)
                        scanThreadStack (c);
                }
                ResumeThread (th);
                CloseHandle (th);
                ++count;
            } while (Thread32Next (snap, &te));
        }
        CloseHandle (snap);
        if (count == 0)
            std::fprintf (stderr, "[no other threads found]\n");
    }

    static void printStack (CONTEXT* ctx)
    {
        collectModules();

        const HANDLE proc = GetCurrentProcess();
        if (!SymInitialize (proc, nullptr, FALSE))
            std::fprintf (stderr, "[crash] SymInitialize failed: %lu\n", GetLastError());
        SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

        STACKFRAME64 frame {};
        frame.AddrPC.Offset      = ctx->Rip;
        frame.AddrFrame.Offset   = ctx->Rbp;

        std::fprintf (stderr, "---- crash stack ----\n");
        int walked = 0;
        for (int depth = 0; depth < 48; ++depth)
        {
            if (!StackWalk64 (PROCESSOR_ARCHITECTURE_AMD64, proc, GetCurrentThread(),
                              &frame, ctx, nullptr, SymFunctionTableAccess64,
                              SymGetModuleBase64, nullptr))
                break;
            ++walked;

            const DWORD64 addr = frame.AddrPC.Offset;
            if (addr == 0)
                break;

            char name[512] = { 0 };
            SYMBOL_INFO* sym = static_cast<SYMBOL_INFO*> (std::calloc (1, sizeof (SYMBOL_INFO) + 256));
            DWORD64 base = 0;
            if (sym != nullptr && SymFromAddr (proc, addr, &base, sym))
                std::snprintf (name, sizeof (name), "%s [+0x%llX]", sym->Name,
                               (unsigned long long) (addr - base));
            else
                name[0] = 0;

            if (name[0])
                std::fprintf (stderr, "#%d %s\n", depth, name);
            else if (const ModuleInfo* m = findModule (addr))
                std::fprintf (stderr, "#%d %s + 0x%llX\n", depth, m->name,
                              (unsigned long long) (addr - m->base));
            else
                std::fprintf (stderr, "#%d 0x%llX\n", depth, (unsigned long long) addr);
        }

        if (walked == 0)
        {
            // Fallback: dbghelp's own capture (works without unwind tables).
            void* addrs[48] = { 0 };
            const int n = CaptureStackBackTrace (0, 48, addrs, nullptr);
            std::fprintf (stderr, "[crash] StackWalk64 failed; raw Rip=0x%llX Rsp=0x%llX\n",
                          (unsigned long long) ctx->Rip, (unsigned long long) ctx->Rsp);
            for (int i = 0; i < n; ++i)
                printAddr ("#d", (DWORD64) addrs[i]);
        }

        std::fflush (stderr);
    }

    static volatile long s_handled = 0;

    static LONG WINAPI seHandler (EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;

        // STATUS_FATAL_APP_EXIT: the NVIDIA driver in this environment raises
        // this on an internal thread and handles it with its own SEH frame.
        // We must NOT intercept-and-terminate here; log it and pass through so
        // the original handler can deal with it (the process survives).
        if (code == 0x406D1388)
        {
            std::fprintf (stderr, "[crash] FATAL_APP_EXIT raised on thread %lu - passing through\n",
                          GetCurrentThreadId());
            std::fflush (stderr);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // STATUS_FATAL_APP_TERMINATE / follow-up fail-fast codes: in this VM
        // environment a misbehaving debug-event listener reacts to Win32
        // OutputDebugString() calls by raising these on the calling thread.
        // They are recoverable (execution resumes after they pass through), so
        // log and pass them through instead of terminating. JUCE's assertion
        // logging is patched to avoid OutputDebugString entirely, but this
        // stays as a safety net in case anything else triggers them.
        if (code == 0x4001000A || code == 0x40010006)
        {
            std::fprintf (stderr, "[crash] fail-fast 0x%lX raised on thread %lu - passing through\n",
                          code, GetCurrentThreadId());
            std::fflush (stderr);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Any other exception: a real crash. Trace it and terminate.
        if (InterlockedCompareExchange (&s_handled, 1, 0) == 0)
        {
            std::fprintf (stderr, "[crash] exception code 0x%lX on thread %lu\n",
                          code, GetCurrentThreadId());

            // Scan the ORIGINAL fault stack (from the exception context), not our
            // own handler frames: this reveals who raised the fail-fast.
            if (info->ContextRecord != nullptr)
            {
                collectModules();
                uint8_t buf [65536];
                SIZE_T got = 0;
                if (ReadProcessMemory (GetCurrentProcess(), (LPCVOID) info->ContextRecord->Rsp,
                                      buf, sizeof (buf), &got))
                {
                    std::fprintf (stderr, "[crash] original fault stack (Rip=0x%llX Rsp=0x%llX):\n",
                                  (unsigned long long) info->ContextRecord->Rip,
                                  (unsigned long long) info->ContextRecord->Rsp);
                    DWORD64 last = 0;
                    int printed = 0;
                    for (SIZE_T i = 0; i + 8 <= got && printed < 40; i += 8)
                    {
                        DWORD64 v = 0;
                        std::memcpy (&v, buf + i, 8);
                        const ModuleInfo* m = findModule (v);
                        if (m == nullptr || v == last)
                            continue;
                        last = v;
                        std::fprintf (stderr, "    [+0x%04zX] %s + 0x%llX\n", i,
                                      m->name, (unsigned long long) (v - m->base));
                        ++printed;
                    }
                }
            }

            dumpOtherThreads();
            printStack (info->ContextRecord);
        }
        TerminateProcess (GetCurrentProcess(), 3);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    inline void install()
    {
        // Windows CRT block-buffers stderr when redirected to a file;
        // force unbuffered so diagnostic ordering is chronological.
        std::setvbuf (stderr, nullptr, _IONBF, 0);
        AddVectoredExceptionHandler (1, seHandler);
    }
}

#endif
