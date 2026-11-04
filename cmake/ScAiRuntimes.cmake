# AI runtimes for macOS and Linux: OpenVINO 2024.6 (noise suppression) and
# whisper.cpp b5454 (transcription). Included by the root CMakeLists.txt when
# SPECTRO_ENABLE_AI=ON on a non-Windows host; Windows keeps its vendored DLLs
# (extern/OpenVINO, bin/Release) and never reads this file.
#
# Nothing binary is committed: both runtimes are fetched at configure time,
# pinned to an exact version and verified against a SHA-256 written below.
#
#  * OpenVINO: Intel's official archive for the host (macOS arm64 / x86_64,
#    Ubuntu 24.04 x86_64), from storage.openvinotoolkit.org. The hashes are
#    the ones published next to each archive (<archive>.sha256). The archive is
#    extracted in the build tree and used through find_package(OpenVINO).
#    -DSC_OPENVINO_DIR=<root> uses an existing OpenVINO 2024.6 installation
#    instead (the folder holding runtime/cmake/OpenVINOConfig.cmake, or that
#    folder itself), e.g. on another Linux distribution or architecture.
#  * whisper.cpp: source archive of the commit tagged b5454, built as shared
#    libraries in its own Release build (ExternalProject: optimised even in a
#    Debug build of the app, and none of the app's flags leak into it). The
#    app loads libwhisper at run time with dlopen (src/AI/WhisperRuntime.cpp),
#    like it loads whisper.dll on Windows; the C API header it compiles against
#    is the vendored extern/whisper-b5454/include/whisper.h, checked here to
#    be identical to the fetched one.
#
# -DSC_AI_DOWNLOAD_DIR=<dir> keeps the downloaded archives in a folder shared
# by several build trees (or a CI cache); a file already there is reused when
# its SHA-256 matches.
#
# Defines:
#  * sc_openvino          INTERFACE target (OpenVINO C++ API, linked normally:
#                         no delay-load outside Windows - the libraries ship
#                         inside the app);
#  * sc_whisper_runtime   ExternalProject building libwhisper + libggml*;
#  * sc_enable_ai(<tgt>)  same contract as the Windows function in
#                         extern/OpenVINO/CMakeLists.txt: SC_ENABLE_AI=1, link,
#                         and copy every runtime library next to the program
#                         (Linux: the executable's folder, RPATH $ORIGIN;
#                         macOS: <app>.app/Contents/Frameworks, RPATH
#                         @executable_path/../Frameworks; console tools on macOS:
#                         next to the executable).
#  * SC_JUCE_NEEDS_CURL   TRUE on Linux: JUCE's URL/WebInputStream needs libcurl
#                         for HTTPS there (model downloads). macOS uses
#                         NSURLSession and needs nothing.

include_guard (GLOBAL)
include (FetchContent)
include (ExternalProject)

set (SC_OPENVINO_DIR "" CACHE PATH
     "Existing OpenVINO 2024.6 installation to use instead of downloading the pinned archive (macOS / Linux)")
set (SC_AI_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_ai_downloads" CACHE PATH
     "Folder where the pinned AI runtime archives are downloaded (can be shared between build trees)")

set (SC_AI_RUNTIMES_OK FALSE)

# ---------------------------------------------------------------------------
# Download helper: fetch <url> into SC_AI_DOWNLOAD_DIR/<name>, verified.
# ---------------------------------------------------------------------------
function (_sc_ai_fetch url name sha256 out_path)
    set (dest "${SC_AI_DOWNLOAD_DIR}/${name}")
    if (EXISTS "${dest}")
        file (SHA256 "${dest}" have)
        if (have STREQUAL sha256)
            set (${out_path} "${dest}" PARENT_SCOPE)
            return ()
        endif ()
        message (STATUS "AI runtimes: ${name} does not match its pinned SHA-256, downloading it again")
        file (REMOVE "${dest}")
    endif ()
    file (MAKE_DIRECTORY "${SC_AI_DOWNLOAD_DIR}")
    message (STATUS "AI runtimes: downloading ${url}")
    file (DOWNLOAD "${url}" "${dest}.part"
          EXPECTED_HASH SHA256=${sha256}
          TLS_VERIFY ON
          STATUS st)
    list (GET st 0 code)
    if (NOT code EQUAL 0)
        file (REMOVE "${dest}.part")
        list (GET st 1 msg)
        message (FATAL_ERROR "AI runtimes: download of ${url} failed (${code}: ${msg}).\n"
                            "Retry, place the file in ${SC_AI_DOWNLOAD_DIR} yourself, "
                            "or configure with -DSPECTRO_ENABLE_AI=OFF.")
    endif ()
    file (RENAME "${dest}.part" "${dest}")
    set (${out_path} "${dest}" PARENT_SCOPE)
