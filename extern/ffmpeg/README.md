# FFmpeg runtime (vendored, Windows x64) - audio extraction from video

A **minimal, audio-only, LGPL** build of [FFmpeg](https://ffmpeg.org) 7.1.1, vendored the same way as
`extern/OpenVINO` and `extern/whisper-b5454`: headers + import libraries + prebuilt DLLs, copied next
to `SpectroCraft.exe` by CMake. It lets SpectroCraft read the audio track out of practically any
video (or audio) container. The integration lives in `src/VideoAudio.cpp`
(a `juce::AudioFormatReader` over libavformat/libavcodec/libswresample, registered after JUCE's
basic formats so native files keep their usual readers); this folder makes the library available
(`sc_enable_ffmpeg()` in `CMakeLists.txt`, define `SC_ENABLE_FFMPEG=1`).

## Size

| DLL | Size |
|---|---|
| `avcodec-61.dll` | 1.17 MB |
| `avformat-61.dll` | 0.65 MB |
| `avutil-59.dll` | 0.70 MB |
| `swresample-5.dll` | 0.11 MB |
| **Total** | **~2.6 MB** (prebuilt full LGPL FFmpeg: 60-140 MB) |

The DLLs import only Windows system DLLs (`KERNEL32`, `msvcrt`, `bcrypt`). All four are required.

## What it can read

Only demuxing + audio decoding is compiled in (video streams are skipped by the demuxer, never decoded).

- **Containers:** MP4/MOV/M4A/3GP, Matroska/WebM, AVI, ASF/WMV/WMA, FLV, MPEG-TS/M2TS, MPEG-PS/VOB, Ogg,
  WAV/W64/AIFF/CAF, MP3, FLAC, AC-3/E-AC-3, DTS, TrueHD/MLP, raw AAC, MXF, NUT, RealMedia, AMR, APE,
  WavPack, TTA, Musepack, GXF.
- **Audio codecs:** AAC (incl. LATM), MP1/MP2/MP3, AC-3, E-AC-3, DTS, TrueHD/MLP, Opus, Vorbis, FLAC, ALAC,
  WMA (v1/v2/Pro/Lossless), AMR-NB/WB, Monkey's Audio, WavPack, TTA, Musepack, IMA/MS/SWF ADPCM,
  A-law/u-law and all common PCM flavours.
- **Not included (by design):** every video decoder, encoders, muxers, filters, network protocols.

Component list (`build.sh`):

```
COMPONENTS=(
  --disable-everything --disable-autodetect --disable-doc --disable-debug
  --disable-network --disable-programs --disable-avdevice --disable-swscale
  --disable-postproc --disable-avfilter --enable-small
  --enable-protocol=file,pipe
  --enable-demuxer=mov,matroska,avi,asf,flv,mpegts,mpegps,ogg,mpegvideo,wav,w64,aiff,mp3,flac,ac3,eac3,dts,dtshd,truehd,aac,mxf,nut,rm,caf,amr,ape,wv,tta,gxf,mpc,mpc8,mlp
  --enable-parser=mpegvideo,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca,mlp,amr
  --enable-decoder=aac,aac_latm,mp1float,mp2float,mp3float,ac3,eac3,dca,truehd,mlp,opus,vorbis,flac,alac,wmav1,wmav2,wmapro,wmalossless,amrnb,amrwb,ape,wavpack,tta,musepack7,musepack8,adpcm_ima_wav,adpcm_ima_qt,adpcm_ms,adpcm_swf,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_bluray,pcm_dvd
)
```

## Licence (important)

Built with `--disable-everything` and a `License: LGPL version 2.1 or later` configure result. It must
stay that way: **never add `--enable-gpl`, `--enable-nonfree`, `--enable-version3` or an external codec
library** (x264, fdk-aac, ...). SpectroCraft is AGPL-3.0; FFmpeg's LGPL-2.1-or-later is compatible and the
libraries are used as separate dynamic DLLs, so users can replace them with their own build.

- Licence texts: `licensing/COPYING.LGPLv2.1`, `licensing/LICENSE.md`.
- Corresponding source: <https://github.com/FFmpeg/FFmpeg/tree/db69d06eeeab4f46da15030a80d539efb4503ca8> (tag `n7.1.1`) plus the exact
  configure options in `build.sh` - running `build.sh` reproduces these binaries.
- Some audio formats are covered by patents in some countries (AAC, AC-3, DTS, ...). Most of the common
  ones (MP3, AAC-LC, AC-3) have expired, but check your own distribution situation.

## Provenance

| Item | Value |
|---|---|
| Source | `https://github.com/FFmpeg/FFmpeg` tag `n7.1.1`, commit `db69d06eeeab4f46da15030a80d539efb4503ca8` |
| Toolchain | mingw-w64 (Ubuntu), `--target-os=mingw32 --arch=x86_64 --enable-shared --extra-ldflags=-static-libgcc` |
| Import libraries | `lib/*.lib`, generated from the DLLs' export `.def` files with `llvm-dlltool` (proper COFF import libraries for MSVC) |
| SHA-256 | see below |

```
b4d749d39b8a356411e15b7bcdb80dcfee5587c878670cd40d6f29d181d53858  avcodec-61.dll
1f8df251924e388f9ffd1e4486798e61647bb88689ec9706b10e07b8323ad848  avformat-61.dll
f6d033f8d2a31612309526d69d849f55d3b11754578f629ab31cee888c588dd6  avutil-59.dll
003583d11fc33123ce4d76b5701d3fa46f3272165f1568b4fb1b885df3caeb76  swresample-5.dll
```

## Using it from C++

```cpp
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}
```

`sc_enable_ffmpeg(SpectroCraft)` (already called in `src/CMakeLists.txt`) links the import libraries and
copies the DLLs. **`examples/extract_audio.c` is a complete, tested reference**: open -> choose audio stream ->
decode -> convert to interleaved float -> write WAV (RF64 above 4 GB).

The real integration (`src/VideoAudio.cpp`) streams instead of pre-decoding: it keeps the demuxer +
decoder alive inside an `AudioFormatReader`, seeks per read, and positions output sample-accurately from
frame timestamps (zero-filling gaps and everything beyond EOF). Notes that shaped it:

- Pass file paths to `avformat_open_input` as **UTF-8** (works with Unicode paths on Windows).
- `AVPacket`s are not owned by the decoder: always `av_packet_free`, drain output on `EAGAIN`,
  and send a flush packet at EOF so codecs can emit their buffered tail.
- Allocate one fresh `AVFrame` per `avcodec_receive_frame` call - `av_frame_free(&fr)` nulls the
  pointer, so reusing it across loop iterations passes NULL to the decoder.
- Only accept audio streams with a known sample rate and channel count (see `is_audio()` in the example);
  video-only containers are rejected by returning no reader.

## Command-line tool

`examples/extract_audio.c` also builds to a small helper (`-DSPECTRO_BUILD_FFMPEG_TOOL=ON`, target
`sc_extract_audio`, off by default):

```
sc_extract_audio --list movie.mkv
sc_extract_audio [--progress] [--stream N] movie.mkv out.wav     :: 32-bit float WAV, source rate/channels
```

## Tested

The same component list was built natively and run through the example on 28 files made with a system
FFmpeg (MP4/MOV/MKV/WebM/AVI/FLV/WMV/MPEG-PS/VOB/MPEG-TS/M2TS/Ogg/MP3/AAC/FLAC; AAC, MP3, MP2, AC-3, E-AC-3,
Vorbis, Opus, FLAC, ALAC, WMA, PCM; H.264/HEVC/MPEG-1/2/4/WMV video). Every result is **bit-identical** to the
system FFmpeg's float decode. Also verified: stereo channel order, 6-channel files, selecting the 2nd audio
track, 96 kHz / 24-bit, Unicode file names, missing / video-only / truncated / garbage input (clean error
codes), and a >4 GB output (valid RF64, 135,000 s). The Windows DLLs are executed on Windows by the
SpectroCraft unit tests (`testVideoAudioExtraction`): hand-crafted PCM AVI files decoded through the
real reader with sample-exact sequential and random-access reads.

## Rebuilding

```
sudo apt-get install -y git make nasm mingw-w64 llvm
./build.sh              # rebuilds the DLLs, headers and import libs into this folder
./build.sh linux-test   # native static build for testing decode on Linux
```
