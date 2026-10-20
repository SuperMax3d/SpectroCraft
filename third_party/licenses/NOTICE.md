# Third-party components & licences

SpectroCraft is licensed under **AGPL-3.0** (see `LICENSE` at the repo root).
The following third-party code is included or used:

| Component | Location | Licence | Notes |
|---|---|---|---|
| JUCE 9.0.4 | `extern/JUCE/` | GPL-3.0-only (see `extern/JUCE/LICENSE.md`) | Vendored, unmodified except two small defensive patches in the Windows accessibility layer (`juce_Windowing_windows.cpp`: `WM_GETOBJECT` returns no provider; `juce_Accessibility_windows.cpp`: never reports active UIA clients). |
| OpenVINO runtime 2024.6 (headers + DLLs) | `extern/OpenVINO/` | Apache-2.0 (+ model licences, see `extern/OpenVINO/licensing/`) | Used only when `-DSPECTRO_ENABLE_AI=ON`. Loaded at runtime; the app runs without it (delay-loaded import). |
| SoXR resampler | `extern/soxr/` | LGPL-2.1 (`COPYING.LGPL`, `LICENCE`, `AUTHORS`) | Built statically into the exe for Effects → Resample… and internal AI-rate conversion. |
| whisper.cpp v1.6.2 (+ ggml) | `extern/whisper.cpp/` | MIT (`LICENSE`, `AUTHORS`) | Speech transcription engine, built with its OpenVINO encoder backend (optional). |
| DeepFilterNet DSP port | `src/AI/DfStft.*`, `src/AI/DeepFilter*.*`, `include/AI/*` | GPL-3.0-only — Copyright (C) 2024 Intel Corporation | Ported from Intel's `openvino-plugins-ai-audacity` plugin (`mod-openvino/noise_suppression/deepfilternet/`). libtorch replaced by plain C++ on top of SpectroCraft's FFT infrastructure. Full licence text: `GPL-3.0-intel-deepfilternet.txt`. Attribution headers are kept in every ported file. |
| miniz (zip reader) | `extern/miniz/` | Public domain / MIT-style (`miniz.h`) | Used for SCTT chunk zip handling helpers. |

## Model weights (not distributed with the source)

Model files are downloaded on demand to `%APPDATA%/SpectroCraft/models/<model-id>/`
(never committed to this repository). Sources and licences:

| Model | Source | Licence |
|---|---|---|
| DeepFilterNet 2 / 3 (OpenVINO IR) | `intel/deepfilternet-openvino` on Hugging Face | See model card in that repo |
| DenseUNet-LL (FP16, Open Model Zoo 2023.0) | Intel Open Source Model Zoo | Apache-2.0 (see model zoo page) |
| Whisper tiny/base/small/medium/large-v1/v2/v3 (`ggml-*.bin`) | `ggerganov/whisper.cpp` on Hugging Face | See the whisper.cpp repo / original paper; weights are OpenAI's Whisper checkpoints — check the upstream terms before commercial use |

## Resulting licence of SpectroCraft

AGPL-3.0 + GPL-3.0-only (JUCE, Intel DSP port) + LGPL-2.1 (SoXR) + MIT
(whisper.cpp) + Apache-2.0 (OpenVINO runtime) is compatible; the combined
work remains **AGPL-3.0**.
