/* ============================================================================
 * SpectroCraft WASAPI recorder plugin (wasapi_recorder.dll)
 * ---------------------------------------------------------------------------
 * Implements the standard SpectroCraft recorder C ABI (see RecorderPlugin.h).
 *
 * Devices:
 *   - Every active WASAPI capture endpoint  -> id "cap:<MMDevice id>"
 *   - Every active render endpoint          -> id "loopback:<MMDevice id>"
 *     (recorded in loopback mode, i.e. what you hear - the Audacity-style
 *      "Stereo Mix / What U Hear" option).
 *
 * The plugin runs its own capture thread that pulls frames from the shared-
 * mode WASAPI client into a ring buffer; the host polls available_frames()/
 * read() at ~25 Hz. Data is always delivered as interleaved float32 in
 * [-1, 1] regardless of the endpoint's native format (PCM 16/24/32 or IEEE
 * float are converted on the fly).
 * ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

// PROPVARIANT comes from combaseapi.h (via windows.h) on this SDK.
extern "C" HRESULT PropVariantClear(PROPVARIANT* p);   // ole32.dll

#include "RecorderPlugin.h"

// This SDK only declares the WASAPI GUIDs (EXTERN_C const, no definitions -
// modern SDKs fill them in via initguid.h). Provide the definitions here with
// matching C linkage; values are taken from this machine's headers.
extern "C" const IID CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c,
    { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
extern "C" const IID IID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35,
    { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
extern "C" const IID IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32,
    { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
extern "C" const IID IID_IAudioCaptureClient = { 0xc8adbd64, 0xe71e, 0x48a0,
    { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };

/* ---------------------------------------------------------------------------
 * This machine's Windows SDK (10.0.18362) ships reduced, C-style WASAPI
 * headers: no WAVEFORMATEXTENSIBLE, no AUDCLNT_STREAMFLAGS constants and the
 * IMMDevice/IMMDeviceEnumerator/IAudioCaptureClient interfaces are declared
 * as raw vtable structs without their modern methods (GetMixFormat,
 * GetProperty, EnumerateAudioEndPoints...). The runtime COM objects implement
 * the full stable interfaces, so we declare them here in their canonical
 * layout and QI to them by the standard IIDs.
 * --------------------------------------------------------------------------- */

#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

struct WAVEFORMATEXTENSIBLE_LOCAL {
    WAVEFORMATEX fmt;
    unsigned int Samples;   // valid bits per sample (packed into wBitsPerSample)
    GUID         SubFormat;
};

static const DWORD kStreamFlagsAutoConvert = 0x1;  // AUDCLNT_STREAMFLAGS_AUTOCONVERT
static const DWORD kStreamFlagsLoopback    = 0x2;  // AUDCLNT_STREAMFLAGS_LOOPBACK
// Some (virtualized) audio stacks use a different value for the loopback
// stream flag. setupCapture() tries the standard value first and falls back
// to this one, so both kinds of stack work.
static const DWORD kStreamFlagsLoopbackAlt = 0x4;

