# SpectroCraft console test application
juce_add_console_app (SpectroCraftTests
    PRODUCT_NAME "SpectroCraftTests"
)

target_sources (SpectroCraftTests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/UnitTests.cpp
        ${CMAKE_SOURCE_DIR}/src/AudioEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/MarkerManager.cpp
        ${CMAKE_SOURCE_DIR}/src/WaveformView.cpp
        ${CMAKE_SOURCE_DIR}/src/SpectralView.cpp
        ${CMAKE_SOURCE_DIR}/src/Panels.cpp
        ${CMAKE_SOURCE_DIR}/src/ScrollBars.cpp
        ${CMAKE_SOURCE_DIR}/src/MainWindow.cpp
        ${CMAKE_SOURCE_DIR}/src/FileDoc.cpp
        ${CMAKE_SOURCE_DIR}/src/Application.cpp
        ${CMAKE_SOURCE_DIR}/src/FFTAvx.cpp
)

if (MSVC)
    set_source_files_properties (${CMAKE_SOURCE_DIR}/src/FFTAvx.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

juce_generate_juce_header (SpectroCraftTests)
target_include_directories (SpectroCraftTests PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries (SpectroCraftTests
    PRIVATE
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
)
