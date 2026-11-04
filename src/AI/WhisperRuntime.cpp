// SpectroCraft — run-time loader for the whisper.cpp b5454 libraries
// (prebuilt DLLs on Windows; libwhisper.dylib / .so built by
// cmake/ScAiRuntimes.cmake on macOS / Linux).
// See include/AI/WhisperRuntime.h and extern/whisper-b5454/README.md.

#include "AI/WhisperRuntime.h"

#include <mutex>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <juce_core/juce_core.h>
    #include <dlfcn.h>
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
    // The build copies the runtime DLLs next to the executable: load them
    // from there by full path. A bare name would fall through the default
    // search order (system folders, PATH, current directory) whenever a DLL
    // is missing from the app folder, and load whatever is found there.
    std::string exeDir;
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
                exeDir = modPath;
            }
        }
    }
    // Bare names only when the exe path itself is unavailable (over MAX_PATH).
    auto runtimeDll = [&exeDir] (const char* name) {
        return exeDir.empty () ? std::string (name) : exeDir + "\\" + name;
    };

    HMODULE h = ::LoadLibraryA (runtimeDll ("whisper.dll").c_str ());

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
    HMODULE hggml = ::LoadLibraryA (runtimeDll ("ggml.dll").c_str ());
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

    // ggml_backend_load_all() also scans the current directory for
    // ggml-cpu-*.dll (e.g. a folder of WAV files on a network share when the
    // app is launched by file association). Restrict the scan to the exe
    // folder when this ggml build exports the path-taking variant.
    void* symFromPath = ::GetProcAddress (hggml, "ggml_backend_load_all_from_path");
    if (symFromPath != nullptr && !exeDir.empty ())
        reinterpret_cast<void (*)(const char*)> (symFromPath) (exeDir.c_str ());
    else
        loadAllBackends ();

    _ready = true;
    return true;
#else
    // macOS / Linux: the build (cmake/ScAiRuntimes.cmake) copies libwhisper
    // and the ggml libraries next to the executable (Linux, console tools) or
    // into <app>.app/Contents/Frameworks (macOS app bundle). Load them from
    // there by full path, never through the default search order.
   #if JUCE_MAC
    const char* const libName = "libwhisper.dylib";
   #else
    const char* const libName = "libwhisper.so";
   #endif
    const juce::File exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                  .getParentDirectory ();
    juce::Array<juce::File> searchDirs { exeDir };
   #if JUCE_MAC
    searchDirs.add (exeDir.getSiblingFile ("Frameworks"));   // Contents/MacOS -> Contents/Frameworks
   #endif

    void* h = nullptr;
    juce::File libDir;
    std::string loadError;
    for (const auto& dir : searchDirs)
    {
        const juce::File lib = dir.getChildFile (libName);
        if (!lib.existsAsFile ())
            continue;
        h = ::dlopen (lib.getFullPathName ().toRawUTF8 (), RTLD_NOW | RTLD_LOCAL);
        if (h != nullptr)
        {
            libDir = dir;
            break;
        }
        if (const char* e = ::dlerror ())
            loadError = e;
    }

    if (h == nullptr)
    {
        if (errOut != nullptr)
        {
            juce::StringArray where;
            for (const auto& dir : searchDirs)
                where.add (dir.getFullPathName ());
            *errOut = std::string (libName) + (loadError.empty ()
                          ? " was not found in " + where.joinIntoString (" or ").toStdString ()
                          : " could not be loaded (" + loadError + ")")
                      + ". The bundled whisper.cpp b5454 runtime is missing - transcription is unavailable.";
        }
        return false;
    }

    // dlsym on the handle also searches the libraries libwhisper depends on
    // (libggml, libggml-base), which is where the ggml entry points live.
    auto resolve = [&h] <typename Fn> (Fn& fn, const char* name) -> bool {
        void* sym = ::dlsym (h, name);
        if (sym == nullptr) return false;
        fn = reinterpret_cast<Fn> (sym);
        return true;
    };

    // Every symbol must be present: a partial match means an incompatible
    // libwhisper version and would crash on the first call.
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
           && resolve (langStr,           "whisper_lang_str")
           && resolve (loadAllBackends,   "ggml_backend_load_all");

    if (!ok)
    {
        ::dlclose (h);
        if (errOut != nullptr)
            *errOut = std::string ("The bundled ") + libName + " is missing expected entry points - it "
                      "does not match the whisper.cpp b5454 version SpectroCraft was built for.";
        return false;
    }

    // Backends built as loadable modules (Linux x86-64: one ggml CPU backend
    // per micro-architecture, libggml-cpu-<arch>.so) are registered by the
    // host, like on Windows. Restrict the scan to the runtime folder: the
    // path-less variant also scans the current directory. On macOS the
    // backends are linked into libggml and this registers nothing more.
    void* symFromPath = ::dlsym (h, "ggml_backend_load_all_from_path");
    if (symFromPath != nullptr)
        reinterpret_cast<void (*)(const char*)> (symFromPath) (libDir.getFullPathName ().toRawUTF8 ());
    else
        loadAllBackends ();

    _ready = true;
    return true;
#endif
}

}} // namespace sc::ai
