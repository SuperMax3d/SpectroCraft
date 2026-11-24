// SpectroCraft - which file dialogs to show (issue #71); see include/FileDialogs.h
#include "FileDialogs.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shobjidl.h>

namespace
{
// Creates and releases one COM object. A plain function with no C++ object
// to unwind, so it can catch a structured exception: creating the file dialog
// crashed with STATUS_NO_MEMORY on some systems instead of failing.
bool canCreate (REFCLSID clsid, REFIID iid) noexcept
{
    void* object = nullptr;
    HRESULT hr = E_FAIL;
   #if defined (_MSC_VER)
    __try
    {
        hr = ::CoCreateInstance (clsid, nullptr, CLSCTX_INPROC_SERVER, iid, &object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
   #else
    hr = ::CoCreateInstance (clsid, nullptr, CLSCTX_INPROC_SERVER, iid, &object);
   #endif
    if (FAILED (hr) || object == nullptr)
        return false;
    static_cast<IUnknown*> (object)->Release();
    return true;
}

bool systemFileDialogWorks()
{
    // COM is normally up on the message thread already (S_FALSE then); keep
    // the calls balanced either way.
    const HRESULT init = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    const bool ok = canCreate (CLSID_FileOpenDialog, IID_IFileOpenDialog)
                    && canCreate (CLSID_FileSaveDialog, IID_IFileSaveDialog);
    if (SUCCEEDED (init))
        ::CoUninitialize();
    return ok;
}
} // namespace
#endif

namespace sc::files
{
bool useNativeDialogsFor (const juce::String& envValue, bool systemDialogWorks)
{
    if (envValue.trim() == "0")
        return false;
    return systemDialogWorks;
}

bool useNativeDialogs()
{
   #if JUCE_WINDOWS
    static const bool works = systemFileDialogWorks();
   #else
    constexpr bool works = true;
   #endif
    return useNativeDialogsFor (juce::SystemStats::getEnvironmentVariable ("SC_NATIVE_FILE_DIALOGS", {}), works);
}
} // namespace sc::files
