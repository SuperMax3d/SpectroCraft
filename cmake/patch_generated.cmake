# patch_generated.cmake - apply the freshly bumped version to JUCE's generated
# resources.rc *after* juceaide has written it but *before* it is compiled.
#
#   RC_FILE      : <JuceLibraryCode>/SpectroCraft_resources.rc  (VS_VERSION_INFO)
#   VERSION_FILE : <build>/generated/SC_VERSION.txt            (written by bump_version.cmake)
#   STAMP_FILE   : stamp written when done (drives the custom command)
#
# JuceHeader.h is deliberately NOT patched any more: it is included by almost
# every translation unit, so rewriting its ProjectInfo::versionString on every
# build (the version is bumped on every build) forced a full rebuild each time.
# Code that needs the bumped version uses SC_VERSION_STRING from the generated
# Version.h instead. Files are only rewritten when their content changes.
#
# This exists because this environment's rc.exe ignores /D preprocessor
# definitions, so JUCE's JUCE_USER_DEFINED_RC_FILE mechanism cannot be used.

cmake_minimum_required(VERSION 3.22)

if(NOT RC_FILE OR NOT VERSION_FILE OR NOT STAMP_FILE)
    message(FATAL_ERROR "patch_generated.cmake: missing -D arguments")
endif()

file(STRINGS "${VERSION_FILE}" new_version LIMIT_COUNT 1)
string(STRIP "${new_version}" new_version)

if(NOT new_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "patch_generated.cmake: bad version '${new_version}'")
endif()

string(REPLACE "." "," new_comma "${new_version}")

# --- resources.rc ----------------------------------------------------------
if(NOT EXISTS "${RC_FILE}")
    message(FATAL_ERROR "patch_generated.cmake: ${RC_FILE} not found (juceaide did not run?)")
endif()

file(READ "${RC_FILE}" rc)
# juceaide writes a 3-part version (e.g. 1,0,42); accept an optional 4th part.
string(REGEX REPLACE "(FILEVERSION[ ]+)[0-9]+,[0-9]+,[0-9]+(,[0-9]+)?"
       "\\1${new_comma}" rc "${rc}")
string(REGEX REPLACE "(PRODUCTVERSION[ ]+)[0-9]+,[0-9]+,[0-9]+(,[0-9]+)?"
       "\\1${new_comma}" rc "${rc}")
string(REGEX REPLACE "(\"FileVersion\",[ ]*\")[0-9]+\.[0-9]+\.[0-9]+"
       "\\1${new_version}" rc "${rc}")
string(REGEX REPLACE "(\"ProductVersion\",[ ]*\")[0-9]+\.[0-9]+\.[0-9]+"
       "\\1${new_version}" rc "${rc}")

# --- application icon --------------------------------------------------------
# Append an ICON statement so the exe carries a real PE icon resource
# (RT_ICON + RT_GROUP_ICON). Verified with this toolchain's rc.exe: an ICON
# statement emits correct RT_ICON/RT_GROUP_ICON entries, which Windows Explorer
# and the taskbar use for the .exe file icon. (An earlier note claiming rc.exe
# emitted RT_CURSOR instead was wrong.)
#
# Idempotency: any previously appended IDI_APP line(s) always sit at the very
# end of the file (juceaide never emits them), so truncate everything from the
# first IDI_APP onward. This avoids regex newline matching entirely, which is
# unreliable in this CMake version's regex engine.
string(FIND "${rc}" "IDI_APP" icon_pos)
if (icon_pos GREATER 0)
    string(SUBSTRING "${rc}" 0 ${icon_pos} rc)
endif()
# Normalise the end to exactly one newline so re-running produces an identical
# file (otherwise every run added a blank line and the rc was always rewritten).
string(LENGTH "${rc}" rc_len)
while(rc_len GREATER 0)
    math(EXPR rc_last "${rc_len} - 1")
    string(SUBSTRING "${rc}" ${rc_last} 1 rc_ch)
    if(NOT (rc_ch STREQUAL "\n" OR rc_ch STREQUAL "\r"))
        break()
    endif()
    string(SUBSTRING "${rc}" 0 ${rc_last} rc)
    set(rc_len ${rc_last})
endwhile()
string(APPEND rc "\n")
if(DEFINED ICON_FILE AND EXISTS "${ICON_FILE}")
    get_filename_component (icon_abs "${ICON_FILE}" ABSOLUTE)
    string(REPLACE "\\" "/" icon_path "${icon_abs}")
    string(APPEND rc "\nIDI_APP ICON \"${icon_path}\"\n")
else()
    message(WARNING "patch_generated.cmake: ${ICON_FILE} not found, exe will have no PE icon resource")
endif()

file(READ "${RC_FILE}" rc_current)
if(NOT rc_current STREQUAL rc)
    file(WRITE "${RC_FILE}" "${rc}")
endif()

# --- stamp -------------------------------------------------------------------
file(WRITE "${STAMP_FILE}" "patched ${new_version}\n")
message(STATUS "SpectroCraft: patched generated resources.rc to ${new_version}")
