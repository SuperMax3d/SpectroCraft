# SpectroCraft console test application
juce_add_console_app (SpectroCraftTests
    PRODUCT_NAME "SpectroCraftTests"
)

target_sources (SpectroCraftTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/UnitTests.cpp
        ${CMAKE_SOURCE_DIR}/src/AudioEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralSelection.cpp
        ${CMAKE_SOURCE_DIR}/src/SavedSelections.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralMaskProcessor.cpp
        ${CMAKE_SOURCE_DIR}/src/SignalGenerator.cpp
        ${CMAKE_SOURCE_DIR}/src/TimeStretch.cpp   # SoundTouch tempo-change wrapper
        ${CMAKE_SOURCE_DIR}/src/ContentAwareTimeScale.cpp   # content-aware time scaling (STFT gap analysis)
        ${CMAKE_SOURCE_DIR}/src/FilterEQ.cpp      # FFT zero-phase curve equaliser
        ${CMAKE_SOURCE_DIR}/src/Envelope.cpp      # amplitude-over-time envelope effect
        ${CMAKE_SOURCE_DIR}/src/MarkerManager.cpp
        ${CMAKE_SOURCE_DIR}/src/PythonScripts.cpp
        ${CMAKE_SOURCE_DIR}/src/Actions.cpp     # recorded actions (record/replay/batch)
        ${CMAKE_SOURCE_DIR}/src/WaveformView.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralView.cpp
        ${CMAKE_SOURCE_DIR}/src/Panels.cpp
        ${CMAKE_SOURCE_DIR}/src/ScrollBars.cpp
        ${CMAKE_SOURCE_DIR}/src/MainWindow.cpp
        ${CMAKE_SOURCE_DIR}/src/FileDoc.cpp
        ${CMAKE_SOURCE_DIR}/src/Application.cpp
        ${CMAKE_SOURCE_DIR}/src/Resampler.cpp
        ${CMAKE_SOURCE_DIR}/src/FFTAvx.cpp
        ${CMAKE_SOURCE_DIR}/src/ScttChunk.cpp   # SCTT RIFF chunk I/O (no AI dependency)
        ${CMAKE_SOURCE_DIR}/src/BkdkChunk.cpp   # B&K bkdk calibration chunk parsing
        ${CMAKE_SOURCE_DIR}/src/FileMeta.cpp    # unified metadata bundle + RIFF patching
        ${CMAKE_SOURCE_DIR}/src/Tables.cpp      # CSV tables: import, transforms, alignment
        ${CMAKE_SOURCE_DIR}/src/TablesTrack.cpp # dedicated table track view
        ${CMAKE_SOURCE_DIR}/src/SingleInstance.cpp   # single-instance mode + file forwarding pipe
        ${CMAKE_SOURCE_DIR}/src/MergeFiles.cpp  # multi-file merge (Shift + drag & drop)
        ${CMAKE_SOURCE_DIR}/src/VideoAudio.cpp  # FFmpeg video-container audio extraction
        ${CMAKE_SOURCE_DIR}/src/AudioCpp.cpp    # audio.cpp stem-separation CLI support
        ${CMAKE_SOURCE_DIR}/src/StemSeparation.cpp
        ${CMAKE_SOURCE_DIR}/src/SuperResolution.cpp   # audio.cpp super-resolution (FlashSR/AudioSR)
        ${CMAKE_SOURCE_DIR}/src/Recorder.cpp    # recorder plugin host (Transport > Record)
        ${CMAKE_SOURCE_DIR}/src/AI/DfStft.cpp   # pure DSP (no OpenVINO dependency)
)

# AI module — same gating as the app target.
if (TARGET sc_openvino)
    target_sources (SpectroCraftTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/AI/OvRuntime.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/ModelManager.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/Sha256.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/DenseUnetModel.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/DeepFilterNetModel.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/DeepFilter.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/WhisperTranscribe.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/WhisperPipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/AI/WhisperRuntime.cpp   # dynamic loader for the prebuilt b5454 DLLs
        ${CMAKE_SOURCE_DIR}/src/NoiseSuppression.cpp)
    # Vendored whisper.cpp b5454 C API header (see src/CMakeLists.txt).
    target_include_directories (SpectroCraftTests PRIVATE ${CMAKE_SOURCE_DIR}/extern/whisper-b5454/include)
    sc_enable_ai (SpectroCraftTests)
endif()

# FFmpeg runtime for video-container audio extraction (Windows only).
if (TARGET sc_ffmpeg)
    sc_enable_ffmpeg (SpectroCraftTests)
endif()