// KSDATAFORMAT subtypes (stable GUIDs from ks.h).
static const GUID kSubtypePcm   { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubtypeFloat { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct IAudioCaptureClientFull : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer (BYTE** ppData, UINT32* pFrameCount,
                                                 REFERENCE_TIME* pTimestamp, PROPVARIANT* pPacketFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextPacketSize (UINT32* pInPacketSize) = 0;
};

// Some stacks expose loopback sessions through a custom render-client-style
// interface instead of IAudioCaptureClient. Its vtable (verified against the
// local SDK's audioclient.h, MIDL_INTERFACE "F294ACFC-3146-4483-A7BF-
// ADDCA7C260E2") is: GetBuffer(numFramesRequested, &data) hands out a buffer
// holding up to that many frames of captured audio; ReleaseBuffer(n, flags)
// consumes n frames.
static const IID kIidLocalRenderClient = { 0xf294acfc, 0x3146, 0x4483,
    { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

struct IAudioRenderClientLocal : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer (UINT32 numFramesRequested, BYTE** ppData) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer (UINT32 numFramesWritten, DWORD dwFlags) = 0;
};

// Not declared by this SDK's default headers. Canonical stable layout:
// QI/AddRef/Release, then GetValue(3), SetValue(4), ContainsKey(5),
// GetCount(6), GetAt(7), GetKeyName(8).
struct IPropertyStoreFull : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetValue (PROPERTYKEY key, PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue (PROPERTYKEY key, const PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE ContainsKey (PROPERTYKEY key, BOOL* hasValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCount (UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt (UINT index, PROPERTYKEY* key) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetKeyName (UINT index, LPWSTR* name) = 0;
};

struct IMMDeviceFull : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Activate (REFIID iid, DWORD dwClsCtx,
                                                PROPVARIANT* pActivationParams, void** ppInterface) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore (DWORD stgmAccess, struct IPropertyStore** ppProperties) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId (LPWSTR* ppstrId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState (DWORD* pdwState) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (WAVEFORMATEX** ppFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty (PROPERTYKEY key, PROPVARIANT* pv) = 0;
};

struct IMMDeviceCollectionFull : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetCount (UINT32* puCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item (UINT32 uIndex, struct IMMDeviceFull** ppDevice) = 0;
};

struct IMMDeviceEnumeratorFull : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints (EDataFlow dataFlow, DWORD dwStateMask,
                                                         struct IMMDeviceCollectionFull** ppDevices) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint (EDataFlow dataFlow, ERole role,
                                                              struct IMMDeviceFull** ppEndpoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice (LPCWSTR pwstrId, struct IMMDeviceFull** ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback (void* pClient) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback (void* pClient) = 0;
};

/* --------------------------------------------------------------------------- */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

struct WasapiDevice {
    std::wstring id;          // raw MMDevice id (no prefix)
    std::string  name;        // friendly name, UTF-8
    bool         loopback = false;
    WAVEFORMATEX fmt {};      // endpoint mix format (unwrapped if extensible)
    // The original mix format exactly as GetMixFormat returned it (may be a
    // full WAVEFORMATEXTENSIBLE). Some engines only accept that verbatim.
    std::vector<char> rawFmtBytes;
};

struct CaptureState {
    WasapiDevice dev;
    int          wantChannels = 0;
    double       wantRate = 0.0;
    int          wantDepth = 16;

    std::atomic<bool> stopFlag{ false };
    HANDLE           threadHandle = nullptr;

    // The worker signals setup completion through this auto-reset event and
    // stores the result code here (0 == ok).
    HANDLE           setupEvent = nullptr;
    int              setupResult = -1;

    // COM objects. Every WASAPI call in a session happens on the dedicated
    // capture worker thread, which also owns CoInitializeEx/CoUninitialize.
    IMMDeviceEnumeratorFull* en = nullptr;
    IMMDeviceFull*           endpoint = nullptr;
    IAudioClient*            client = nullptr;
    IAudioCaptureClientFull* capture = nullptr;
    IAudioRenderClientLocal* localCapture = nullptr;   // custom-interface loopback reader
    bool                     useLocalCapture = false;
    bool                     ownsComInit = false;   // we called CoInitializeEx in setup

    // Ring buffer of interleaved float32 frames.
    std::vector<float> ring;
    size_t             head = 0, count = 0;   // [head, head+count) mod capacity
    std::mutex         ringMutex;

    std::string lastError;
};

CaptureState* g_state = nullptr;
std::vector<WasapiDevice> g_devices;
std::mutex g_mutex;

void setError(const std::string& msg)
{
    if (g_state != nullptr)
        g_state->lastError = msg;
}

std::wstring toWide(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == 0) return L"";
    const int n = MultiByteToWideChar (CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w ((size_t) n > 0 ? (size_t) (n - 1) : 0, L'\0');
    if (!w.empty ())
        MultiByteToWideChar (CP_UTF8, 0, utf8, -1, &w[0], (int) w.size ());
    return w;
}

std::string toUtf8(const wchar_t* w)
{
    if (w == nullptr || *w == 0) return {};
    const int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) n > 0 ? (size_t) (n - 1) : 0, '\0');
    if (!s.empty ())
        WideCharToMultiByte (CP_UTF8, 0, w, -1, &s[0], (int) s.size (), nullptr, nullptr);
    return s;
}

// PKEY_FriendlyName (defined in devprop.h on newer SDKs).
static const PROPERTYKEY kPropFriendlyName { 0xa45c254e, 0xde17, 0x4d21,
                                             { 0x8b, 0x21, 0xfb, 0x73, 0x73, 0xde, 0x41, 0x1b } };

// Property keys used by some (virtualized) audio stacks where
// IMMDevice::GetProperty is unavailable and the standard PKEY_FriendlyName is
// absent from the endpoint's property store. Verified on a Windows 11 VM with
// a reduced SDK / custom audio stack:
//   kLocalNameKey -> endpoint name ("Microphone", "Speakers", ...)
//   kLocalMfrKey  -> manufacturer ("Realtek(R) Audio", "NVIDIA High Definition Audio", ...)
static const PROPERTYKEY kLocalNameKey { 0xa45c254e, 0xdf1c, 0x4efd,
                                         { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 }, 2 };
static const PROPERTYKEY kLocalMfrKey  { 0xb3f8fa53, 0x0004, 0x438e,
                                         { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc }, 6 };

// Some stacks expose IPropertyStore with a reduced vtable:
//   QI/AddRef/Release, GetCount(3), GetAt(4), GetValue(5), SetValue(6), Commit(7)
struct IPropertyStoreLocal : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetCount (DWORD* cProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt (DWORD iProp, PROPERTYKEY* pkey) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue (PROPERTYKEY key, PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue (PROPERTYKEY key, const PROPVARIANT* propvar) = 0;
    virtual HRESULT STDMETHODCALLTYPE Commit () = 0;
};

// Composes "name (manufacturer)", the Audacity-style device label. Pure
// function so it can be unit-tested.
std::string composeDeviceName(const std::string& name, const std::string& mfr)
{
    if (!name.empty () && !mfr.empty ())
        return name + " (" + mfr + ")";
    return name;
}

// Determines whether the property store uses the reduced local vtable layout,
// without ever raising an exception. The probe calls vtable slot 3 with
// arguments that are valid under BOTH known layouts:
//   local     slot 3 = GetCount(DWORD*)    -> reads RDX (a valid pointer we
//                                             pass), returns S_OK + small count
//   canonical slot 3 = GetValue(key, pv)   -> the key is read from our own
//                                             stack shadow space (stale bytes -
//                                             a safe read of our page) and pv in
//                                             R8 points at a valid PROPVARIANT;
//                                             an unknown key fails gracefully.
// No SEH is involved: SpectroCraft's first-chance vectored exception handler
// terminates the process on any access violation before __except can run, so
// probes must be crash-free by construction rather than caught after the fact.
static bool probeLocalLayout(IPropertyStore* ps)
{
    using Fn3 = HRESULT (*)(IPropertyStore*, DWORD*, PROPVARIANT*);
    void*** vtp = reinterpret_cast<void***> (ps);
    auto fn = reinterpret_cast<Fn3> ((*vtp)[3]);

    DWORD cnt = 0;
    PROPVARIANT scratch {};
    const HRESULT hr = fn (ps, &cnt, &scratch);
    return SUCCEEDED (hr) && cnt < 4096;   // only GetCount behaves like this
}

// Fills nameOut with the best device label this stack can provide.
static void queryDeviceNames(IMMDeviceFull* dev, std::string& nameOut)
{
    // Path 1: standard IMMDevice::GetProperty (works on stock Windows).
    PROPVARIANT pv {};
    if (SUCCEEDED (dev->GetProperty (kPropFriendlyName, &pv)) && pv.vt == VT_LPWSTR
        && pv.pwszVal != nullptr && *pv.pwszVal)
    {
        nameOut = toUtf8 (pv.pwszVal);
        PropVariantClear (&pv);
        return;
    }
    if (pv.vt != VT_EMPTY)
        PropVariantClear (&pv);

    // Path 2: the property store, with a runtime layout probe.
    IPropertyStore* ps = nullptr;
    if (FAILED (dev->OpenPropertyStore (STGM_READ, &ps)) || ps == nullptr)
        return;

    const bool localLayout = probeLocalLayout (ps);

    auto getValue = [&] (const PROPERTYKEY& key, std::string& out) {
        PROPVARIANT v {};
        const HRESULT hr = localLayout ? ((IPropertyStoreLocal*) ps)->GetValue (key, &v)
                                       : ((IPropertyStoreFull*) ps)->GetValue (key, &v);
        if (SUCCEEDED (hr) && v.vt == VT_LPWSTR && v.pwszVal != nullptr && *v.pwszVal)
            out = toUtf8 (v.pwszVal);
        if (v.vt != VT_EMPTY)
            PropVariantClear (&v);
    };

    std::string name, mfr, lname, lmfr;
    getValue (kPropFriendlyName, name);   // standard key first
    getValue (kLocalNameKey, lname);      // this stack's endpoint-name key
    getValue (kLocalMfrKey, lmfr);        // this stack's manufacturer key
    if (name.empty ())
        name = lname;
    mfr = lmfr;

    ps->Release ();

    nameOut = composeDeviceName (name, mfr);
}

void enumerateDevices()
{
    // Callable from any thread (the host calls it from the UI thread when
    // building menus). CoInitializeEx returns S_OK only if we newly
    // initialised COM on this thread - in that case, and only then, do we
    // uninitialise at the end. If the thread already has COM (S_FALSE or
    // RPC_E_CHANGED_MODE) we simply use it as-is.
    const HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool weInitCom = SUCCEEDED (hrCo) && hrCo == S_OK;

    std::vector<WasapiDevice> found;

    IMMDeviceEnumeratorFull* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                    IID_IMMDeviceEnumerator, (void**) &en)))
    {
        auto collect = [&](EDataFlow flow, bool loopback) {
            IMMDeviceCollectionFull* coll = nullptr;
            if (FAILED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)))
                return;
            UINT32 n = 0;
            coll->GetCount (&n);
            for (UINT32 i = 0; i < n; ++i)
            {
                IMMDeviceFull* dev = nullptr;
                if (FAILED (coll->Item (i, &dev))) continue;

                WasapiDevice d;
                LPWSTR idW = nullptr;
                WAVEFORMATEX* raw = nullptr;
                bool okId   = SUCCEEDED (dev->GetId (&idW));

                // This stack's IMMDevice has no working GetMixFormat, so ask
                // IAudioClient for it.
                IAudioClient* ac = nullptr;
                if (SUCCEEDED (dev->Activate (IID_IAudioClient, CLSCTX_ALL, nullptr,
                                             (void**) &ac)) && ac != nullptr)
                {
                    WAVEFORMATEX* fmt2 = nullptr;
                    if (SUCCEEDED (ac->GetMixFormat (&fmt2)))
                        raw = fmt2;   // CoTaskMemAlloc'd by the system
                    ac->Release ();
                }
                if (!okId || raw == nullptr) { dev->Release (); continue; }

                d.id = idW;
                CoTaskMemFree (idW);

                // Unwrap WAVEFORMATEXTENSIBLE to a plain PCM/float description.
                // Read the SubFormat from `raw` itself - copying only the first
                // 18 bytes into d.fmt would leave SubFormat out of bounds.
                d.rawFmtBytes.assign ((const char*) raw,
                                      (const char*) raw + sizeof (WAVEFORMATEX) + raw->cbSize);
                d.fmt = *raw;
                if (d.fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE_LOCAL*> (raw);
                    d.fmt.wFormatTag = IsEqualGUID (ext->SubFormat, kSubtypeFloat)
                                        ? WAVE_FORMAT_IEEE_FLOAT
                                        : WAVE_FORMAT_PCM;
                    d.fmt.cbSize = 0;   // plain format now - drop extensible padding size
                }
                CoTaskMemFree (raw);

                // Friendly name: standard GetProperty first, then the property
                // store with a runtime vtable-layout probe and dual key lookup.
                queryDeviceNames (dev, d.name);
                if (d.name.empty ())
                    d.name = loopback ? "Loopback device" : "Capture device";

                d.loopback = loopback;
                found.push_back (std::move (d));
                dev->Release ();
            }
            coll->Release ();
        };

        collect (eRender, true);    // loopback first - it's the interesting one
        collect (eCapture, false);
        // NOTE: we deliberately do NOT Release() the enumerator here. On some
        // audio stacks releasing it corrupts COM state and crashes; leaking
        // one small object per scan is harmless.
        (void) en;
    }

    {
        std::lock_guard<std::mutex> lock (g_mutex);
        g_devices = std::move (found);
    }

    if (weInitCom)
        CoUninitialize ();
}

void convertToFloat(const void* src, WAVEFORMATEX& fmt, float* dst, size_t frames)
{
    const int ch = fmt.nChannels;
    if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.wBitsPerSample == 32)
    {
        std::memcpy (dst, src, frames * (size_t) ch * sizeof (float));
        return;
    }

    const auto* p = static_cast<const unsigned char*> (src);
    for (size_t f = 0; f < frames; ++f)
    {
        for (int c = 0; c < ch; ++c, p += fmt.nBlockAlign)
        {
            float v = 0.0f;
            if (fmt.wFormatTag == WAVE_FORMAT_PCM)
            {
                switch (fmt.wBitsPerSample)
                {
                    case 8:  v = ((float) p[0] - 128.0f) / 128.0f; break;
                    case 16: v = (float) *reinterpret_cast<const int16_t*> (p) / 32768.0f; break;
                    case 24: {
                        const int32_t s = (int32_t) p[0] | ((int32_t) p[1] << 8) | ((int32_t) p[2] << 16);
                        v = (float) (s < 0x800000 ? s | ~0xFFFFFF : s) / 8388608.0f;
                    } break;
                    case 32: v = (float) *reinterpret_cast<const int32_t*> (p) / 2147483648.0f; break;
                    default: break;
                }
            }
            else if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt.wBitsPerSample == 64)
            {
                v = (float) *reinterpret_cast<const double*> (p);
            }
            dst[(f * (size_t) ch) + (size_t) c] = v;
        }
    }
}