endfunction ()

# ---------------------------------------------------------------------------
# OpenVINO 2024.6.0 (build 17404.4c0f47d2335)
# ---------------------------------------------------------------------------
set (_sc_ov_base "https://storage.openvinotoolkit.org/repositories/openvino/packages/2024.6")
set (_sc_ov_build "2024.6.0.17404.4c0f47d2335")
set (_sc_ov_archive "")

if (APPLE)
    set (_sc_arch "${CMAKE_OSX_ARCHITECTURES}")
    if (NOT _sc_arch)
        set (_sc_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif ()
    list (LENGTH _sc_arch _sc_narch)
    if (_sc_narch GREATER 1)
        message (FATAL_ERROR "AI runtimes: universal macOS builds are not supported "
                            "(CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}); build one architecture.")
    endif ()
    if (_sc_arch STREQUAL "arm64")
        set (_sc_ov_archive "m_openvino_toolkit_macos_12_6_${_sc_ov_build}_arm64.tgz")
        set (_sc_ov_url "${_sc_ov_base}/macos/${_sc_ov_archive}")
        set (_sc_ov_sha256 "7780d1675c43db511e42ba05eed93d5f1b33164ccd838ecf2c876eb283947907")
    elseif (_sc_arch STREQUAL "x86_64")
        set (_sc_ov_archive "m_openvino_toolkit_macos_12_6_${_sc_ov_build}_x86_64.tgz")
        set (_sc_ov_url "${_sc_ov_base}/macos/${_sc_ov_archive}")
        set (_sc_ov_sha256 "62621bd51238820b2df367da047d96f9241f6f097ec3fe4f74d1a332499ce2d4")
    endif ()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # Built on Ubuntu 24.04 (glibc 2.39, libstdc++ from GCC 13).
    set (_sc_ov_archive "l_openvino_toolkit_ubuntu24_${_sc_ov_build}_x86_64.tgz")
    set (_sc_ov_url "${_sc_ov_base}/linux/${_sc_ov_archive}")
    set (_sc_ov_sha256 "d47281e02644d93fa299853f660dec63b787f0a101aa393b4cde71d1c8c00c18")
endif ()

if (SC_OPENVINO_DIR)
    if (EXISTS "${SC_OPENVINO_DIR}/runtime/cmake/OpenVINOConfig.cmake")
        set (_sc_ov_cmake_dir "${SC_OPENVINO_DIR}/runtime/cmake")
    else ()
        set (_sc_ov_cmake_dir "${SC_OPENVINO_DIR}")
    endif ()
elseif (_sc_ov_archive)
    _sc_ai_fetch ("${_sc_ov_url}" "${_sc_ov_archive}" "${_sc_ov_sha256}" _sc_ov_tgz)
    set (_sc_ov_extract "${CMAKE_BINARY_DIR}/_deps/openvino")
    set (_sc_ov_stamp "${_sc_ov_extract}/.sc_extracted")
    set (_sc_ov_have "")
    if (EXISTS "${_sc_ov_stamp}")
        file (READ "${_sc_ov_stamp}" _sc_ov_have)
    endif ()
    if (NOT _sc_ov_have STREQUAL _sc_ov_sha256)
        file (REMOVE_RECURSE "${_sc_ov_extract}")
        file (MAKE_DIRECTORY "${_sc_ov_extract}")
        message (STATUS "AI runtimes: extracting ${_sc_ov_archive}")
        file (ARCHIVE_EXTRACT INPUT "${_sc_ov_tgz}" DESTINATION "${_sc_ov_extract}")
        file (WRITE "${_sc_ov_stamp}" "${_sc_ov_sha256}")
    endif ()
    string (REGEX REPLACE "\\.tgz$" "" _sc_ov_root_name "${_sc_ov_archive}")
    set (_sc_ov_cmake_dir "${_sc_ov_extract}/${_sc_ov_root_name}/runtime/cmake")
else ()
    message (WARNING "AI runtimes: no pinned OpenVINO archive for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}. "
                     "Pass -DSC_OPENVINO_DIR=<OpenVINO 2024.6 install> to enable the AI features; "
                     "they are disabled in this build.")
    return ()
endif ()

find_package (OpenVINO 2024.6 REQUIRED COMPONENTS Runtime
              PATHS "${_sc_ov_cmake_dir}" NO_DEFAULT_PATH)
if (OpenVINO_VERSION VERSION_LESS 2024.6 OR NOT OpenVINO_VERSION VERSION_LESS 2024.7)
    message (FATAL_ERROR "AI runtimes: OpenVINO ${OpenVINO_VERSION} found in ${_sc_ov_cmake_dir}; "
                        "SpectroCraft is pinned to 2024.6.")
endif ()

# Runtime files the app needs, the same set as on Windows (openvino.dll,
# openvino_intel_cpu_plugin.dll, openvino_ir_frontend.dll, tbb12.dll). Each
# is copied under its SONAME / install name, the name the loader and
# OpenVINO's own plugin and frontend discovery look for:
#  - libopenvino.<2460>.(dylib|so)  core, linked by the app;
#  - the CPU plugin (ARM on Apple Silicon, Intel elsewhere), found by
#    OpenVINO in its own folder (compiled-in plugin list, no plugins.xml);
#  - libopenvino_ir_frontend.<2460>.*  scanned for in the same folder;
#  - libtbb.12, imported by the core and the CPU plugin.
get_target_property (_sc_ov_core openvino::runtime IMPORTED_LOCATION_RELEASE)
if (NOT _sc_ov_core)
    get_target_property (_sc_ov_core openvino::runtime IMPORTED_LOCATION)
endif ()
get_filename_component (_sc_ov_libdir "${_sc_ov_core}" DIRECTORY)
get_filename_component (_sc_ov_root "${_sc_ov_cmake_dir}/.." ABSOLUTE)

if (APPLE)
    file (GLOB _sc_ov_core_so  "${_sc_ov_libdir}/libopenvino.[0-9][0-9][0-9][0-9].dylib")
    file (GLOB _sc_ov_ir_so    "${_sc_ov_libdir}/libopenvino_ir_frontend.[0-9][0-9][0-9][0-9].dylib")
    set (_sc_tbb_name "libtbb.12.dylib")
else ()
    file (GLOB _sc_ov_core_so  "${_sc_ov_libdir}/libopenvino.so.[0-9][0-9][0-9][0-9]")
    file (GLOB _sc_ov_ir_so    "${_sc_ov_libdir}/libopenvino_ir_frontend.so.[0-9][0-9][0-9][0-9]")
    set (_sc_tbb_name "libtbb.so.12")
endif ()
file (GLOB _sc_ov_cpu_so "${_sc_ov_libdir}/libopenvino_*_cpu_plugin.so")

set (_sc_tbb_so "")
foreach (dir "${_sc_ov_root}/3rdparty/tbb/lib" "${_sc_ov_libdir}")
    if (NOT _sc_tbb_so AND EXISTS "${dir}/${_sc_tbb_name}")
        set (_sc_tbb_so "${dir}/${_sc_tbb_name}")
    endif ()
endforeach ()

foreach (v _sc_ov_core_so _sc_ov_ir_so _sc_ov_cpu_so)
    list (LENGTH ${v} n)
    if (NOT n EQUAL 1)
        message (FATAL_ERROR "AI runtimes: unexpected OpenVINO layout in ${_sc_ov_libdir} "
                            "(${v}: '${${v}}').")
    endif ()
endforeach ()

set (_sc_ov_files "")
foreach (f IN LISTS _sc_ov_core_so _sc_ov_ir_so _sc_ov_cpu_so)
    get_filename_component (n "${f}" NAME)
    list (APPEND _sc_ov_files "${f}|${n}")
endforeach ()
if (_sc_tbb_so)
    list (APPEND _sc_ov_files "${_sc_tbb_so}|${_sc_tbb_name}")
else ()
    message (WARNING "AI runtimes: ${_sc_tbb_name} not found next to OpenVINO; the app will "
                     "rely on a system-wide oneTBB at run time.")
endif ()

add_library (sc_openvino INTERFACE)
target_link_libraries (sc_openvino INTERFACE openvino::runtime)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _sc_tbb_so)
    # GNU ld resolves libopenvino's own NEEDED entries (libtbb.so.12) when it
    # links the executable; the archive's libraries have no run path, so tell
    # ld where TBB is. Link time only: nothing is written into the binary.
    get_filename_component (_sc_tbb_dir "${_sc_tbb_so}" DIRECTORY)
    target_link_options (sc_openvino INTERFACE "LINKER:-rpath-link,${_sc_tbb_dir}")
