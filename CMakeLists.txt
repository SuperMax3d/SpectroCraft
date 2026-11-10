cmake_minimum_required(VERSION 3.22)
project(SpectroCraft VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored copy of the JUCE framework (official repo, master branch)
add_subdirectory(extern/JUCE)

# SoXR resampler library (LGPL-2.1) — same engine Audacity uses for resampling.
add_subdirectory(extern/soxr)

# SoundTouch audio processing library (LGPL-2.1) — the same engine Audacity
# uses for its Change Tempo / Time Stretch effect.
add_subdirectory(extern/soundtouch)

# AI features (OpenVINO noise suppression, whisper.cpp transcription). Windows:
# the runtime DLLs are vendored (extern/OpenVINO, bin/Release). macOS / Linux:
# the pinned OpenVINO 2024.6 archive and whisper.cpp b5454 are fetched and
# verified at configure time (cmake/ScAiRuntimes.cmake). Model weights are
# downloaded on first use and never committed. Disable with
# -DSPECTRO_ENABLE_AI=OFF for a lean build.
option(SPECTRO_ENABLE_AI "Build AI features (OpenVINO noise suppression, Whisper transcription)" ON)
set(SC_JUCE_NEEDS_CURL FALSE)   # set to TRUE by ScAiRuntimes.cmake on Linux
if (SPECTRO_ENABLE_AI AND WIN32)
    add_subdirectory(extern/OpenVINO)
elseif (SPECTRO_ENABLE_AI AND (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    include(cmake/ScAiRuntimes.cmake)
endif()
if (SC_JUCE_NEEDS_CURL)
    set(SC_JUCE_USE_CURL 1)
else()
    set(SC_JUCE_USE_CURL 0)
endif()

# FFmpeg (LGPL-2.1+) audio extraction for video containers (.mp4, .mkv, ...).
# The runtime is vendored in extern/ffmpeg as prebuilt Windows x64 DLLs +
# import libraries; on other platforms this is a no-op and SpectroCraft
# falls back to the built-in JUCE formats only.
option(SPECTRO_ENABLE_FFMPEG "Enable FFmpeg-based video audio extraction" ON)
if (SPECTRO_ENABLE_FFMPEG AND WIN32)
    add_subdirectory(extern/ffmpeg)
endif()

# Video window: picture decoding with the platform media APIs (no extra
# dependency), plus the AVFoundation audio reader for movies on macOS.
include(cmake/ScVideo.cmake)

# Recorder plugins (standard C ABI, see include/RecorderPlugin.h). The WASAPI
# plugin is shipped in <exe dir>/recorders/ and picked up at startup.
if (WIN32)
    add_subdirectory(plugins/wasapi)
endif()

# Auto-incremented version: this target runs on EVERY build and bumps the
# patch number in VERSION.txt (1.0.0 -> 1.0.1 -> ...), then regenerates
# generated/Version.h (About box) + SC_VERSION.txt. The sc_patch_generated
# target (src/CMakeLists.txt) applies that version to JUCE's generated
# resources.rc before it is compiled, so the exe's Windows file properties
# always show the freshly bumped version. The generated files are declared as
# BYPRODUCTS so Ninja re-checks what depends on them after this target ran
# (instead of deciding at build start, one version behind).
set(SC_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(sc_bump_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DSC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/bump_version.cmake
    BYPRODUCTS ${SC_GENERATED_DIR}/Version.h ${SC_GENERATED_DIR}/SC_VERSION.txt)

# Loopback control server + --control + Window > Allow AI control (REPORT
# §9 Q14). OFF builds keep the registry, --json and action replay.
option(SC_ENABLE_CONTROL "Build the AI-control server (localhost, opt-in at run time)" ON)
if (SC_ENABLE_CONTROL)
    set(SC_CONTROL_DEFINE 1)
else()
    set(SC_CONTROL_DEFINE 0)
endif()

# AI control (docs/ai-control/REPORT.md §5): command registry, session
# journal, jobs, --json CLI. Shared by the app and the console test target.
set(SC_CONTROL_SOURCES
    ${CMAKE_SOURCE_DIR}/src/control/CommandRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/control/ControlState.cpp
    ${CMAKE_SOURCE_DIR}/src/control/Journal.cpp
    ${CMAKE_SOURCE_DIR}/src/control/JobManager.cpp
    ${CMAKE_SOURCE_DIR}/src/control/StateJson.cpp
    ${CMAKE_SOURCE_DIR}/src/control/JsonCli.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Core.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/History.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Steps.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Jobs.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/View.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Transport.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Ai.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Measure.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Edit.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Python.cpp
    ${CMAKE_SOURCE_DIR}/src/control/commands/Diag.cpp
    ${CMAKE_SOURCE_DIR}/src/control/PathPolicy.cpp
    ${CMAKE_SOURCE_DIR}/src/control/ControlServer.cpp)

# UI font of the docking workspace (src/Docking.cpp): Source Sans 3, Adobe's
# open-source UI family (SIL OFL 1.1, third_party/licenses/SourceSans3-OFL.md),
# embedded so the panels look the same on every platform. Linked by the app
# and the console test target.
juce_add_binary_data (sc_dock_fonts
    HEADER_NAME DockFonts.h
    NAMESPACE DockFonts
    SOURCES
        ${CMAKE_SOURCE_DIR}/resources/fonts/SourceSans3-Regular.ttf
        ${CMAKE_SOURCE_DIR}/resources/fonts/SourceSans3-Semibold.ttf)
set_target_properties (sc_dock_fonts PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

# `ctest` registration lives in tests/CMakeLists.txt (unit_tests, with the test environment).
