cmake_minimum_required(VERSION 3.22)
project(SpectroCraft VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored copy of the JUCE framework (official repo, master branch)
add_subdirectory(extern/JUCE)

# SoXR resampler library (LGPL-2.1) — same engine Audacity uses for resampling.
add_subdirectory(extern/soxr)

# AI features (OpenVINO noise suppression, later Whisper). The OpenVINO runtime
# is vendored in extern/OpenVINO; model weights are downloaded on first use and
# never committed. Disable with -DSPECTRO_ENABLE_AI=OFF for a lean build.
option(SPECTRO_ENABLE_AI "Build AI features (OpenVINO noise suppression)" ON)
if (SPECTRO_ENABLE_AI AND WIN32)
    add_subdirectory(extern/OpenVINO)
endif()

# Auto-incremented version: this target runs on EVERY build and bumps the
# patch number in VERSION.txt (1.0.0 -> 1.0.1 -> ...), then regenerates
# generated/Version.h (About box) + SC_VERSION.txt. The sc_patch_generated
# target (src/CMakeLists.txt) applies that version to JUCE's generated
# resources.rc / JuceHeader.h before they are compiled, so the exe's Windows
# file properties always show the freshly bumped version.
set(SC_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(sc_bump_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DSC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/bump_version.cmake)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
