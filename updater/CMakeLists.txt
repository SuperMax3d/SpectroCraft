# SpectroCraft updater (spectrocraft-updater.exe)
#
# A plain C++20 console program: no JUCE, no linked libraries beyond the
# self-contained Ed25519/SHA-512 in src/Ed25519.cpp. It has to keep running
# while the rest of the install folder is being replaced, so it depends on
# nothing the app provides.
#
# In a release archive it lives next to the runtime DLLs:
#   SpectroCraft.exe
#   SpectroCraft/spectrocraft-updater.exe
#   SpectroCraft/*.dll
#   recorders/wasapi_recorder.dll
# (the same folder the OpenVINO / whisper.cpp / FFmpeg DLLs go to, see
# docs/updates.md).

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

# Ship it inside the app's runtime folder (same pattern as the recorder plugin
# in plugins/wasapi/CMakeLists.txt): CI uploads the whole Release folder as the
# SpectroCraft-windows-x64 artifact, so the updater ends up in the zip.
add_custom_command (TARGET spectrocraft-updater POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SpectroCraft>/SpectroCraft
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:spectrocraft-updater>
            $<TARGET_FILE_DIR:SpectroCraft>/SpectroCraft/spectrocraft-updater.exe)
