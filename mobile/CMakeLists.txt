# SpectroCraft Mobile (issue #73): the iOS / Android companion app.
#
# A project of its own (configure this folder, not the repository root): it
# builds the JUCE app from the desktop's UI-free engine sources (../src) plus the
# touch UI in mobile/src. The desktop build is not involved.
#
#   iOS (Xcode):   cmake -S mobile -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
#                        -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
#   Android:       mobile/android (Gradle runs this file through externalNativeBuild)
#   macOS / Linux: cmake -S mobile -B build-mobile -G Ninja   (a desktop window, for
#                  development and the mobile unit tests)
#
# See mobile/README.md.

cmake_minimum_required (VERSION 3.22)

project (SpectroCraftMobile VERSION 0.1.0 LANGUAGES C CXX)

if (APPLE)
    enable_language (OBJCXX)
endif()

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_POSITION_INDEPENDENT_CODE ON)   # Android links everything into one shared library

set (SC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# App identity. The bundle / application id is registered in the store account
# that publishes the app; pass -DSC_MOBILE_BUNDLE_ID=... to change it.
set (SC_MOBILE_BUNDLE_ID "com.spectrocraft.mobile" CACHE STRING "iOS bundle id / Android application id")
set (SC_MOBILE_BUILD_NUMBER "1" CACHE STRING "Build number (CI run number)")
option (SC_MOBILE_APP "Build the app (OFF: only the unit tests, on desktop platforms)" ON)

add_subdirectory (${SC_ROOT}/extern/JUCE ${CMAKE_BINARY_DIR}/JUCE)
add_subdirectory (${SC_ROOT}/extern/soxr ${CMAKE_BINARY_DIR}/soxr)
add_subdirectory (${SC_ROOT}/extern/soundtouch ${CMAKE_BINARY_DIR}/soundtouch)

# The desktop's UI font (Source Sans 3), so the app reads like the desktop.
juce_add_binary_data (sc_mobile_fonts
    HEADER_NAME MobileFonts.h
    NAMESPACE MobileFonts
    SOURCES
        ${SC_ROOT}/resources/fonts/SourceSans3-Regular.ttf
        ${SC_ROOT}/resources/fonts/SourceSans3-Semibold.ttf)

# ---------------------------------------------------------------- engine sources
# The desktop's UI-free model and DSP, compiled as they are (same spectrogram,
# same measurements, same WAV / metadata code as the desktop).
set (SC_ENGINE_SOURCES
    ${SC_ROOT}/src/AudioEngine.cpp
    ${SC_ROOT}/src/AxisScale.cpp
    ${SC_ROOT}/src/BkdkChunk.cpp
    ${SC_ROOT}/src/Diagnostics.cpp
    ${SC_ROOT}/src/Envelope.cpp
    ${SC_ROOT}/src/FFTAvx.cpp
    ${SC_ROOT}/src/FileDoc.cpp
    ${SC_ROOT}/src/FileMeta.cpp
    ${SC_ROOT}/src/FilterEQ.cpp
    ${SC_ROOT}/src/MarkerManager.cpp
    ${SC_ROOT}/src/Measures.cpp
    ${SC_ROOT}/src/Resampler.cpp
    ${SC_ROOT}/src/SavedSelections.cpp
    ${SC_ROOT}/src/ScttChunk.cpp
    ${SC_ROOT}/src/Session.cpp
    ${SC_ROOT}/src/SignalGenerator.cpp
    ${SC_ROOT}/src/SoundModel.cpp
    ${SC_ROOT}/src/SpectralEngine.cpp
    ${SC_ROOT}/src/SpectralMaskProcessor.cpp
    ${SC_ROOT}/src/SpectralSelection.cpp
    ${SC_ROOT}/src/Tables.cpp
    ${SC_ROOT}/src/TimeFormat.cpp
    ${SC_ROOT}/src/TimeStretch.cpp
    ${SC_ROOT}/src/UndoManager.cpp
    ${SC_ROOT}/src/VideoAudio.cpp
    ${SC_ROOT}/src/VideoFrames.cpp)
foreach (f IN LISTS SC_ENGINE_SOURCES)
    if (NOT EXISTS ${f})
        list (REMOVE_ITEM SC_ENGINE_SOURCES ${f})   # header-only on this branch
    endif()
endforeach()

if (APPLE)
    set (SC_ENGINE_MM ${SC_ROOT}/src/VideoFrames_mac.mm ${SC_ROOT}/src/VideoAudio_mac.mm)
    set_source_files_properties (${SC_ENGINE_MM} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    list (APPEND SC_ENGINE_SOURCES ${SC_ENGINE_MM})
endif()

if (SC_MOBILE_APP)

juce_add_gui_app (SpectroCraftMobile
    PRODUCT_NAME "SpectroCraft"
    COMPANY_NAME "SpectroCraft"
    VERSION ${PROJECT_VERSION}
    BUILD_VERSION ${SC_MOBILE_BUILD_NUMBER}
    BUNDLE_ID ${SC_MOBILE_BUNDLE_ID}
    ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-1024.png
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "SpectroCraft records and analyses sound from the microphone."
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "SpectroCraft can film a video next to the sound recording."
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "SpectroCraft can record from a Bluetooth microphone."
    FILE_SHARING_ENABLED TRUE          # recordings show in the Files app / iTunes
    DOCUMENT_BROWSER_ENABLED FALSE
    # Files app access to the recordings folder, and no export-compliance
    # question on every TestFlight upload (the app uses no encryption).
    PLIST_TO_MERGE "<plist><dict><key>LSSupportsOpeningDocumentsInPlace</key><true/><key>ITSAppUsesNonExemptEncryption</key><false/></dict></plist>"
    BACKGROUND_AUDIO_ENABLED TRUE      # keep recording with the screen locked
    STATUS_BAR_HIDDEN FALSE
    REQUIRES_FULL_SCREEN TRUE
    TARGETED_DEVICE_FAMILY "1,2"       # iPhone and iPad
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait
                               UIInterfaceOrientationLandscapeLeft
                               UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait
                             UIInterfaceOrientationPortraitUpsideDown
                             UIInterfaceOrientationLandscapeLeft
                             UIInterfaceOrientationLandscapeRight
    SHOULD_ADD_STORYBOARD TRUE
    LAUNCH_STORYBOARD_FILE ${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard
)

# Android: JuceActivity / JuceApp load "libjuce_jni.so" (System.loadLibrary).
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set_target_properties (SpectroCraftMobile PROPERTIES OUTPUT_NAME juce_jni)
endif()

# ---------------------------------------------------------------- mobile sources
set (SC_MOBILE_SOURCES
    src/MobileMain.cpp
    src/MobileStyle.cpp
    src/MobileDocument.cpp
    src/TimeViewport.cpp
    src/SpectrogramPane.cpp
    src/WaveformPane.cpp
    src/LiveAudio.cpp
    src/Recorder.cpp
    src/SignalSource.cpp
    src/AnalysisPane.cpp
    src/StoredPlots.cpp
    src/Calibration.cpp
    src/CalibrationPane.cpp
    src/GeneratorPane.cpp
    src/InputPane.cpp
    src/EditPane.cpp
    src/FilesPane.cpp
    src/VideoCapture.cpp
    src/MainScreen.cpp)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_source_files_properties (src/AudioRoute_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    list (APPEND SC_MOBILE_SOURCES src/AudioRoute_ios.mm)
else()
    list (APPEND SC_MOBILE_SOURCES src/AudioRoute_generic.cpp)
endif()

target_sources (SpectroCraftMobile PRIVATE ${SC_MOBILE_SOURCES} ${SC_ENGINE_SOURCES})

juce_generate_juce_header (SpectroCraftMobile)

target_include_directories (SpectroCraftMobile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SC_ROOT}/include)

target_link_libraries (SpectroCraftMobile
    PRIVATE
        sc_mobile_fonts
        soxr
        soundtouch
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_video
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

target_compile_definitions (SpectroCraftMobile PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_CAMERA=1
    JUCE_CAMERA_RECORD_AUDIO=0   # video only: the app records the microphone itself
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SpectroCraftMobile,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SpectroCraftMobile,JUCE_VERSION>"
    SC_MOBILE=1
    SC_ENABLE_CONTROL=0)

if (APPLE)
    target_link_libraries (SpectroCraftMobile PRIVATE
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
endif()

endif()   # SC_MOBILE_APP

# ---------------------------------------------------------------- unit tests
# Mobile-only logic (generator, loop buffer, calibrator maths, stored plots,
# video link) as a console program, built on desktop platforms only.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    juce_add_console_app (SpectroCraftMobileTests PRODUCT_NAME "SpectroCraftMobileTests")
    target_sources (SpectroCraftMobileTests PRIVATE
        tests/MobileTests.cpp
        src/SignalSource.cpp
        src/StoredPlots.cpp
        src/Calibration.cpp
        src/LiveAudio.cpp
        src/Recorder.cpp
        src/MobileDocument.cpp
        src/LoopBuffer.h
        ${SC_ENGINE_SOURCES})
    juce_generate_juce_header (SpectroCraftMobileTests)
    target_include_directories (SpectroCraftMobileTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${SC_ROOT}/include)
    target_link_libraries (SpectroCraftMobileTests PRIVATE
        soxr soundtouch
        juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats juce::juce_core
        juce::juce_data_structures juce::juce_dsp juce::juce_events juce::juce_graphics juce::juce_gui_basics
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    target_compile_definitions (SpectroCraftMobileTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SC_MOBILE=1 SC_ENABLE_CONTROL=0
        JUCE_MODAL_LOOPS_PERMITTED=0)
    if (APPLE)
        target_link_libraries (SpectroCraftMobileTests PRIVATE
            "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
    endif()
    enable_testing()
    add_test (NAME mobile_unit_tests COMMAND SpectroCraftMobileTests)
endif()
