// SpectroCraft — minimal ggml-cpu.h shim for the vendored whisper.cpp b5454
// header (see extern/whisper-b5454/README.md).
//
// The real b5454 ggml-cpu.h is only needed when compiling ggml itself.
// SpectroCraft never compiles ggml: it loads the prebuilt whisper.dll /
// ggml*.dll binaries at runtime and uses only the C API declared in
// whisper.h, which does not reference any type from ggml-cpu.h. This shim
// exists so that `#include "ggml-cpu.h"` inside whisper.h resolves without
// dragging in the full ggml backend headers (and their own include chains).

#ifndef GGML_CPU_SHIM_H
#define GGML_CPU_SHIM_H

#endif // GGML_CPU_SHIM_H
