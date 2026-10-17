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
        ${CMAKE_CURRENT_SOURCE_DIR}/FFTAvx.cpp
        ${CMAKE_SOURCE_DIR}/tests/UnitTests.cpp
)

# Application icon (icon.ico) is embedded at runtime: include/IconData.h holds
# the raw ICO bytes and MainWindow applies them via CreateIconFromResourceEx +
# WM_SETICON. A .rc resource file is not used because this toolchain's rc.exe
# emits ICON statements as RT_CURSOR entries, which Windows ignores for window
# icons.

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
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SpectroCraft,JUCE_VERSION>"
)