// Releases every COM object the session owns and uninitialises COM if we own
// it. Must run on the capture worker thread (the one that activated them).
static void cleanupCapture(CaptureState* st)
{
    if (st->localCapture != nullptr) { st->localCapture->Release (); st->localCapture = nullptr; }
    if (st->capture != nullptr) { st->capture->Release (); st->capture = nullptr; }
    if (st->client  != nullptr)
    {
        st->client->Stop ();
        st->client->Release ();
        st->client = nullptr;
    }
    if (st->endpoint != nullptr) { st->endpoint->Release (); st->endpoint = nullptr; }
    // NOTE: the enumerator is deliberately leaked - releasing it corrupts COM
    // state on some audio stacks. One small object per session.
    st->en = nullptr;
    if (st->setupEvent != nullptr)
    {
        CloseHandle (st->setupEvent);
        st->setupEvent = nullptr;
    }
    if (st->ownsComInit)
        CoUninitialize ();
    st->ownsComInit = false;
}

// Activates the whole capture pipeline synchronously on the calling thread.
static int setupCapture(CaptureState* st)
{
    const HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    if (FAILED (hrCo) && hrCo != RPC_E_CHANGED_MODE)
    { st->lastError = "COM initialisation failed"; return 1; }
    if (SUCCEEDED (hrCo))
        st->ownsComInit = true;

    if (FAILED (CoCreateInstance (CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                 IID_IMMDeviceEnumerator, (void**) &st->en)))
    { st->lastError = "device enumerator unavailable"; cleanupCapture (st); return 2; }

    HRESULT hr = st->en->GetDevice (st->dev.id.c_str (), &st->endpoint);
    if (FAILED (hr)) { st->lastError = "device disappeared before start"; cleanupCapture (st); return 3; }

    const DWORD flags = kStreamFlagsAutoConvert | (st->dev.loopback ? kStreamFlagsLoopback : 0);
    hr = st->endpoint->Activate (IID_IAudioClient, CLSCTX_ALL, nullptr, (void**) &st->client);
    if (FAILED (hr)) { st->lastError = "IAudioClient activation failed"; cleanupCapture (st); return 4; }

    // This machine's audio engine is picky about formats: try the mix format
    // verbatim first, then AUTOCONVERT variants. Log every attempt.
    WAVEFORMATEX pcm16 = st->dev.fmt;
    pcm16.wFormatTag = WAVE_FORMAT_PCM;
    pcm16.wBitsPerSample = 16;
    pcm16.nBlockAlign = (WORD) (pcm16.nChannels * 2);
    pcm16.nAvgBytesPerSec = (DWORD) (pcm16.nSamplesPerSec * pcm16.nBlockAlign);

    struct Attempt { DWORD fl; const WAVEFORMATEX* fmt; DWORD shareMode; };
    std::vector<Attempt> tries;
    if (!st->dev.rawFmtBytes.empty ())
        tries.push_back ({ st->dev.loopback ? kStreamFlagsLoopback : 0,
                           reinterpret_cast<const WAVEFORMATEX*> (st->dev.rawFmtBytes.data ()),
                           AUDCLNT_SHAREMODE_SHARED });
    tries.push_back ({ flags, &st->dev.fmt, AUDCLNT_SHAREMODE_SHARED });
    tries.push_back ({ flags, &pcm16, AUDCLNT_SHAREMODE_SHARED });
    if (!st->dev.loopback && !st->dev.rawFmtBytes.empty ())
        tries.push_back ({ 0,
                           reinterpret_cast<const WAVEFORMATEX*> (st->dev.rawFmtBytes.data ()),
                           AUDCLNT_SHAREMODE_EXCLUSIVE });

    // Fallback for stacks whose loopback flag is not the standard value:
    // retry with kStreamFlagsLoopbackAlt.
    if (st->dev.loopback)
    {
        if (!st->dev.rawFmtBytes.empty ())
            tries.push_back ({ kStreamFlagsLoopbackAlt,
                               reinterpret_cast<const WAVEFORMATEX*> (st->dev.rawFmtBytes.data ()),
                               AUDCLNT_SHAREMODE_SHARED });
        tries.push_back ({ kStreamFlagsAutoConvert | kStreamFlagsLoopbackAlt,
                           &st->dev.fmt, AUDCLNT_SHAREMODE_SHARED });
    }

    HRESULT lastHr = E_FAIL;
    for (const auto& t : tries)
    {
        hr = st->client->Initialize ((AUDCLNT_SHAREMODE) t.shareMode, t.fl, 0, 0, t.fmt, nullptr);
        if (SUCCEEDED (hr))
        {
            // GetBuffer will deliver data in the format we requested - make
            // sure our conversion table matches it.
            if (t.fmt == &pcm16)
                st->dev.fmt = pcm16;
            break;
        }
        lastHr = hr;
    }
    if (FAILED (hr))
    {
        char buf[128];
        sprintf_s (buf, "IAudioClient::Initialize failed (last hr=0x%08lX tag=%u ch=%u rate=%lu bits=%u loopback=%d)",
                   (unsigned long) lastHr, st->dev.fmt.wFormatTag, st->dev.fmt.nChannels,
                   (unsigned long) st->dev.fmt.nSamplesPerSec, st->dev.fmt.wBitsPerSample,
                   st->dev.loopback ? 1 : 0);
        st->lastError = buf;
        cleanupCapture (st); return 5;
    }

    hr = st->client->GetService (IID_IAudioCaptureClient, (void**) &st->capture);
    if (FAILED (hr) || st->capture == nullptr)
    {
        // Some stacks expose loopback sessions through the custom
        // render-client-style interface instead of IAudioCaptureClient.
        st->capture = nullptr;
        hr = st->client->GetService (kIidLocalRenderClient, (void**) &st->localCapture);
        if (FAILED (hr) || st->localCapture == nullptr)
        { st->lastError = "no capture client available"; cleanupCapture (st); return 6; }
        st->useLocalCapture = true;
    }

    hr = st->client->Start ();
    if (FAILED (hr))
    {
        st->lastError = "IAudioClient::Start failed";
        cleanupCapture (st); return 7;
    }

    UINT32 bufFrames = 0;
    st->client->GetBufferSize (&bufFrames);
    const size_t capFrames = (size_t) std::max<UINT32> (1u, bufFrames * 4);   // ~4s of headroom
    st->ring.assign ((size_t) st->dev.fmt.nChannels * capFrames, 0.0f);
    return 0;
}

