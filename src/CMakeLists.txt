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
        ${CMAKE_CURRENT_SOURCE_DIR}/SpectralView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MarkerManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Panels.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ScrollBars.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MainWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resampler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/FFTAvx.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/NoiseSuppression.cpp)
    sc_enable_ai (SpectroCraft)
endif()

# Application icon (icon.ico) is embedded at runtime: include/IconData.h holds
# the raw ICO bytes and MainWindow applies them via CreateIconFromResourceEx +
# WM_SETICON. A .rc resource file with ICON statements is not used because this
# toolchain's rc.exe emits them as RT_CURSOR entries, which Windows ignores for
# window icons. (The version resource below is a VERSIONINFO-only .rc, which
# rc.exe handles correctly.)

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
            -P ${CMAKE_SOURCE_DIR}/cmake/patch_generated.cmake
    DEPENDS ${SC_RC_FILE} ${SC_HEADER_FILE}
    COMMENT "Applying bumped version to generated resources.rc / JuceHeader.h")
add_custom_target (sc_patch_generated
    DEPENDS ${CMAKE_BINARY_DIR}/generated/generated_patched.stamp)
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

juce_generate_juce_header (SpectroCraft)
target_include_directories (SpectroCraft PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries (SpectroCraft
    PRIVATE
        soxr   # SoXR resampler (LGPL-2.1), same engine Audacity uses
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
