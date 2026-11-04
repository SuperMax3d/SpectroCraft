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
endforeach ()

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
