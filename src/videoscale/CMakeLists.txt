# Video picture scaler core (Lanczos / Bicubic / Nearest): include/VideoScalerCore.h.
#
# It runs for every video frame shown, and an unoptimised build is about 70x
# slower (100-250 ms per frame instead of 1-4 ms), so it is compiled optimised
# in EVERY configuration, Debug included - the macOS / Linux CI artefacts are
# Debug builds. It is plain C++ without JUCE, so nothing it shares with the
# app depends on these flags. The changes below are directory-scoped: they
# reach this library only.
if (MSVC)
    # /RTC1 forbids optimisation; the debug CRT (/MDd, _DEBUG) stays, so the
    # library still links with a Debug app.
    string (REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string (REPLACE "/Od" "/O2" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string (REPLACE "/Ob0" "/Ob2" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
else()
    string (APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")
endif()

add_library (sc_video_scaler_core STATIC ${CMAKE_CURRENT_SOURCE_DIR}/VideoScalerCore.cpp)
target_include_directories (sc_video_scaler_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set_target_properties (sc_video_scaler_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package (Threads REQUIRED)
target_link_libraries (sc_video_scaler_core PUBLIC Threads::Threads)
