# OpenVINO Runtime (vendored, Windows x64)

Intel® Distribution of OpenVINO™ Toolkit **2024.6** — the exact version pinned by
Intel's `openvino-plugins-ai-audacity` build docs. Only the pieces needed for
CPU inference of OpenVINO IR models are vendored here; everything else from
the toolkit (GPU/NPU plugins, Python, samples) is omitted.

## Provenance

| Item | Source |
|---|---|
| Package | `w_openvino_toolkit_windows_2024.6.0.17404.4c0f47d2335_x86_64.zip` from <https://storage.openvinotoolkit.org/repositories/openvino/packages/2024.6/windows/> |
| SHA-256 (verified at vendoring time) | `45a71a1e11f3e8a8109118e56434e79b3b2bfcc828b38b0e61be55949f317a53` (matches the published `.sha256`) |

## Contents

```
<repo>/bin/Release/              the runtime DLLs live in the repo's self-contained
  openvino.dll                    folder (shipped on GitHub with the app):
  openvino_intel_cpu_plugin.dll   core runtime (ov::Core, model read/compile)
  openvino_ir_frontend.dll        OpenVINO IR (.xml/.bin) frontend
  tbb12.dll                       oneTBB (threading; required by core + CPU plugin)
include/openvino/                 C++ API headers (openvino.hpp et al.)
licensing/
  LICENSE                         Apache-2.0 (OpenVINO source, incl. headers)
  Apache_license.txt              same text as shipped in the toolkit
  redist.txt                      Intel's redistribution list — explicitly covers
                                  runtime/bin/intel64/Release/* and
                                  runtime/3rdparty/tbb/bin/*
```

## Licence notes

- OpenVINO is **Apache-2.0** (see `licensing/LICENSE`). Compatible with
  SpectroCraft's AGPL-3.0; the combined app stays AGPL-3.0.
- The DLLs are redistributable per `licensing/redist.txt` (Windows section:
  `runtime/bin/intel64/Release/*`, `runtime/3rdparty/tbb/bin/*`).
- Runtime dependencies beyond these files: the MSVC runtime
  (`MSVCP140.dll`, `VCRUNTIME140*.dll`) and standard Windows DLLs — already a
  requirement of SpectroCraft itself.

## How it is used

`src/AI/OvRuntime.cpp` loads `openvino.dll` from the application directory at
runtime (the CMake build copies them from `<repo>/bin/Release` next to the exe).
The CPU plugin
and IR frontend are discovered by OpenVINO automatically because they sit in
the same folder as `openvino.dll`.
