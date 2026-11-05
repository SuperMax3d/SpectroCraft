// SpectroCraft — dynamic loader for the prebuilt whisper.cpp b5454 DLLs.
// See include/AI/WhisperRuntime.h and extern/whisper-b5454/README.md.

#include "AI/WhisperRuntime.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
    // JUCE's headers do not pull in windows.h on their own; keep NOMINMAX set
    // so the min/max macros never leak into later C++ headers.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #undef max
    #undef min
#endif

namespace sc { namespace ai {

WhisperRuntime& WhisperRuntime::instance()
{
    static WhisperRuntime rt;
    return rt;
}

bool WhisperRuntime::load(std::string* errOut)
{
    static std::mutex loadMutex;
    std::lock_guard<std::mutex> lock (loadMutex);
    if (_ready) return true;

#if defined(_WIN32)
    // The build copies whisper.dll into the SpectroCraft subfolder next to the
    // executable, which sc::setupRuntimeDllSearchPath() adds to the process
    // DLL search path at startup; the exe directory itself is still searched
    // first, so old flat layouts keep working. Fall back to an explicit path
    // beside the module for odd PATH configurations.
    HMODULE h = ::LoadLibraryA ("whisper.dll");
    if (h == nullptr)
    {
        char modPath [MAX_PATH] = {};
        const DWORD n = ::GetModuleFileNameA (nullptr, modPath, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            char* lastSlash = nullptr;
            for (char* p = modPath; p < modPath + n; ++p)
                if (*p == '\\') lastSlash = p;
            if (lastSlash != nullptr)
            {
                *lastSlash = '\0';
                std::string full = std::string (modPath) + "\\whisper.dll";
                h = ::LoadLibraryA (full.c_str ());
            }
        }
    }

    if (h == nullptr)
    {
        if (errOut != nullptr)
            *errOut = "whisper.dll was not found in the SpectroCraft folder next to "
                      "SpectroCraft.exe. The bundled whisper.cpp b5454 runtime is missing — "
                      "transcription is unavailable.";
        return false;
    }

    auto resolve = [&h] <typename Fn> (Fn& fn, const char* name) -> bool {
        void* sym = ::GetProcAddress (h, name);
        if (sym == nullptr) return false;
        fn = reinterpret_cast<Fn> (sym);
        return true;
    };

    // Every symbol must be present: a partial match means an incompatible
    // whisper.dll version and would crash on the first call.
    bool ok = resolve (initFromFilePath,  "whisper_init_from_file_with_params")
           && resolve (freeContext,       "whisper_free")
           && resolve (fullDefaultParams, "whisper_full_default_params")
           && resolve (full,              "whisper_full")
           && resolve (nSegments,         "whisper_full_n_segments")
           && resolve (segmentT0,         "whisper_full_get_segment_t0")
           && resolve (segmentT1,         "whisper_full_get_segment_t1")
           && resolve (segmentText,       "whisper_full_get_segment_text")
           && resolve (langId,            "whisper_full_lang_id")
           && resolve (langMaxId,         "whisper_lang_max_id")
           && resolve (langStr,           "whisper_lang_str");

    if (!ok)
    {
        ::FreeLibrary (h);
        if (errOut != nullptr)
            *errOut = "The installed whisper.dll is missing expected entry points — it does not "
                      "match the bundled whisper.cpp b5454 version.";
        return false;
    }

    // ggml.dll is already in memory as a dependency of whisper.dll; this just
    // returns its handle. The CPU backend DLLs (ggml-cpu-<arch>.dll) are NOT
    // loaded by whisper.dll itself — b5454 expects the host application to
    // register them before creating a context. It is idempotent.
    HMODULE hggml = ::LoadLibraryA ("ggml.dll");
    if (hggml == nullptr)
    {
        ::FreeLibrary (h);
        if (errOut != nullptr)
            *errOut = "ggml.dll was not found in the SpectroCraft folder next to "
                      "SpectroCraft.exe — the bundled whisper.cpp b5454 runtime is incomplete.";
        return false;
    }

    // The default ggml_backend_load_all() scans only the executable's
    // directory (and the CWD), but our backend DLLs live in the SpectroCraft
    // subfolder next to the exe, so prefer ggml_backend_load_all_from_path()
    // pointed at that folder when it actually contains backend DLLs; fall back
    // to the default scan for old flat layouts.
    using LoadAllFromPathFn = void (*)(const char*);
    auto fromPath = reinterpret_cast<LoadAllFromPathFn>
        (::GetProcAddress (hggml, "ggml_backend_load_all_from_path"));
    loadAllBackends = reinterpret_cast<void (*)()>
        (::GetProcAddress (hggml, "ggml_backend_load_all"));
    if (fromPath == nullptr && loadAllBackends == nullptr)
    {
        ::FreeLibrary (h);
        ::FreeLibrary (hggml);   // drops our ref; whisper.dll keeps it alive
        if (errOut != nullptr)
            *errOut = "The installed ggml.dll is missing ggml_backend_load_all — it does not "
                      "match the bundled whisper.cpp b5454 version.";
        return false;
    }

    std::string backendDir;
    {
        char modPath [MAX_PATH] = {};
        if (::GetModuleFileNameA (nullptr, modPath, MAX_PATH) > 0)
        {
            for (char* p = modPath + ::strlen (modPath); p > modPath; --p)
                if (*p == '\\' || *p == '/') { *p = '\0'; break; }
            char sub [MAX_PATH];
            std::snprintf (sub, sizeof sub, "%s\\SpectroCraft", modPath);
            WIN32_FIND_DATAA fd {};
            HANDLE hFind = ::FindFirstFileA ((std::string (sub) + "\\ggml-cpu-*.dll").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE)
            {
                backendDir = sub;   // backends present: scan exactly this folder
                ::FindClose (hFind);
            }
        }
    }

    if (!backendDir.empty() && fromPath != nullptr)
        fromPath (backendDir.c_str());
    else if (loadAllBackends != nullptr)
        loadAllBackends ();

    _ready = true;
    return true;
#else
    (void) errOut;
    return false;   // prebuilt binaries are shipped for Windows x64 only
#endif
}

}} // namespace sc::ai
