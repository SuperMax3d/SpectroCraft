// SpectroCraft — run-time loader for the whisper.cpp b5454 libraries.
//
// Windows: the official release binaries (see extern/whisper-b5454/README.md)
// are loaded with LoadLibrary + GetProcAddress. macOS / Linux: libwhisper and
// libggml* are built from the same tag by cmake/ScAiRuntimes.cmake, shipped
// next to the executable (Linux) or in the app bundle's Frameworks folder
// (macOS), and loaded with dlopen + dlsym. Either way the executable is built
// without compiling whisper.cpp/ggml into it and starts even when the
// libraries are missing (only transcription degrades). The C API struct
// layouts come from the vendored b5454 header (include/whisper.h), which must
// match the shipped binaries exactly (checked at configure time off Windows).

#pragma once

#include "whisper.h"   // vendored b5454 C API: opaque types + param structs

#include <string>

namespace sc { namespace ai {

class WhisperRuntime {
public:
    static WhisperRuntime& instance();

    // Loads whisper.dll / libwhisper.dylib / libwhisper.so (see above) and
    // resolves every entry point. On Windows the DLLs live in the SpectroCraft
    // subfolder next to the executable (added to the process search path at
    // startup), with a fallback to the exe directory for old flat layouts.
    // Idempotent; safe from multiple threads. Returns false with a
    // human-readable message in *errOut when the DLL or a symbol is missing.
    bool load(std::string* errOut);

    bool ready() const { return _ready; }

    // Resolved C API entry points (null until load() succeeds). Signatures
    // match whisper.cpp b5454 exactly — do not "modernise" them.
    struct whisper_context* (*initFromFilePath)(const char*, struct whisper_context_params) = nullptr;
    void                     (*freeContext)     (struct whisper_context*)                    = nullptr;
    struct whisper_full_params (*fullDefaultParams)(enum whisper_sampling_strategy)          = nullptr;
    int                      (*full)            (struct whisper_context*, struct whisper_full_params,
                                                 const float*, int)                        = nullptr;
    int                      (*nSegments)       (struct whisper_context*)                    = nullptr;
    int64_t                  (*segmentT0)       (struct whisper_context*, int)              = nullptr;
    int64_t                  (*segmentT1)       (struct whisper_context*, int)              = nullptr;
    const char*              (*segmentText)     (struct whisper_context*, int)              = nullptr;
    int                      (*langId)          (struct whisper_context*)                   = nullptr;
    int                      (*langMaxId)       ()                                          = nullptr;
    const char*              (*langStr)         (int)                                       = nullptr;

    // From ggml (ggml.dll / libggml): registers every CPU backend library.
    // load() prefers scanning the runtime folder via
    // ggml_backend_load_all_from_path (the SpectroCraft subfolder next to the
    // exe on Windows, the executable's own folder off Windows) and only falls
    // back to this default scan for old flat layouts. b5454's whisper library
    // does NOT register backends itself — the host application must do it
    // before creating a context.
    void                     (*loadAllBackends) ()                                          = nullptr;

private:
    WhisperRuntime() = default;
    bool _ready = false;
};

}} // namespace sc::ai
