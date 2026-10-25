#!/usr/bin/env bash
# Rebuilds the vendored minimal, AUDIO-ONLY, LGPL-2.1+ FFmpeg for SpectroCraft.
#
#   ./build.sh            cross-compile Windows x64 DLLs on Linux and install them
#                         into this folder (bin/, lib/, include/, licensing/)
#   ./build.sh linux-test build the same component set natively (static) into
#                         ./_native, used only to test decoding on Linux
#
# Needs (Ubuntu): sudo apt-get install -y git make nasm mingw-w64 llvm
#
# LICENCE RULES - do not break these or the binaries stop being LGPL:
#   never add --enable-gpl, --enable-nonfree, --enable-version3 or any external
#   codec library (x264, x265, fdk-aac, ...). The configure output must keep
#   saying "License: LGPL version 2.1 or later".
#
# Only DEMUXING + AUDIO DECODING is enabled (the video demuxers/parsers are there
# only so FFmpeg can recognise video streams and not mistake them for audio). Video streams are never decoded,
# so the whole set is ~2.5 MB instead of tens of MB. No encoders, muxers,
# filters, network code or programs are built.
set -euo pipefail

MODE="${1:-windows}"
FFMPEG_TAG="${FFMPEG_TAG:-n7.1.1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-$HERE/_build}"

COMPONENTS=(
  --disable-everything --disable-autodetect --disable-doc --disable-debug
  --disable-network --disable-programs --disable-avdevice --disable-swscale
  --disable-postproc --disable-avfilter --enable-small
  --enable-protocol=file,pipe
  --enable-demuxer=mov,matroska,avi,asf,flv,mpegts,mpegps,ogg,mpegvideo,wav,w64,aiff,mp3,flac,ac3,eac3,dts,dtshd,truehd,aac,mxf,nut,rm,caf,amr,ape,wv,tta,gxf,mpc,mpc8,mlp
  --enable-parser=mpegvideo,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca,mlp,amr
  --enable-decoder=aac,aac_latm,mp1float,mp2float,mp3float,ac3,eac3,dca,truehd,mlp,opus,vorbis,flac,alac,wmav1,wmav2,wmapro,wmalossless,amrnb,amrwb,ape,wavpack,tta,musepack7,musepack8,adpcm_ima_wav,adpcm_ima_qt,adpcm_ms,adpcm_swf,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_bluray,pcm_dvd
)

mkdir -p "$WORK"
if [ ! -d "$WORK/FFmpeg" ]; then
  git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git "$WORK/FFmpeg"
fi

build_dir="$WORK/$MODE"
rm -rf "$build_dir"; mkdir -p "$build_dir"; cd "$build_dir"

if [ "$MODE" = "linux-test" ]; then
  PREFIX="$HERE/_native"
  "$WORK/FFmpeg/configure" --prefix="$PREFIX" --enable-static --disable-shared "${COMPONENTS[@]}"
  make -j"$(nproc)"; make install
  echo "Native test build installed in $PREFIX"
  exit 0
fi

PREFIX="$build_dir/out"
"$WORK/FFmpeg/configure" \
  --prefix="$PREFIX" \
  --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- \
  --enable-shared --disable-static --extra-ldflags="-static-libgcc" \
  "${COMPONENTS[@]}" | tee configure.log
grep -q "License: LGPL version 2.1 or later" configure.log || { echo "NOT LGPL 2.1+ - aborting"; exit 1; }

make -j"$(nproc)"
make install

# --- install into the repo -----------------------------------------------------
# DLLs live in <repo>/bin/Release (the self-contained runtime folder that ships
# on GitHub); headers/libs/licensing stay under extern/ffmpeg.
rm -rf "$HERE/lib" "$HERE/include" "$HERE/licensing"
mkdir -p "$HERE/lib" "$HERE/include" "$HERE/licensing" "$HERE/../../bin/Release"
cp "$PREFIX"/bin/*.dll "$HERE/../../bin/Release/"
x86_64-w64-mingw32-strip --strip-unneeded \
    "$HERE/../../bin/Release/avcodec-61.dll" \
    "$HERE/../../bin/Release/avformat-61.dll" \
    "$HERE/../../bin/Release/avutil-59.dll" \
    "$HERE/../../bin/Release/swresample-5.dll"
cp -r "$PREFIX"/include/libavcodec "$PREFIX"/include/libavformat \
      "$PREFIX"/include/libavutil  "$PREFIX"/include/libswresample "$HERE/include/"
cp "$WORK/FFmpeg/COPYING.LGPLv2.1" "$WORK/FFmpeg/LICENSE.md" "$HERE/licensing/"

# MSVC-style import libraries (proper COFF import libs) from the export .def files
for def in "$PREFIX"/lib/*.def; do
  base="$(basename "$def" .def)"                 # e.g. avcodec-61
  name="${base%-*}"                              # e.g. avcodec
  llvm-dlltool -m i386:x86-64 -d "$def" -D "$base.dll" -l "$HERE/lib/$name.lib"
done

echo "Installed into $HERE and $(cd "$HERE/../.." && pwd)/bin/Release:"
ls -la "$HERE/lib" "$HERE/../../bin/Release"