// Converts one raw packet to interleaved float32 and appends it to the ring
// buffer (dropping the oldest samples when full).
static void appendFrames(CaptureState* st, const void* data, size_t frames)
{
    if (frames == 0 || data == nullptr) return;
    const size_t chN = (size_t) st->dev.fmt.nChannels > 0 ? (size_t) st->dev.fmt.nChannels : 1;
    std::vector<float> tmp ((size_t) frames * chN);
    convertToFloat (data, st->dev.fmt, tmp.data (), frames);

    // head/count are in SAMPLE units; the ring holds capFrames*ch floats.
    const size_t ringSamples = st->ring.size ();
    std::lock_guard<std::mutex> lock (st->ringMutex);
    for (size_t i = 0; i < tmp.size (); ++i)
    {
        const size_t pos = (st->head + st->count) % ringSamples;
        if (pos == st->head && st->count == ringSamples)   // full: drop oldest
        {
            st->head = (st->head + 1) % ringSamples;
            --st->count;
        }
        st->ring[pos] = tmp[i];
        ++st->count;
    }
}

// Polls the capture client until stopped, converting each packet to
// interleaved float32 and appending it to the ring buffer. Runs on the
// capture worker thread (the same thread that activated the pipeline).
static void pollLoop(CaptureState* st)
{
    // Custom-interface loopback reader: request a small chunk; a successful
    // call hands out exactly that many frames of captured audio, and
    // ReleaseBuffer consumes them.
    static const UINT32 kLocalChunk = 192;   // ~4 ms at 48 kHz

    while (!st->stopFlag.load ())
    {
        if (st->useLocalCapture)
        {
            BYTE* data = nullptr;
            if (SUCCEEDED (st->localCapture->GetBuffer (kLocalChunk, &data)) && data != nullptr)
            {
                appendFrames (st, data, kLocalChunk);
                st->localCapture->ReleaseBuffer (kLocalChunk, 0);
            }
            else
                Sleep (1);
            continue;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        REFERENCE_TIME ts = 0;
        PROPVARIANT pkt {};
        const HRESULT hrGb = st->capture->GetBuffer (&data, &frames, &ts, &pkt);

        if (FAILED (hrGb))
        {
            // A transient buffer overflow is recoverable: the engine keeps
            // running and we simply lost that packet's data. Some engines
            // also return a "no data yet" error instead of blocking; treat
            // any failure other than a hard device fault as retryable after
            // a short pause.
            static const HRESULT kBufferOverflow = MAKE_HRESULT (1, 0x889, 1);
            if (hrGb == kBufferOverflow)
                Sleep (5);
            else
                break;   // AUDCLNT_E_DEVICE_INVALIDATED etc. - stop the session
            continue;
        }

        appendFrames (st, data, frames);

        // Some engines write garbage into the packet-info PROPVARIANT, so we
        // never call PropVariantClear on it - just reset for next iteration.
        pkt = {};
    }
}

// The dedicated capture worker: activates the whole pipeline, signals setup
// completion, polls until stopped, then releases everything - all on this one
// thread so COM state stays consistent.
DWORD WINAPI captureWorker(LPVOID param)
{
    CaptureState* st = static_cast<CaptureState*> (param);

    const int rc = setupCapture (st);
    st->setupResult = rc;
    if (st->setupEvent != nullptr)
        SetEvent (st->setupEvent);

    if (rc == 0)
        pollLoop (st);

    cleanupCapture (st);
    return 0;
}

// Stops a live session's worker thread. The worker releases its own COM
// objects before exiting, so there is nothing else to do here.
static void teardownSession(CaptureState* st)
{
    if (st->threadHandle != nullptr)
    {
        st->stopFlag.store (true);
        WaitForSingleObject (st->threadHandle, 5000);
        CloseHandle (st->threadHandle);
        st->threadHandle = nullptr;
    }
}

} // anonymous namespace

