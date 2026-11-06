# Test baseline before any change (macOS)

Date: 2026-11-05. Branch `ai-control`, tip `e04d711` (= SuperMax3d's `main` `8c527e3` + a docs commit).

## Deviation from the instructions

The instructions called for a Linux cloud agent (`baseline-linux.md`, build under `xvfb-run`). The run
actually took place on Nicolas's Mac (MacBook Air M-series, macOS 26.6, Xcode 26.6, AppleClang 21, arm64),
in an isolated worktree. No Docker or Linux VM available: **no local Linux build**. Linux and
Windows are covered by CI (`gh workflow run build.yml`, see each lot's notes).

## Configuration

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSPECTRO_ENABLE_AI=ON -DSPECTRO_ENABLE_FFMPEG=OFF \
  -DSC_AI_DOWNLOAD_DIR=<SpectroCraft-build>/ai-downloads -DSC_TEST_MODELS_DIR=<SpectroCraft-build>/test-models
cmake --build build --target SpectroCraft --parallel 8
ctest --test-dir build -j 8 -V
```

OpenVINO 2024.6 (arm64) and whisper.cpp b5454 archives already downloaded (local cache), test models
`denseunet`, `deepfilternet2`, `whisper-base` already present: the AI tests actually ran.

## Result

- Build: OK, no errors.
- ctest: **10/10** (2 fixtures + 8 shards), 67 s.
- Checks: **4859/4859 checks passed** (sum over the 8 shards).
- 9 tests or parts skipped, all expected on macOS:
  - change gain dialog / drawing key shortcuts / scale dialog focus: keyboard focus requires the OS foreground;
  - playback chain, rendering backend: Windows only;
  - scfft AVX2 (×2): no AVX2 on arm64;
  - super-res cli args: audio.cpp CLI is Windows only;
  - video audio extraction: build without FFmpeg.

No pre-existing failures.
