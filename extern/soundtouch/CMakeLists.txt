# SoundTouch audio processing library - LGPL-2.1
# http://www.surinaar.ee/soundtouch (Olli Parviainen)
#
# Vendored from the Audacity 3.0.5 source tree (lib-src/soundtouch), the same
# engine Audacity uses for its Change Tempo / Time Stretch effect. Build
# recipe mirrors Audacity's cmake proxy:
#   - the standalone translation units only (BPMDetect/PeakFinder are not
#     needed and are omitted),
#   - float sample mode + x86 SSE2 optimizations (x64 baseline). MSVC picks
#     both up automatically from STTypes.h, so no config header is required.

add_library (soundtouch STATIC)

target_sources (soundtouch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source/AAFilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/FIFOSampleBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/FIRFilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/RateTransposer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/SoundTouch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/TDStretch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/cpu_detect_x86.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/sse_optimized.cpp
)

target_include_directories (soundtouch PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# The library uses its own `uint` typedef and BOOL/TRUE/FALSE macros; keep the
# build quiet under MSVC.
if (MSVC)
    target_compile_options (soundtouch PRIVATE /W3)
endif()
