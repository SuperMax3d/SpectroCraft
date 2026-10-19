# OpenVINO runtime (Apache-2.0) — vendored C++ headers + Windows x64 DLLs.
# See extern/OpenVINO/README.md for provenance and licence notes.

# NOTE: CMAKE_CURRENT_SOURCE_DIR inside a function is the *caller's* directory,
# and sibling subdirectory scopes do not propagate variables sideways, so
# sc_enable_ai() below references this folder via its fixed project-relative
# path instead.

add_library(sc_openvino INTERFACE)
target_include_directories (sc_openvino INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (WIN32 AND MSVC)
    # Import library generated from openvino.dll's export table (MSVC-mangled
    # C++ API). Only the symbols actually referenced get pulled in.
    target_link_libraries (sc_openvino INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/openvino.lib
        wininet)   # ModelManager downloads models over HTTPS on Windows
else ()
    message (WARNING "OpenVINO runtime is only vendored for Windows x64; AI features will be disabled.")
endif ()

# sc_enable_ai(<target>) — turn the AI feature set on for a target:
#  * defines SC_ENABLE_AI=1 (code guards)
#  * links the OpenVINO interface library
#  * copies the runtime DLLs next to the built executable so the standard
#    Windows DLL search order finds openvino.dll + CPU plugin + IR frontend.
function (sc_enable_ai tgt)
    if (NOT WIN32 OR NOT MSVC)
        return ()
    endif ()

    target_compile_definitions (${tgt} PRIVATE SC_ENABLE_AI=1)
    target_link_libraries (${tgt} PRIVATE sc_openvino)

    # Delay-load openvino.dll so the executable starts even when the OpenVINO
    # runtime DLLs are not installed next to it. OvRuntime::init() probes the
    # DLL and reports a clear error; only the AI features degrade.
    # delayimp.lib provides the delay-load helper symbols.
    set_property (TARGET ${tgt} APPEND PROPERTY LINK_OPTIONS
        "/DELAYLOAD:openvino.dll" "delayimp.lib")

    add_custom_command (TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/extern/OpenVINO/bin/openvino.dll
            ${CMAKE_SOURCE_DIR}/extern/OpenVINO/bin/openvino_intel_cpu_plugin.dll
            ${CMAKE_SOURCE_DIR}/extern/OpenVINO/bin/openvino_ir_frontend.dll
            ${CMAKE_SOURCE_DIR}/extern/OpenVINO/bin/tbb12.dll
            $<TARGET_FILE_DIR:${tgt}>
        COMMENT "Copying OpenVINO runtime DLLs next to ${tgt}")
endfunction ()
