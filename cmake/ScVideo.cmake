# Video window support: picture decoding (VideoFrames*) and, on macOS, the
# AVFoundation audio reader for movie containers (VideoAudio_mac.mm).
#
#   macOS:   AVFoundation / CoreMedia / CoreVideo (system frameworks)
#   Windows: Media Foundation (system DLLs, delay-loaded so Windows "N"
#            editions without the Media Feature Pack still start)
#   Linux:   no picture decoder; VideoFrames.cpp reports it as unavailable
#
# Called from src/CMakeLists.txt and tests/CMakeLists.txt for their target
# (functions run in the caller's directory, so the per-source ARC flag below
# applies to that target).
function (sc_enable_video tgt)
    target_sources (${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/VideoFrames.cpp)

    if (APPLE)
        set (_sc_video_mm
            ${CMAKE_SOURCE_DIR}/src/VideoFrames_mac.mm
            ${CMAKE_SOURCE_DIR}/src/VideoAudio_mac.mm)
        target_sources (${tgt} PRIVATE ${_sc_video_mm})
        set_source_files_properties (${_sc_video_mm} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries (${tgt} PRIVATE
            "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
    elseif (WIN32)
        target_sources (${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/VideoFrames_win.cpp)
        target_link_libraries (${tgt} PRIVATE mfplat mfreadwrite mfuuid ole32)
        if (MSVC)
            set_property (TARGET ${tgt} APPEND PROPERTY LINK_OPTIONS
                "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll")
            target_link_libraries (${tgt} PRIVATE delayimp.lib)
        endif ()
    endif ()
endfunction ()