/* ============================ C ABI implementation ========================= */

// String/array storage for sc_rec_device_info pointers. The host copies what it
// needs immediately; everything is rebuilt on every get_device() call while
// holding g_mutex, so the pointers stay valid until the next one.
static std::vector<std::string> g_strPool;
static std::vector<double>      g_ratePool;
static std::vector<int>         g_depthPool;

extern "C" {

SC_REC_EXPORT int sc_rec_get_api_version() { return SC_RECORDER_API_VERSION; }

SC_REC_EXPORT int sc_rec_get_plugin_info(sc_rec_plugin_info* out)
{
    if (out == nullptr) return 1;
    std::memset (out, 0, sizeof (*out));
    out->id      = "wasapi";
    out->name    = "WASAPI (Windows Audio)";
    out->version = "1.0";
    return 0;
}

// Ensures g_devices is populated. Must NOT be called while holding g_mutex
// (enumerateDevices locks it internally).
static void ensureDevices()
{
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        if (!g_devices.empty ())
            return;
    }
    enumerateDevices ();
}

SC_REC_EXPORT int sc_rec_count_devices()
{
    ensureDevices ();
    std::lock_guard<std::mutex> lock (g_mutex);
    return (int) g_devices.size ();
}

SC_REC_EXPORT int sc_rec_get_device(int index, sc_rec_device_info* out)
{
    if (out == nullptr) return 1;
    std::lock_guard<std::mutex> lock (g_mutex);
    if ((size_t) index >= g_devices.size ()) return 2;

    const WasapiDevice& d = g_devices[(size_t) index];
    std::memset (out, 0, sizeof (*out));

    // Public id carries the mode prefix so the host can tell loopback apart.
    g_strPool.clear ();
    g_ratePool.clear ();
    g_depthPool.clear ();

    const std::string pubId = std::string (d.loopback ? "loopback:" : "cap:") + toUtf8 (d.id.c_str ());
    std::string dispName = d.name;
    if (d.loopback && dispName.find ("[Loopback]") == std::string::npos)
        dispName += " [Loopback]";

    g_strPool.push_back (pubId);
    g_strPool.push_back (dispName);
    out->id   = g_strPool[0].c_str ();
    out->name = g_strPool[1].c_str ();

    const int ch = d.fmt.nChannels;
    out->minChannels  = ch > 0 ? ch : 1;
    out->maxChannels  = ch > 0 ? ch : 1;
    out->defaultRate  = (double) d.fmt.nSamplesPerSec;

    if (ch > 0 && d.fmt.nSamplesPerSec > 0)
    {
        g_ratePool.push_back ((double) d.fmt.nSamplesPerSec);
        out->numRates = 1;
        out->rates    = g_ratePool.data ();
    }
    else
    {
        out->numRates = 0;
        out->minRate  = 8000.0;
        out->maxRate  = 192000.0;
    }

    g_depthPool.push_back (16);
    g_depthPool.push_back (24);
    g_depthPool.push_back (32);
    out->numDepths = 3;
    out->depths    = g_depthPool.data ();
    return 0;
}

