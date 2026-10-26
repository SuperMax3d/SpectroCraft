# patch_generated.cmake - apply the freshly bumped version to JUCE's generated
# files *after* juceaide has written them but *before* they are compiled.
#
#   RC_FILE      : <JuceLibraryCode>/SpectroCraft_resources.rc  (VS_VERSION_INFO)
#   HEADER_FILE  : <JuceLibraryCode>/JuceHeader.h              (ProjectInfo::versionString)
#   VERSION_FILE : <build>/generated/SC_VERSION.txt            (written by bump_version.cmake)
#   STAMP_FILE   : stamp written when done (drives the custom command)
#
# This exists because this environment's rc.exe ignores /D preprocessor
# definitions, so JUCE's JUCE_USER_DEFINED_RC_FILE mechanism cannot be used.

cmake_minimum_required(VERSION 3.22)

if(NOT RC_FILE OR NOT HEADER_FILE OR NOT VERSION_FILE OR NOT STAMP_FILE)
    message(FATAL_ERROR "patch_generated.cmake: missing -D arguments")
endif()

file(STRINGS "${VERSION_FILE}" new_version LIMIT_COUNT 1)
string(STRIP "${new_version}" new_version)

if(NOT new_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "patch_generated.cmake: bad version '${new_version}'")
endif()

string(REPLACE "." ";" vparts "${new_version}")
list(GET vparts 0 sc_maj)
list(GET vparts 1 sc_min)
list(GET vparts 2 sc_pat)
string(REPLACE "." "," new_comma "${new_version}")

# versionNumber = 0xMMmmpp (CMake's math() has no hex output, so convert manually)
math(EXPR vnum "(${sc_maj} * 65536 + ${sc_min} * 256 + ${sc_pat})")
set(hex_digits "0123456789ABCDEF")
set(vhex "")
math(EXPR tmp "${vnum}")
while(tmp GREATER 0)
    math(EXPR d "${tmp} % 16")
    string(SUBSTRING "${hex_digits}" ${d} 1 c)
    set(vhex "${c}${vhex}")
    math(EXPR tmp "${tmp} / 16")
endwhile()
if(vhex STREQUAL "")
    set(vhex "0")
endif()
string(LENGTH "${vhex}" vhex_len)
math(EXPR vhex_pad "6 - ${vhex_len}")
if(vhex_pad GREATER 0)
    string(SUBSTRING "000000" 0 ${vhex_pad} vhex_zeros)
    set(vhex "${vhex_zeros}${vhex}")
endif()
set(version_hex "0x${vhex}")

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
if(DEFINED ICON_FILE AND EXISTS "${ICON_FILE}")
    get_filename_component (icon_abs "${ICON_FILE}" ABSOLUTE)
    string(REPLACE "\\" "/" icon_path "${icon_abs}")
    string(APPEND rc "\nIDI_APP ICON \"${icon_path}\"\n")
else()
    message(WARNING "patch_generated.cmake: ${ICON_FILE} not found, exe will have no PE icon resource")
endif()

file(WRITE "${RC_FILE}" "${rc}")

# --- JuceHeader.h -----------------------------------------------------------
if(NOT EXISTS "${HEADER_FILE}")
    message(FATAL_ERROR "patch_generated.cmake: ${HEADER_FILE} not found (juceaide did not run?)")
endif()

file(READ "${HEADER_FILE}" hdr)
string(REGEX REPLACE "(versionString[ ]*=[ ]*\")[0-9]+\.[0-9]+\.[0-9]+(\")"
       "\\1${new_version}\\2" hdr "${hdr}")
string(REGEX REPLACE "(versionNumber[ ]*=[ ]*)0x[0-9a-fA-F]+"
       "\\1${version_hex}" hdr "${hdr}")
file(WRITE "${HEADER_FILE}" "${hdr}")

# --- stamp -------------------------------------------------------------------
file(WRITE "${STAMP_FILE}" "patched ${new_version}\n")
message(STATUS "SpectroCraft: patched generated resources.rc / JuceHeader.h to ${new_version}")
