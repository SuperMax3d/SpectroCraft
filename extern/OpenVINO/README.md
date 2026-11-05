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
<repo>/bin/Release/SpectroCraft/ the runtime DLLs live in the repo's self-contained
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

`src/AI/OvRuntime.cpp` loads `openvino.dll` by name at runtime; it is found in
the `SpectroCraft` subfolder next to the exe, which `sc::setupRuntimeDllSearchPath()`
(`include/RuntimeDlls.h`) adds to the DLL search path at startup (the CMake build
copies them there from `<repo>/bin/Release/SpectroCraft`).
The CPU plugin
and IR frontend are discovered by OpenVINO automatically because they sit in
the same folder as `openvino.dll`.

## macOS and Linux

Nothing is vendored for these platforms: `cmake/ScAiRuntimes.cmake` downloads
Intel's official 2024.6.0 (build 17404.4c0f47d2335) archive at configure
time, verifies it against the SHA-256 published next to it and uses it through
`find_package(OpenVINO)` (the C++ headers come from the archive):

| Platform | Archive (under `https://storage.openvinotoolkit.org/repositories/openvino/packages/2024.6/`) | SHA-256 |
|---|---|---|
| macOS arm64 | `macos/m_openvino_toolkit_macos_12_6_2024.6.0.17404.4c0f47d2335_arm64.tgz` | `7780d1675c43db511e42ba05eed93d5f1b33164ccd838ecf2c876eb283947907` |
| macOS x86_64 | `macos/m_openvino_toolkit_macos_12_6_2024.6.0.17404.4c0f47d2335_x86_64.tgz` | `62621bd51238820b2df367da047d96f9241f6f097ec3fe4f74d1a332499ce2d4` |
| Ubuntu 24.04 x86_64 | `linux/l_openvino_toolkit_ubuntu24_2024.6.0.17404.4c0f47d2335_x86_64.tgz` | `d47281e02644d93fa299853f660dec63b787f0a101aa393b4cde71d1c8c00c18` |

The app links `libopenvino` normally (no delay-load) and ships the same four
pieces as on Windows — core, CPU plugin (`libopenvino_arm_cpu_plugin.so` on
Apple Silicon, `libopenvino_intel_cpu_plugin.so` elsewhere), IR frontend and
`libtbb.12` — under their SONAME / install names in
`SpectroCraft.app/Contents/Frameworks` (macOS) or next to the executable
(Linux). OpenVINO finds the plugin and the frontend in the folder it was
loaded from (compiled-in plugin list, no `plugins.xml`). Redistribution is
covered by the archive's own licence files (Apache-2.0, `docs/licensing`).
