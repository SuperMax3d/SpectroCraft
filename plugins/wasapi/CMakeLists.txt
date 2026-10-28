# SpectroCraft WASAPI recorder plugin (standard Recorder Plugin API v1).
# Builds wasapi_recorder.dll next to the app so it is found at startup in
# <exe dir>/recorders/.
add_library(sc_wasapi_recorder SHARED WasapiRecorder.cpp)

target_include_directories(sc_wasapi_recorder PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(sc_wasapi_recorder PROPERTIES
    OUTPUT_NAME wasapi_recorder
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

if (WIN32)
    target_link_libraries(sc_wasapi_recorder PRIVATE ole32)
endif()

# Ship the DLL in <exe dir>/recorders/ for both app and test artefact dirs.
add_custom_command(TARGET sc_wasapi_recorder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SpectroCraft>/recorders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sc_wasapi_recorder>
            $<TARGET_FILE_DIR:SpectroCraft>/recorders/wasapi_recorder.dll)
