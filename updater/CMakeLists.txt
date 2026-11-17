# SpectroCraft updater (spectrocraft-updater[.exe])
#
# A plain C++20 console program: no JUCE, no linked libraries beyond the
# self-contained Ed25519/SHA-512 in src/Ed25519.cpp. It has to keep running
# while the rest of the install folder is being replaced, so it depends on
# nothing the app provides.
#
# In the Windows archive it lives next to the runtime DLLs:
#   SpectroCraft.exe
#   SpectroCraft/spectrocraft-updater.exe
#   SpectroCraft/*.dll
#   recorders/wasapi_recorder.dll
# (the same folder the OpenVINO / whisper.cpp / FFmpeg DLLs go to); in the
# macOS bundle next to the executable, in the Linux folder next to the
# program (docs/updates.md).

add_executable (spectrocraft-updater
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_SOURCE_DIR}/src/Ed25519.cpp)

target_include_directories (spectrocraft-updater PRIVATE ${CMAKE_SOURCE_DIR}/include)

set_target_properties (spectrocraft-updater PROPERTIES
    OUTPUT_NAME "spectrocraft-updater"
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if (MSVC)
    target_compile_options (spectrocraft-updater PRIVATE /EHsc)
endif()

# Ship it where the release archive has it (src/Updates.cpp updaterPath(),
# docs/updates.md), so the CI payload and a local build agree:
#   Windows  <exe dir>/SpectroCraft/spectrocraft-updater.exe (next to the
#            runtime DLLs; same pattern as the recorder plugin in
#            plugins/wasapi/CMakeLists.txt)
#   macOS    SpectroCraft.app/Contents/MacOS/spectrocraft-updater
#   Linux    <exe dir>/spectrocraft-updater
# The app depends on it, so every build of SpectroCraft has its updater (the
# update tests run the real program, tests/regression/updates.inc).
add_dependencies (SpectroCraft spectrocraft-updater)

if (WIN32)
    add_custom_command (TARGET spectrocraft-updater POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:SpectroCraft>/SpectroCraft
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:spectrocraft-updater>
                $<TARGET_FILE_DIR:SpectroCraft>/SpectroCraft/spectrocraft-updater.exe)
else ()
    # macOS: TARGET_FILE_DIR of the bundle target is Contents/MacOS (built
    # before the app, so the folder may not exist yet).
    add_custom_command (TARGET spectrocraft-updater POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SpectroCraft>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:spectrocraft-updater>
                $<TARGET_FILE_DIR:SpectroCraft>/spectrocraft-updater)
endif ()