SC_REC_EXPORT int sc_rec_configure(const char* deviceId, int wantChannels, double wantRate,
                                   int wantBitDepth, sc_rec_negotiated* actual)
{
    // A previous session's state is no longer needed once we reconfigure.
    // Stop its capture thread first so it can't touch freed memory.
    if (g_state != nullptr)
    {
        teardownSession (g_state);
        delete g_state;
        g_state = nullptr;
    }

    if (deviceId == nullptr) return 1;

    // Strip the "cap:"/"loopback:" prefix - GetDevice wants the raw MMDevice id.
    std::string pub(deviceId);
    const size_t colon = pub.find (':');
    std::wstring rawId = toWide ((colon != std::string::npos ? pub.substr (colon + 1) : pub).c_str ());

    WasapiDevice chosen;
    ensureDevices ();
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        for (const auto& d : g_devices)
            if (d.id == rawId) { chosen = d; break; }
    }
    if (chosen.id.empty ()) return 2;   // unknown device

    const int ch = chosen.fmt.nChannels;
    if (wantChannels < ch || wantChannels > ch)
        return 3;                       // unsupported channel count
    if (wantRate <= 0.0 || std::abs (wantRate - (double) chosen.fmt.nSamplesPerSec) > 1.0)
        return 3;                       // unsupported sample rate

    auto* st = new CaptureState ();
    st->dev = chosen;
    st->wantChannels = wantChannels;
    st->wantRate = wantRate;
    st->wantDepth = wantBitDepth;
    g_state = st;

    if (actual != nullptr)
    {
        actual->channels  = ch;
        actual->rate      = (double) chosen.fmt.nSamplesPerSec;
        actual->bitDepth  = wantBitDepth;
    }
    return 0;
}