endif ()

# ---------------------------------------------------------------------------
# whisper.cpp b5454 (= whisper.cpp 1.9.5, commit below)
# ---------------------------------------------------------------------------
set (SC_WHISPER_TAG     "b5454")
set (SC_WHISPER_COMMIT  "d1be6fde11ac6e0407606b4e42fe72d34add8037")
set (SC_WHISPER_SHA256  "645445da884df6dbd52b30f82d7be7c01d1767c938714e477e786b88d529e9f4")

# FETCHCONTENT_SOURCE_DIR_SC_WHISPER=<checkout> builds from a local copy instead.
FetchContent_Declare (sc_whisper
    URL      "https://github.com/ggml-org/whisper.cpp/archive/${SC_WHISPER_COMMIT}.tar.gz"
    URL_HASH SHA256=${SC_WHISPER_SHA256}
    DOWNLOAD_DIR "${SC_AI_DOWNLOAD_DIR}"
    DOWNLOAD_NAME "whisper.cpp-${SC_WHISPER_TAG}-${SC_WHISPER_COMMIT}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Populate only: the libraries are built by the ExternalProject below.
    SOURCE_SUBDIR "sc-do-not-add-subdirectory")
FetchContent_MakeAvailable (sc_whisper)
set (_sc_whisper_src "${sc_whisper_SOURCE_DIR}")

