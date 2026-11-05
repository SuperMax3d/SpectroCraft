// SpectroCraft — runtime DLL search path setup (Windows).
//
// All bundled runtime DLLs (OpenVINO, whisper.cpp/ggml, FFmpeg) live in a
// "SpectroCraft" subfolder next to the executable. This keeps the app folder
// clean and fully portable: no installation, no environment variables — the
// app discovers its own DLL location at startup from its own path.
//
// setupRuntimeDllSearchPath() adds that subfolder to the process-wide DLL
// search path via SetDllDirectory(). Windows always searches the executable's
// directory first (SafeDllSearchMode), so old layouts with the DLLs directly
// beside the exe keep working unchanged; when both are present, the copy in
// the exe directory wins. Call it once at startup, before any runtime DLL is
// loaded or delay-load resolved.
#pragma once

#if defined(_WIN32)
    // JUCE's headers do NOT pull in windows.h on their own, so this may be
    // the first windows.h include of a translation unit: keep NOMINMAX set so
    // the min/max macros never leak into later C++ headers (e.g. OpenVINO's),
    // and clear them if an earlier header left them defined.
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #undef max
    #undef min
    #include <cstdio>
    #include <cstring>

namespace sc {

inline void setupRuntimeDllSearchPath()
{
    char modPath [MAX_PATH] = {};
    if (::GetModuleFileNameA (nullptr, modPath, MAX_PATH) == 0)
        return;   // cannot locate ourselves: nothing to add

    // Strip the file name to get the executable's directory.
    for (char* p = modPath + ::strlen (modPath); p > modPath; --p)
        if (*p == '\\' || *p == '/') { *p = '\0'; break; }

    char sub [MAX_PATH];
    std::snprintf (sub, sizeof sub, "%s\\SpectroCraft", modPath);

    const DWORD attrs = ::GetFileAttributesA (sub);
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return;   // no subfolder: old flat layout, the default search order is enough

    wchar_t subW [MAX_PATH];
    ::MultiByteToWideChar (CP_ACP, 0, sub, -1, subW, MAX_PATH);
    ::SetDllDirectoryW (subW);
}

} // namespace sc

#endif   // _WIN32