SC_REC_EXPORT int sc_rec_start()
{
    if (g_state == nullptr) return 1;
    CaptureState* st = g_state;

    st->stopFlag.store (false);
    st->head = st->count = 0;
    st->ring.clear ();
    st->lastError.clear ();
    st->setupResult = -1;

    if (st->setupEvent == nullptr)
        st->setupEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);

    // All WASAPI work happens on the dedicated worker thread. We wait for its
    // setup-complete signal so failures are still reported synchronously.
    st->threadHandle = CreateThread (nullptr, 0, captureWorker, st, 0, nullptr);
    if (st->threadHandle == nullptr)
        return 4;               // thread creation failed

    if (st->setupEvent != nullptr &&
        WaitForSingleObject (st->setupEvent, 15000) != WAIT_OBJECT_0)
    {
        teardownSession (st);
        st->lastError = "capture setup timed out";
        return 8;
    }
    if (st->setupResult != 0)
    {
        teardownSession (st);
        return st->setupResult;
    }
    return 0;
}

SC_REC_EXPORT int sc_rec_stop()
{
    CaptureState* st = g_state;
    if (st == nullptr) return 1;

    teardownSession (st);
    return 0;
}

SC_REC_EXPORT int sc_rec_is_running()
{
    return g_state != nullptr && g_state->threadHandle != nullptr ? 1 : 0;
}

