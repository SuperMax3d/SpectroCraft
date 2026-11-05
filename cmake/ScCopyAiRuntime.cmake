# Copies the AI runtime libraries (macOS / Linux) into DEST: run as a
# POST_BUILD step by sc_enable_ai() in cmake/ScAiRuntimes.cmake.
#   cmake -DDEST=<folder> -DMANIFEST=<build>/sc_ai_runtime_manifest.cmake -P ScCopyAiRuntime.cmake
#
# * OpenVINO: each file is copied under its SONAME / install name (the
#   resolved real file, not the symlink chain): only the names the loader and
#   OpenVINO's plugin / frontend discovery look for end up in the app.
# * whisper.cpp / ggml: every shared library the whisper build produced,
#   symlinks kept as symlinks (libwhisper.dylib -> libwhisper.1.dylib -> ...),
#   so dlopen("libwhisper.<ext>") and the libraries' own references all resolve.

if (NOT DEST OR NOT MANIFEST)
    message (FATAL_ERROR "ScCopyAiRuntime.cmake: DEST and MANIFEST are required")
endif ()
include ("${MANIFEST}")

file (MAKE_DIRECTORY "${DEST}")

foreach (pair IN LISTS SC_OV_FILES)
    string (REPLACE "|" ";" pair "${pair}")
    list (GET pair 0 src)
    list (GET pair 1 name)
    file (REAL_PATH "${src}" real)
    file (COPY_FILE "${real}" "${DEST}/${name}" ONLY_IF_DIFFERENT)
    list (APPEND sc_ov_copied "${DEST}/${name}")
endforeach ()

# Linux: OpenVINO's prebuilt libraries carry no run path, so libopenvino only
# finds libtbb (and the CPU plugin / IR frontend find libopenvino) through the
# executable's inherited DT_RPATH. Give each bundled OpenVINO library its own
# RUNPATH=$ORIGIN: the folder is self-contained whoever loads the library
# (dlopen of the plugin, ldd, tools). macOS install names are rewritten by
# OpenVINO's @rpath layout already.
if (NOT CMAKE_HOST_APPLE AND sc_ov_copied)
    find_program (SC_PATCHELF patchelf)
    if (SC_PATCHELF)
        foreach (lib IN LISTS sc_ov_copied)
            execute_process (COMMAND "${SC_PATCHELF}" --set-rpath "\$ORIGIN" "${lib}"
                             RESULT_VARIABLE rc)
            if (NOT rc EQUAL 0)
                message (FATAL_ERROR "patchelf failed on ${lib}")
            endif ()
        endforeach ()
    else ()
        message (WARNING "patchelf not found: bundled OpenVINO libraries keep no run path "
                         "(they still load from the app through its DT_RPATH; install patchelf "
                         "for a fully self-contained folder)")
    endif ()
endif ()

if (CMAKE_HOST_APPLE)   # script mode: the host is the target (no cross builds)
    file (GLOB libs LIST_DIRECTORIES false "${SC_WHISPER_LIB_DIR}/lib*.dylib")
else ()
    file (GLOB libs LIST_DIRECTORIES false "${SC_WHISPER_LIB_DIR}/lib*.so" "${SC_WHISPER_LIB_DIR}/lib*.so.*")
endif ()
# libparakeet is a second model family of whisper.cpp 1.9 that SpectroCraft
# does not use (only built when the whole project is built).
list (FILTER libs EXCLUDE REGEX "/libparakeet[.]")
if (NOT libs)
    message (FATAL_ERROR "ScCopyAiRuntime.cmake: no whisper.cpp libraries in ${SC_WHISPER_LIB_DIR}")
endif ()
file (COPY ${libs} DESTINATION "${DEST}")
