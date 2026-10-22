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
        ${CMAKE_SOURCE_DIR}/src/MarkerManager.cpp
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

if (MSVC)
    set_source_files_properties (${CMAKE_SOURCE_DIR}/src/FFTAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

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
        soxr   # SoXR resampler (LGPL-2.1), same engine Audacity uses
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