if (MSVC)
    set_source_files_properties (${CMAKE_SOURCE_DIR}/src/FFTAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

# The test target compiles its own JuceHeader.h (module includes + defs).
juce_generate_juce_header (SpectroCraftTests)

# /EHa on the OpenVINO wrapper so catch(...) can intercept the delay-load
# failure SEH code (0xC06D007E) raised when openvino.dll cannot be loaded.
if (MSVC AND TARGET sc_openvino)
    set_source_files_properties (${CMAKE_SOURCE_DIR}/src/AI/OvRuntime.cpp
        PROPERTIES COMPILE_OPTIONS "/EHa")
endif()

# MainWindow.cpp includes the auto-generated Version.h, so the tests need the
# same generated directory and must wait for sc_bump_version to run.
add_dependencies (SpectroCraftTests sc_bump_version)
target_include_directories (SpectroCraftTests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)

target_link_libraries (SpectroCraftTests
    PRIVATE
        soxr         # SoXR resampler (LGPL-2.1), same engine Audacity uses
        soundtouch   # SoundTouch tempo change (LGPL-2.1)
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_dsp
        juce::juce_gui_basics
)

target_compile_definitions (SpectroCraftTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        SC_TESTS_EXE=1
        SC_NO_AUDIO_DEVICE=1   # headless tests: no real output device
        SC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Recorder plugins for testing (standard API, see include/RecorderPlugin.h).
# The fake plugin is a deterministic test double; the real WASAPI plugin is
# shipped alongside so loading it can be exercised when devices exist. Both
# land in <test exe dir>/recorders/, which RecorderHost scans by default.
# ---------------------------------------------------------------------------
add_library (sc_fake_recorder SHARED plugins/fake_rec/FakeRecPlugin.cpp)
target_include_directories (sc_fake_recorder PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties (sc_fake_recorder PROPERTIES
    OUTPUT_NAME fake_rec
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

add_custom_command(TARGET sc_fake_recorder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SpectroCraftTests>/recorders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sc_fake_recorder>
            $<TARGET_FILE_DIR:SpectroCraftTests>/recorders/fake_rec.dll)

if (WIN32 AND TARGET sc_wasapi_recorder)
    add_custom_command(TARGET SpectroCraftTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sc_wasapi_recorder>
                $<TARGET_FILE_DIR:SpectroCraftTests>/recorders/wasapi_recorder.dll)
endif()

add_dependencies (SpectroCraftTests sc_fake_recorder)

# ---------------------------------------------------------------------------
# ctest: run the unit suite through the app's --run-unit-tests entry point
# (the same code path users and the README document).
#  - Pass/fail is decided from the summary line the app prints: the GUI exe
#    does not (yet) return the suite's result as its process exit code, and a
#    crash or hang never prints "all unit tests passed".
#  - SC_TEST_DATA_DIR / SC_REQUIRE_TEST_DATA: the reference files in
#    tests/data are found regardless of where the exe lives, and a missing one
#    is a failure instead of a silently skipped test.
#  - SC_TEST_RECORDER_DIR: the fake recorder plugin is loaded from the test
#    binary's recorders/ folder; it is never copied next to the real app.
#  - On macOS/Linux HOME is redirected to the build tree so the run cannot
#    touch the user's real settings store (SpectroCraft.recent: recent files,
#    recorded actions...) or model cache. On Windows the app resolves those
#    through the shell folder API, which ignores the environment: there the
#    suite still uses the real per-user store unless the app isolates it
#    itself (see SC_TESTS_EXE in Application.cpp) - CI runners are disposable.
# Run with: ctest --test-dir build -C <config> --output-on-failure
# ---------------------------------------------------------------------------
add_test (NAME unit_tests
          COMMAND $<TARGET_FILE:SpectroCraft> --run-unit-tests)

set (SC_TEST_ENV
    "SC_TEST_DATA_DIR=${CMAKE_SOURCE_DIR}/tests/data"
    "SC_REQUIRE_TEST_DATA=1"
    "SC_TEST_RECORDER_DIR=$<TARGET_FILE_DIR:SpectroCraftTests>/recorders")
if (NOT WIN32)
    set (SC_TEST_HOME "${CMAKE_CURRENT_BINARY_DIR}/test-home")
    file (MAKE_DIRECTORY "${SC_TEST_HOME}")
    list (APPEND SC_TEST_ENV "HOME=${SC_TEST_HOME}")
endif()

set_tests_properties (unit_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "all unit tests passed"
    FAIL_REGULAR_EXPRESSION "unit tests FAILED"
    ENVIRONMENT "${SC_TEST_ENV}"
    TIMEOUT 3600)
