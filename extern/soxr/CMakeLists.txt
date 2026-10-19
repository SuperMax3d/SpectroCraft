# SoX Resampler Library (libsoxr) - LGPL-2.1
# https://sourceforge.net/projects/libsamplerate/  (robs@users.sourceforge.net)
#
# Vendored from the same source tree Audacity uses for its resampling
# ("SoXR" in their UI). Build recipe mirrors Audacity's cmake proxy:
#   - the exact set of standalone translation units (several .c files such as
#     pffft.c, cr-core.c and util-simd.c are #include-only and must NOT be
#     compiled on their own),
#   - _USE_MATH_DEFINES / _CRT_SECURE_NO_WARNINGS / SOXR_LIB defines,
#   - WITH_PFFFT + SSE2 SIMD path (x64 baseline).

add_library (soxr STATIC)

target_sources (soxr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cr32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cr32s.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cr64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data-io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dbesi0.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fft4g.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fft4g32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fft4g64.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/filter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pffft32s.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/soxr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util32s.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vr32.c
)

# Same defines Audacity uses for its MSVC build.
target_compile_definitions (soxr PRIVATE
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS
    SOXR_LIB
)

if (MSVC)
    # SSE2 is x64 baseline; the SIMD path (pffft32s.c / cr32s.c) uses it.
    target_compile_options (soxr PRIVATE "/arch:SSE2")
endif()

# Public API header soxr.h lives in src/; the preconfigured soxr-config.h
# (see its file header) sits one level up and is only needed internally.
target_include_directories (soxr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories (soxr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