# The app compiles against the vendored b5454 headers and calls into the
# library through function pointers: they must be the very same headers.
foreach (pair "include/whisper.h|include/whisper.h" "include/ggml.h|ggml/include/ggml.h")
    string (REPLACE "|" ";" pair "${pair}")
    list (GET pair 0 vendored)
    list (GET pair 1 fetched)
    file (SHA256 "${CMAKE_SOURCE_DIR}/extern/whisper-b5454/${vendored}" h1)
    file (SHA256 "${_sc_whisper_src}/${fetched}" h2)
    if (NOT h1 STREQUAL h2)
        message (FATAL_ERROR "AI runtimes: extern/whisper-b5454/${vendored} differs from ${fetched} "
                            "of whisper.cpp ${SC_WHISPER_TAG} - the C API structs would not match.")
    endif ()
endforeach ()

set (_sc_whisper_bin   "${CMAKE_BINARY_DIR}/_deps/whisper-build")
set (_sc_whisper_stamp "${CMAKE_BINARY_DIR}/_deps/whisper-stamp")
set (_sc_whisper_args
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DBUILD_SHARED_LIBS:BOOL=ON
    -DWHISPER_BUILD_TESTS:BOOL=OFF
    -DWHISPER_BUILD_EXAMPLES:BOOL=OFF
    -DWHISPER_BUILD_SERVER:BOOL=OFF
    -DWHISPER_CURL:BOOL=OFF
    -DWHISPER_SDL2:BOOL=OFF
    -DGGML_NATIVE:BOOL=OFF      # portable binaries, not tuned to the build machine
    -DGGML_OPENMP:BOOL=OFF      # ggml's own thread pool: no libgomp / libomp to ship
    -DGGML_CCACHE:BOOL=OFF
    # Final run paths straight from the build tree: the libraries find each
    # other in the folder they are copied to, nothing points at the build tree.
    -DCMAKE_BUILD_WITH_INSTALL_RPATH:BOOL=ON
    -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER})
