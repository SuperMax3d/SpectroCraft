// SpectroCraft — dynamic loader for the prebuilt whisper.cpp b5454 DLLs.
//
// The official release binaries (see extern/whisper-b5454/README.md) are
// loaded at runtime with LoadLibrary + GetProcAddress, so the executable is
// built without compiling whisper.cpp/ggml into it and starts even when the
// DLLs are missing (only transcription degrades). The C API struct layouts
// come from the vendored b5454 header (include/whisper.h), which must match
// the shipped binaries exactly.

#pragma once

#include "whisper.h"   // vendored b5454 C API: opaque types + param structs

#include <string>

namespace sc { namespace ai {

class WhisperRuntime {
public:
    static WhisperRuntime& instance();

    // Loads whisper.dll (next to the executable) and resolves every entry
    // point. Idempotent; safe from multiple threads. Returns false with a
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

    // From ggml.dll: registers every backend DLL found next to the executable
    // (ggml-cpu-<arch>.dll, best score wins). b5454's whisper.dll does NOT do
    // this itself — the host application must call it before creating a context.
    void                     (*loadAllBackends) ()                                          = nullptr;

private:
    WhisperRuntime() = default;
    bool _ready = false;
};

}} // namespace sc::ai
