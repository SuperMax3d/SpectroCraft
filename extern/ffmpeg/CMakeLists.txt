# FFmpeg runtime (LGPL-2.1-or-later) - vendored headers + Windows x64 DLLs.
# A minimal, AUDIO-ONLY build (demuxers + audio decoders, no video decoding,
# no encoders/filters/network): ~2.6 MB of DLLs that can read the audio track out
# of practically any video or audio container. See extern/ffmpeg/README.md for
# provenance, the exact component list and the licence notes.
#
# Used by src/VideoAudio.cpp (video-container audio extraction). Provides:
#   * the `sc_ffmpeg` interface target (include dirs + MSVC import libraries)
#   * sc_enable_ffmpeg(<target>) - links it, defines SC_ENABLE_FFMPEG=1 and copies
#     the DLLs next to the executable (same mechanism as sc_enable_ai()).
#   * an optional command-line tool (sc_extract_audio) built from
#     examples/extract_audio.c:  -DSPECTRO_BUILD_FFMPEG_TOOL=ON

add_library(sc_ffmpeg INTERFACE)
target_include_directories(sc_ffmpeg INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (WIN32 AND MSVC)
    target_link_libraries(sc_ffmpeg INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/avformat.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/avcodec.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/swresample.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/avutil.lib)
else ()
    message(WARNING "FFmpeg runtime is only vendored for Windows x64 (MSVC); video-audio extraction is unavailable.")
endif ()

# Runtime DLLs (all four are required, they depend on each other). They live
# in the repo's self-contained runtime folder, bin/Release.
set(SC_FFMPEG_BIN_DIR ${CMAKE_SOURCE_DIR}/bin/Release)
set(SC_FFMPEG_DLLS
    ${SC_FFMPEG_BIN_DIR}/avformat-61.dll
    ${SC_FFMPEG_BIN_DIR}/avcodec-61.dll
    ${SC_FFMPEG_BIN_DIR}/swresample-5.dll
    ${SC_FFMPEG_BIN_DIR}/avutil-59.dll)

# Same pattern as sc_enable_ai() in extern/OpenVINO: a function the target's own
# CMakeLists calls, so the POST_BUILD copy is attached to that target.
function (sc_enable_ffmpeg tgt)
    if (NOT WIN32 OR NOT MSVC)
        return ()
    endif ()

    target_compile_definitions(${tgt} PRIVATE SC_ENABLE_FFMPEG=1)
    target_link_libraries(${tgt} PRIVATE sc_ffmpeg)

    # The prebuilt DLLs live in the repo's self-contained runtime folder,
    # bin/Release (the same folder that ships on GitHub with the app).
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/bin/Release/avformat-61.dll
            ${CMAKE_SOURCE_DIR}/bin/Release/avcodec-61.dll
            ${CMAKE_SOURCE_DIR}/bin/Release/swresample-5.dll
            ${CMAKE_SOURCE_DIR}/bin/Release/avutil-59.dll
            $<TARGET_FILE_DIR:${tgt}>
        COMMENT "Copying FFmpeg (audio extraction) runtime DLLs next to ${tgt}")
endfunction ()

# Optional helper tool: sc_extract_audio.exe <video> <out.wav>  (see the C file).
option(SPECTRO_BUILD_FFMPEG_TOOL "Build the sc_extract_audio command-line tool" OFF)
if (SPECTRO_BUILD_FFMPEG_TOOL AND WIN32 AND MSVC)
    add_executable(sc_extract_audio ${CMAKE_CURRENT_SOURCE_DIR}/examples/extract_audio.c)
    target_compile_definitions(sc_extract_audio PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(sc_extract_audio PRIVATE sc_ffmpeg)
    add_custom_command(TARGET sc_extract_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SC_FFMPEG_DLLS} $<TARGET_FILE_DIR:sc_extract_audio>
        COMMENT "Copying FFmpeg runtime DLLs next to sc_extract_audio")
endif ()
