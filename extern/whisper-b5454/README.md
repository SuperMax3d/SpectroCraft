# whisper.cpp b5454 — prebuilt Windows x64 binaries (vendored)

SpectroCraft's Whisper transcription runs on the **official prebuilt
binaries** from the [whisper.cpp release `b5454`](https://github.com/ggml-org/whisper.cpp/releases/tag/b5454)
(`whisper-bin-x64.zip`), loaded at runtime with `LoadLibrary`. This is the
same solution that made CPU-only transcription fast in practice: the release
ships per-microarchitecture ggml CPU backend DLLs (`ggml-cpu-*.dll`) with
runtime SIMD dispatch, plus a prebuilt decoder path via `llama.dll`.

## Contents

| Path | What it is |
|---|---|
| `include/whisper.h` | The C API header from b5454, verbatim (MIT). |
| `include/ggml.h` | The ggml header from b5454, verbatim (MIT) — needed because whisper.h includes it; only two typedefs are actually used. |
| `include/ggml-cpu.h` | Minimal shim: whisper.h includes this file but uses nothing from it, so the full backend headers are not needed to compile against the C API. |
| `bin/*.dll` | The runtime DLLs, copied next to SpectroCraft.exe at build time (see `sc_enable_ai()` in `extern/OpenVINO/CMakeLists.txt`). |

## Runtime DLL set (all required)

- `whisper.dll` — the Whisper C API library.
- `ggml.dll`, `ggml-base.dll` — ggml runtime + base kernels.
- `llama.dll` — decoder backend used by b5454's whisper.cpp.
- `ggml-cpu-{alderlake,cannonlake,cascadelake,haswell,icelake,sandybridge,skylakex,sse42,x64}.dll`
  — per-microarchitecture CPU backends; ggml-base loads the best match at
  runtime (the generic `x64`/`sse42` DLLs are the fallback).

Only the Microsoft Visual C++ runtime (`MSVCP140.dll`, `VCRUNTIME140*.dll`)
is required from the system — no SDL, no OpenVINO.

## How SpectroCraft uses it

- `include/AI/WhisperRuntime.h` / `src/AI/WhisperRuntime.cpp` resolve the C API
  entry points with `LoadLibraryA("whisper.dll")` + `GetProcAddress` (the app
  directory is first in the Windows DLL search order).
- `src/AI/WhisperTranscribe.cpp` calls those function pointers; it no longer
  compiles whisper.cpp into the executable and has no OpenVINO encoder path.
- Models are plain ggml `.bin` checkpoints downloaded from
  `ggerganov/whisper.cpp` on Hugging Face (see `src/AI/ModelManager.cpp`).

## Licence

MIT — Copyright (c) 2024-2025 Georgi Gerganov and the whisper.cpp contributors
(https://github.com/ggml-org/whisper.cpp). The ggml and llama.cpp components
are MIT as well. See `third_party/licenses/NOTICE.md`.
