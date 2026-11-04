# Third-party notices

SpectroCraft itself is licensed under the GNU Affero General Public License
v3.0 (see `LICENSE`). It includes, links against or loads at runtime the
third-party components listed below. Each remains under its own licence; the
full licence texts ship in the folders indicated. More background (usage,
build options, model weights) is in `third_party/licenses/NOTICE.md`.

## Libraries included in the source tree or distributed with the binaries

| Component | Version | Licence | Licence file(s) | Upstream |
|---|---|---|---|---|
| JUCE framework | 9.0.4 | AGPL-3.0 (dual-licensed with the commercial JUCE licence; SpectroCraft uses it under the AGPLv3) | `extern/JUCE/LICENSE.md` | https://juce.com — https://github.com/juce-framework/JUCE |
| FFmpeg (libavformat, libavcodec, libavutil, libswresample; audio-only build, no GPL / nonfree parts) | 7.1.1 | LGPL-2.1-or-later | `extern/ffmpeg/licensing/LICENSE.md`, `extern/ffmpeg/licensing/COPYING.LGPLv2.1` | https://ffmpeg.org — source tag `n7.1.1`, configure options in `extern/ffmpeg/build.sh` |
| SoundTouch audio processing library — Copyright (c) Olli Parviainen | 1.7.1 | LGPL-2.1-or-later | `extern/soundtouch/LICENSE.txt` | https://www.surina.net/soundtouch |
| SoX Resampler library (SoXR) — Copyright (c) 2007-18 robs@users.sourceforge.net | 0.1.3 | LGPL-2.1-or-later | `extern/soxr/LICENCE`, `extern/soxr/COPYING.LGPL`, `extern/soxr/AUTHORS` | https://sourceforge.net/projects/soxr/ |
| Intel Distribution of OpenVINO Toolkit runtime (openvino, CPU plugin, IR frontend, oneTBB redistributables) | 2024.6 | Apache-2.0 | `extern/OpenVINO/licensing/LICENSE`, `extern/OpenVINO/licensing/Apache_license.txt`, `extern/OpenVINO/licensing/redist.txt` | https://github.com/openvinotoolkit/openvino |
| whisper.cpp prebuilt binaries (whisper, ggml, ggml-base, ggml-cpu-*, llama) — Copyright (c) Georgi Gerganov and the whisper.cpp / ggml contributors | b5454 | MIT | `extern/whisper-b5454/README.md` (licence section) | https://github.com/ggml-org/whisper.cpp |

## Code ported from third-party projects

| Component | Licence | Where | Upstream |
|---|---|---|---|
| DeepFilterNet DSP and DenseUNet model runner ported from Intel's OpenVINO AI plugins for Audacity — Copyright (C) 2024 Intel Corporation | GPL-3.0-only | `src/AI/DfStft.*`, `src/AI/DeepFilter*.*`, `src/AI/DenseUnetModel.*`, `include/AI/*`; licence text in `third_party/licenses/GPL-3.0-intel-deepfilternet.txt` | https://github.com/intel/openvino-plugins-ai-audacity |

## Components downloaded on demand (not distributed with SpectroCraft)

These are fetched by the user from their upstream locations at first use and
stay under their upstream terms:

- AI model weights (DeepFilterNet, DenseUNet, Whisper checkpoints): see the
  "Model weights" table in `third_party/licenses/NOTICE.md`.
- The audio.cpp command-line tool (`audiocpp_cli.exe`) and its SAM-Audio /
  HTDemucs / FlashSR / AudioSR model files, used by Stem Separation and Super
  Resolution: https://github.com/0xShug0/audio.cpp and the
  `audio-cpp` organisation on Hugging Face. Check those repositories for their
  licences before redistributing them.

## Licence texts

- GNU LGPL v2.1: `extern/ffmpeg/licensing/COPYING.LGPLv2.1`,
  `extern/soundtouch/LICENSE.txt`, `extern/soxr/COPYING.LGPL`
- Apache License 2.0: `extern/OpenVINO/licensing/Apache_license.txt`
- GNU GPL v3: `third_party/licenses/GPL-3.0-intel-deepfilternet.txt`
- GNU AGPL v3: `LICENSE`
