# SpectroCraft GUI application
juce_add_gui_app (SpectroCraft
    PRODUCT_NAME "SpectroCraft"
    COMPANY_NAME "SpectroCraft"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
)

target_sources (SpectroCraft
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Application.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FileDoc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AudioEngine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SpectralEngine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SpectralSelection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SavedSelections.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SpectralMaskProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SignalGenerator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/TimeStretch.cpp   # SoundTouch tempo-change wrapper
        ${CMAKE_CURRENT_SOURCE_DIR}/FilterEQ.cpp      # FFT zero-phase curve equaliser
        ${CMAKE_CURRENT_SOURCE_DIR}/Envelope.cpp      # amplitude-over-time envelope effect
        ${CMAKE_CURRENT_SOURCE_DIR}/SpectralView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MarkerManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/PythonScripts.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Panels.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ScrollBars.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MainWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resampler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FFTAvx.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ScttChunk.cpp   # SCTT RIFF chunk I/O (no AI dependency)
        ${CMAKE_CURRENT_SOURCE_DIR}/BkdkChunk.cpp   # B&K bkdk calibration chunk parsing
        ${CMAKE_CURRENT_SOURCE_DIR}/FileMeta.cpp    # unified metadata bundle + RIFF patching
        ${CMAKE_CURRENT_SOURCE_DIR}/MergeFiles.cpp  # multi-file merge (Shift + drag & drop)
        ${CMAKE_CURRENT_SOURCE_DIR}/VideoAudio.cpp  # FFmpeg video-container audio extraction
        ${CMAKE_CURRENT_SOURCE_DIR}/AudioCpp.cpp    # audio.cpp stem-separation CLI support
        ${CMAKE_CURRENT_SOURCE_DIR}/StemSeparation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SuperResolution.cpp   # audio.cpp super-resolution (FlashSR/AudioSR)
        ${CMAKE_CURRENT_SOURCE_DIR}/Recorder.cpp    # recorder plugin host (Transport > Record)
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/DfStft.cpp   # pure DSP (no OpenVINO dependency)
        ${CMAKE_SOURCE_DIR}/tests/UnitTests.cpp
)

# AI module (OpenVINO plumbing + model manager). Only compiled when the
# OpenVINO runtime target exists (SPECTRO_ENABLE_AI=ON on Windows).
if (TARGET sc_openvino)
    target_sources (SpectroCraft PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/OvRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/ModelManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/Sha256.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/DenseUnetModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/DeepFilterNetModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/DeepFilter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/WhisperTranscribe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/WhisperPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AI/WhisperRuntime.cpp   # dynamic loader for the prebuilt b5454 DLLs
        ${CMAKE_CURRENT_SOURCE_DIR}/NoiseSuppression.cpp)
    # Vendored whisper.cpp b5454 C API header (the binaries themselves are
    # loaded at runtime — see extern/whisper-b5454/README.md).
    target_include_directories (SpectroCraft PRIVATE ${CMAKE_SOURCE_DIR}/extern/whisper-b5454/include)
    sc_enable_ai (SpectroCraft)
endif()

# FFmpeg runtime for video-container audio extraction (Windows only).
if (TARGET sc_ffmpeg)
    sc_enable_ffmpeg (SpectroCraft)
endif()

# Application icon: icon.ico is embedded into the exe as a proper PE resource
# (RT_ICON + RT_GROUP_ICON) by appending an ICON statement to JUCE's generated
# resources.rc in sc_patch_generated below. This makes Windows Explorer and the
# taskbar show the app icon without any runtime help. MainWindow additionally
# applies the same icon via CreateIconFromResourceEx + WM_SETICON (include/
# IconData.h) as a fallback for the window title-bar icon.

# Version handling: sc_bump_version (root CMakeLists) bumps VERSION.txt on every
# build and regenerates generated/Version.h (About box). sc_patch_generated then
# rewrites the version into JUCE's generated resources.rc (VS_VERSION_INFO ->
# Windows file properties) and JuceHeader.h, after juceaide has written them but
# before they are compiled. (JUCE's JUCE_USER_DEFINED_RC_FILE mechanism is not
# usable here: this environment's rc.exe ignores /D preprocessor definitions.)
add_dependencies (SpectroCraft sc_bump_version)
target_include_directories (SpectroCraft PRIVATE ${CMAKE_BINARY_DIR}/generated)

get_target_property (SC_JUCE_CODE_DIR SpectroCraft JUCE_GENERATED_SOURCES_DIRECTORY)
set (SC_RC_FILE "${SC_JUCE_CODE_DIR}/SpectroCraft_resources.rc")
set (SC_HEADER_FILE "${SC_JUCE_CODE_DIR}/JuceHeader.h")
add_custom_command (
    OUTPUT ${CMAKE_BINARY_DIR}/generated/generated_patched.stamp
    COMMAND ${CMAKE_COMMAND}
            -DRC_FILE=${SC_RC_FILE}
            -DHEADER_FILE=${SC_HEADER_FILE}
            -DVERSION_FILE=${CMAKE_BINARY_DIR}/generated/SC_VERSION.txt
            -DSTAMP_FILE=${CMAKE_BINARY_DIR}/generated/generated_patched.stamp
            -DICON_FILE=${CMAKE_SOURCE_DIR}/icon.ico
            -P ${CMAKE_SOURCE_DIR}/cmake/patch_generated.cmake
    DEPENDS ${SC_RC_FILE} ${SC_HEADER_FILE} ${CMAKE_SOURCE_DIR}/icon.ico
    COMMENT "Applying bumped version to generated resources.rc / JuceHeader.h")
add_custom_target (sc_patch_generated
    DEPENDS ${CMAKE_BINARY_DIR}/generated/generated_patched.stamp)
# bump must run first: the patch reads SC_VERSION.txt, and bump deletes the
# stamp that makes this command re-run on every build.
add_dependencies (sc_patch_generated sc_bump_version)
if (TARGET SpectroCraft_rc_lib)   # must compile the rc only after it is patched
    add_dependencies (SpectroCraft_rc_lib sc_bump_version sc_patch_generated)
endif()
add_dependencies (SpectroCraft sc_patch_generated)

# AVX2 FFT: compile this one translation unit with /arch:AVX2 on MSVC so the
# intrinsics are available; runtime CPUID dispatch falls back to scalar otherwise.
if (MSVC)
    set_source_files_properties (${CMAKE_CURRENT_SOURCE_DIR}/FFTAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

# /EHa on the OpenVINO wrapper so catch(...) can intercept the delay-load
# failure SEH code (0xC06D007E) raised when openvino.dll cannot be loaded.
if (MSVC AND TARGET sc_openvino)
    set_source_files_properties (${CMAKE_CURRENT_SOURCE_DIR}/AI/OvRuntime.cpp
        PROPERTIES COMPILE_OPTIONS "/EHa")
endif()

juce_generate_juce_header (SpectroCraft)
target_include_directories (SpectroCraft PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries (SpectroCraft
    PRIVATE
        soxr         # SoXR resampler (LGPL-2.1), same engine Audacity uses
        soundtouch   # SoundTouch tempo change (LGPL-2.1), same engine as Audacity
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions (SpectroCraft
    PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SpectroCraft,JUCE_PRODUCT_NAME>"
)