SC_REC_EXPORT long long sc_rec_available_frames()
{
    CaptureState* st = g_state;
    if (st == nullptr) return 0;
    std::lock_guard<std::mutex> lock (st->ringMutex);
    const size_t ch = (size_t) st->dev.fmt.nChannels > 0 ? (size_t) st->dev.fmt.nChannels : 1;
    return (long long) (st->count / ch);
}

SC_REC_EXPORT int sc_rec_read(float* dest, int channels, long long maxFrames)
{
    CaptureState* st = g_state;
    if (dest == nullptr || st == nullptr || maxFrames <= 0) return 0;

    std::lock_guard<std::mutex> lock (st->ringMutex);
    const size_t ch = (size_t) st->dev.fmt.nChannels > 0 ? (size_t) st->dev.fmt.nChannels : 1;
    long long frames = st->count / ch;
    if (frames > maxFrames) frames = maxFrames;

    for (long long f = 0; f < frames; ++f)
        for (size_t c = 0; c < ch && c < (size_t) channels; ++c)
            dest[(f * channels + (long long) c)] = st->ring[(st->head + (size_t) f * ch + c) % st->ring.size ()];

    return (int) frames;
}

SC_REC_EXPORT const char* sc_rec_last_error()
{
    static thread_local std::string msg = "no error";
    if (g_state != nullptr && !g_state->lastError.empty ())
        msg = g_state->lastError;
    else
        msg = "no error";
    return msg.c_str ();
}

} // extern "C"
