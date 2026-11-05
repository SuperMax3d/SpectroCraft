// SoundTouch build configuration for GCC / Clang builds (macOS, Linux, MinGW).
//
// STTypes.h includes "soundtouch_config.h" whenever __GNUC__ is defined: the
// upstream autotools build generates it. This vendored copy is built by CMake
// (extern/soundtouch/CMakeLists.txt), so the header is provided here, next to
// STTypes.h (a quoted include finds it first). Same choices as the MSVC build,
// which does not read this file: 32-bit float samples; the x86 SSE kernels are
// enabled by STTypes.h itself on x86 / x86-64 and disabled elsewhere (ARM).
#pragma once

#ifndef SOUNDTOUCH_FLOAT_SAMPLES
#define SOUNDTOUCH_FLOAT_SAMPLES 1
#endif
