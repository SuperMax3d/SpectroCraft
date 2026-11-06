# audio.cpp on macOS and Linux (stem separation, super resolution)

Date: 2026-11-06. Branch `audiocpp-posix`, on top of `ai-control`. Machine: macOS 26.6 arm64
(Apple M2), Debug + AI build. Linux and Windows: the `audio.cpp end to end` CI step (below).

## What changed

Stem separation and super resolution used to be Windows-only: `src/AudioCpp.cpp` offered the four
Windows zips, downloaded with WinINet, launched with `CreateProcessW`, and its POSIX branch was a stub
("requires the Windows CLI binary"). The 0xShug0/audio.cpp v0.9.2-drama3-base-small release that the
Windows build already pins also publishes macOS and Linux builds; they are now used.

| Platform | Variants (`binaryChoices()`) | Archive | Installed files |
|---|---|---|---|
| Windows x64 | `cpu`, `cuda12.4`, `cuda13.3`, `vulkan` (unchanged) | `.zip` | `audiocpp_cli.exe` only (unchanged, CUDA still blocked) |
| macOS arm64 / x64 | `metal` (backends `metal`, `cpu`) | `.tar.gz` | `audiocpp_cli` (self-contained, system frameworks only, ad-hoc signed) |
| Linux x64 | `cpu`, `vulkan` (portable builds) | `.tar.gz` | `audiocpp_cli` + 20 `libggml*.so` (21 for vulkan; RUNPATH `$ORIGIN`; needs `libgomp.so.1`) |
| others (e.g. Linux arm64) | none | — | `unsupported_platform`, as before |

Archive and executable SHA-256 / sizes were computed on 2026-11-06 from the downloaded assets; the
archive hashes equal the `digest` GitHub publishes.

- **Install** (`installBinaryFromArchive`, replaces `installBinaryFromZip`): the format is read from
  the first bytes. A `.tar.gz` goes through a small streaming tar reader (ustar / GNU / pax headers,
  GNU base-256 sizes, header checksum, symlinks between extracted files copied): only the executable
  and the top-level `lib*.so*` / `*.dylib` are extracted, into a scratch folder first; the executable
  is checked against its published hash, made executable, and the library names are written to
  `runtime-files.txt` so that a reinstall or `removeInstalledBinary()` (catalogue removal) deletes
  them. A refused archive leaves the installed runtime untouched.
- **Download** (`downloadFile`): `juce::WebInputStream` (NSURLSession / libcurl) on macOS / Linux,
  same contract and `commitDownload` validation as WinINet.
- **Run**: `sc::py::runProcessEx` (own process group, SIGTERM then SIGKILL of the whole tree on
  cancel, `PR_SET_PDEATHSIG` on Linux, stdin `/dev/null`). It gained `RunOptions::onStdoutLine` (both
  platforms) so the `--log` lines still stream to the progress dialog / job. The Windows runner is
  unchanged.
- **Backends**: `metal` accepted by the services, the commands (`backend` `oneOf`) and the windows
  (Backend combo item 4, macOS only; ids 1-3 keep their meaning; the stored setting is clamped to the
  platform's list instead of 1-3). `audioCppBackendsForVariant("metal")` = `metal, cpu`.
- **Catalogue / devices**: the audio.cpp models list every CLI platform, the runtime entries this
  platform's tag; `installable`, `ai.devices` and `resolveAiDevice` depend on
  `sc::acpp::platformSupported()` instead of `JUCE_WINDOWS`. `ai.devices.audiocpp` gained `platform`.
- **Control**: flag `WindowsOnly` renamed `NeedsAudioCpp` (checked at run time); the MCP adapter
  describes both (`WindowsOnly` for older apps). `docs/commands.md` regenerated.

## Tests

- Unit / regression (embedded suite): tar reader (release layout, pax and GNU long names, POSIX
  prefix, symlinks, dangling link, refusals: not gzip, not tar, truncated, no executable), tar
  install / reinstall / removal (execute bit, previous libraries removed, a damaged manifest never
  deletes outside the folder), zip install with this platform's variant, platform tables, backend
  combo, catalogue / devices / commands on every platform, `onStdoutLine`, and a shell script
  standing in for `audiocpp_cli` (paths with spaces, `--log` lines relayed, super resolution and
  SAM-Audio results read back, cancellation of a hanging CLI and its `sleep` grandchild).
  `ctest -j 8`: every new and updated check passes. Three load-sensitive checks failed once under the
  8 parallel shards and passed when run alone (`scale focus`, `load: p99 < 50 ms`, `views#W1`).
- **Real run on the M2** (`tools/acpp_e2e.py`, also run by hand through `--json`): catalogue
  install of `audiocpp-cli-metal` (32.7 MB downloaded, SHA-256 verified, extracted) and `flashsr`;
  FlashSR on a 10 s stereo clip: 17.6 s on `metal` (first run includes the Metal shader compilation),
  mono 48 kHz output; htdemucs_6stems q8_0: 9-12 s on `metal`, 22 s on `cpu`, 6 stereo stems. The
  stems SpectroCraft produces with `cpu` are identical (difference below -169 dB) to the ones the
  CLI writes when run directly on the same file: the integration passes the audio through
  untouched. `metal` and `cpu` stems differ by -11 to -36 dB (GPU arithmetic inside audio.cpp).
- **CI**: new step `audio.cpp end to end (real CLI)` in `windows-ai` (cpu zip — the first real run
  on Windows), `macos-ai` (Metal build, cpu backend) and `linux-ai` (cpu tar.gz, under xvfb-run).

## Not done / debts

- SAM-Audio (text-prompted stems) was only run with the fake CLI: its smallest model is 2.4 GB. The
  real runs used htdemucs_6stems (42 MB, placed by hand: the catalogue still lists htdemucs as a
  manual install).
- The Windows CUDA variants still extract `audiocpp_cli.exe` only (pre-existing). The tar reader
  is not used for zips; extracting the zip's `ggml*.dll` the same way would unblock them.
- No GUI walk-through of the two windows on macOS (their layout tests run in the suite).
- The Linux CUDA build (`cuda12.8-colab` plus a separate `cudart` archive) and the `lib-*` packages
  (C API, for the planned migration away from the CLI) are not offered.