if (APPLE)
    # Metal + Accelerate (BLAS) stay at their ggml defaults; transcription runs
    # on the CPU (use_gpu = false), which Accelerate speeds up.
    list (APPEND _sc_whisper_args
        -DCMAKE_INSTALL_RPATH:STRING=@loader_path
        -DCMAKE_OSX_ARCHITECTURES:STRING=${_sc_arch})
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        list (APPEND _sc_whisper_args -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif ()
    if (CMAKE_OSX_SYSROOT)
        list (APPEND _sc_whisper_args -DCMAKE_OSX_SYSROOT:STRING=${CMAKE_OSX_SYSROOT})
    endif ()
else ()
    list (APPEND _sc_whisper_args -DCMAKE_INSTALL_RPATH:STRING=$ORIGIN)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        # Same scheme as the official Windows binaries: one CPU backend per
        # x86 micro-architecture, the best one picked at run time by
        # ggml_backend_load_all_from_path (called by WhisperRuntime).
        list (APPEND _sc_whisper_args
            -DGGML_BACKEND_DL:BOOL=ON
            -DGGML_CPU_ALL_VARIANTS:BOOL=ON)
    endif ()
endif ()

ExternalProject_Add (sc_whisper_runtime
    SOURCE_DIR      "${_sc_whisper_src}"
    BINARY_DIR      "${_sc_whisper_bin}"
    STAMP_DIR       "${_sc_whisper_stamp}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND  ""
    CMAKE_CACHE_ARGS ${_sc_whisper_args}
    # Only the C API library and what it needs (ggml, ggml-base, backends).
    BUILD_COMMAND   ${CMAKE_COMMAND} --build "${_sc_whisper_bin}" --target whisper --config Release
    INSTALL_COMMAND ""
    LOG_CONFIGURE   ON
    LOG_OUTPUT_ON_FAILURE ON)

# ---------------------------------------------------------------------------
# Run-time copy manifest + sc_enable_ai()
# ---------------------------------------------------------------------------
set (SC_AI_RUNTIME_MANIFEST "${CMAKE_BINARY_DIR}/sc_ai_runtime_manifest.cmake")
file (WRITE "${SC_AI_RUNTIME_MANIFEST}.in"
    "# Generated by cmake/ScAiRuntimes.cmake - read by cmake/ScCopyAiRuntime.cmake.\n"
    "set (SC_OV_FILES \"${_sc_ov_files}\")\n"
    "set (SC_WHISPER_LIB_DIR \"${_sc_whisper_bin}/bin\")\n")
configure_file ("${SC_AI_RUNTIME_MANIFEST}.in" "${SC_AI_RUNTIME_MANIFEST}" COPYONLY)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # JUCE needs libcurl for HTTPS downloads on Linux (libcurl4-openssl-dev).
    find_package (PkgConfig REQUIRED)
    pkg_check_modules (SC_LIBCURL libcurl)
    if (NOT SC_LIBCURL_FOUND)
        message (FATAL_ERROR "AI features on Linux download their models with libcurl: "
                            "install libcurl4-openssl-dev (or configure with -DSPECTRO_ENABLE_AI=OFF).")
    endif ()
    set (SC_JUCE_NEEDS_CURL TRUE)
endif ()

function (sc_enable_ai tgt)
    target_compile_definitions (${tgt} PRIVATE SC_ENABLE_AI=1)
    target_link_libraries (${tgt} PRIVATE sc_openvino)
    add_dependencies (${tgt} sc_whisper_runtime)

    get_target_property (is_bundle ${tgt} MACOSX_BUNDLE)
    if (APPLE AND is_bundle)
        set (dest "$<TARGET_BUNDLE_CONTENT_DIR:${tgt}>/Frameworks")
        set (rpath "@executable_path/../Frameworks")
    elseif (APPLE)
        set (dest "$<TARGET_FILE_DIR:${tgt}>")
        set (rpath "@executable_path")
    else ()
        set (dest "$<TARGET_FILE_DIR:${tgt}>")
        set (rpath "$ORIGIN")
        # DT_RPATH, not DT_RUNPATH: the OpenVINO libraries carry no run path of
        # their own, and only the executable's DT_RPATH also applies to the
        # dependencies of its dependencies (libopenvino -> libtbb.so.12).
        target_link_options (${tgt} PRIVATE "LINKER:--disable-new-dtags")
    endif ()
    # Only the bundled location: the build tree never ends up in the binary.
    set_target_properties (${tgt} PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "${rpath}")

    # Relink (and so re-copy) when the runtime set or the whisper build changes.
    set_property (TARGET ${tgt} APPEND PROPERTY LINK_DEPENDS
        "${SC_AI_RUNTIME_MANIFEST}"
        "${_sc_whisper_stamp}/sc_whisper_runtime-done")

    add_custom_command (TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                "-DDEST=${dest}"
                "-DMANIFEST=${SC_AI_RUNTIME_MANIFEST}"
                -P "${CMAKE_SOURCE_DIR}/cmake/ScCopyAiRuntime.cmake"
        COMMENT "Copying the OpenVINO and whisper.cpp runtime libraries for ${tgt}"
        VERBATIM)
endfunction ()

message (STATUS "AI runtimes: OpenVINO ${OpenVINO_VERSION} (${_sc_ov_libdir}), "
                "whisper.cpp ${SC_WHISPER_TAG} (${SC_WHISPER_COMMIT})")
set (SC_AI_RUNTIMES_OK TRUE)
