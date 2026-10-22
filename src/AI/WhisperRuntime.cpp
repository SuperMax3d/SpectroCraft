// SpectroCraft — dynamic loader for the prebuilt whisper.cpp b5454 DLLs.
// See include/AI/WhisperRuntime.h and extern/whisper-b5454/README.md.

#include "AI/WhisperRuntime.h"

#include <mutex>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
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
    // The app directory is first in the default DLL search order, and the
    // build copies whisper.dll next to the executable. Fall back to an
    // explicit path beside the module for odd PATH configurations.
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
            *errOut = "whisper.dll was not found next to SpectroCraft.exe. The bundled "
                      "whisper.cpp b5454 runtime is missing — transcription is unavailable.";
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
    // loaded by whisper.dll itself — b5454 expects the host application to call
    // ggml_backend_load_all(), which scans the executable's directory for
    // ggml-cpu-*.dll and registers the best-scoring one. It is idempotent.
    HMODULE hggml = ::LoadLibraryA ("ggml.dll");
    if (hggml == nullptr)
    {
        ::FreeLibrary (h);
        if (errOut != nullptr)
            *errOut = "ggml.dll was not found next to SpectroCraft.exe — the bundled "
                      "whisper.cpp b5454 runtime is incomplete.";
        return false;
    }

    void* sym = ::GetProcAddress (hggml, "ggml_backend_load_all");
    if (sym == nullptr)
    {
        ::FreeLibrary (h);
        ::FreeLibrary (hggml);   // drops our ref; whisper.dll keeps it alive
        if (errOut != nullptr)
            *errOut = "The installed ggml.dll is missing ggml_backend_load_all — it does not "
                      "match the bundled whisper.cpp b5454 version.";
        return false;
    }
    loadAllBackends = reinterpret_cast<void (*)()> (sym);
    loadAllBackends ();

    _ready = true;
    return true;
#else
    (void) errOut;
    return false;   // prebuilt binaries are shipped for Windows x64 only
#endif
}

}} // namespace sc::ai
