/**
 * SpectroCraft - Video frame decoding, Windows (Media Foundation source reader).
 *
 * mfplat.dll / mfreadwrite.dll are delay-loaded (cmake/ScVideo.cmake): Windows
 * "N" editions and Server installs without the Media Foundation feature lack
 * them, and the app must still start there. They are probed with
 * LoadLibrary before the first call, so a missing runtime gives a message
 * in the Video window instead of a delay-load abort.
 */

#include "VideoFrames.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#undef min
#undef max

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace sc {
namespace {

using Microsoft::WRL::ComPtr;

// Windows 8+ GUIDs, defined here so the build does not depend on the
// _WIN32_WINNT level JUCE selects.
// MF_MT_VIDEO_ROTATION (mfapi.h).
const GUID kMtVideoRotation = { 0xc380465d, 0x2271, 0x428c, { 0x9b, 0x83, 0xec, 0xea, 0x3b, 0x4a, 0x85, 0xc1 } };
// MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING (mfreadwrite.h, Windows 8+).
const GUID kReaderAdvancedVideoProcessing = { 0x0f81da2c, 0xb537, 0x4672, { 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3 } };

bool mediaFoundationPresent()
{
    static const bool present = [] {
        const bool ok = ::LoadLibraryW (L"mfplat.dll") != nullptr
                        && ::LoadLibraryW (L"mfreadwrite.dll") != nullptr;
        return ok;
    }();
    return present;
}

juce::String hrText (HRESULT hr)
{
    return "0x" + juce::String::toHexString ((int) hr).paddedLeft ('0', 8);
}

struct Layout
{
    int frameW = 0, frameH = 0;     // decoded buffer size
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    LONG defaultStride = 0;         // bytes per row, negative = bottom-up
};

struct WinFrame : VideoFrameStream::Native
{
    ComPtr<IMFSample> sample;
    Layout layout;

    juce::Image toImage() override
    {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED (sample->ConvertToContiguousBuffer (&buf)))
            return {};

        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> b2;
        bool locked2d = false;
        if (SUCCEEDED (buf.As (&b2)) && SUCCEEDED (b2->Lock2D (&scan0, &pitch)))
            locked2d = true;
        else
        {
            BYTE* raw = nullptr;
            DWORD len = 0;
            if (FAILED (buf->Lock (&raw, nullptr, &len)))
                return {};
            pitch = layout.defaultStride != 0 ? layout.defaultStride : (LONG) layout.frameW * 4;
            const size_t need = (size_t) std::abs (pitch) * (size_t) layout.frameH;
            if (len < need)
            {   buf->Unlock(); return {}; }
            scan0 = pitch >= 0 ? raw : raw + (size_t) (-pitch) * (size_t) (layout.frameH - 1);
        }

        juce::Image img;
        const int w = layout.cropW, h = layout.cropH;
        if (scan0 != nullptr && w > 0 && h > 0
            && layout.cropX + w <= layout.frameW && layout.cropY + h <= layout.frameH)
        {
            img = juce::Image (juce::Image::ARGB, w, h, false);
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            // MFVideoFormat_RGB32 is B,G,R,X in memory: JUCE's ARGB layout
            // once the unused byte is made opaque (one pass).
            for (int y = 0; y < h; ++y)
            {
                const auto* src = (const uint32_t*) (scan0 + (ptrdiff_t) (y + layout.cropY) * pitch
                                                     + (ptrdiff_t) layout.cropX * 4);
                auto* dst = (uint32_t*) bd.getLinePointer (y);
                for (int x = 0; x < w; ++x)
                    dst[x] = src[x] | 0xff000000u;
            }
        }

        if (locked2d) b2->Unlock2D();
        else          buf->Unlock();
        return img;
    }
};

// Display aperture of a video type: coded sizes are padded to the macroblock
// size (1920x1088), the aperture is what is meant to be shown.
void apertureOf (IMFMediaType* t, int frameW, int frameH, int& x, int& y, int& w, int& h)
{
    x = 0; y = 0; w = frameW; h = frameH;
    MFVideoArea area {};
    if (SUCCEEDED (t->GetBlob (MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*) &area, sizeof (area), nullptr))
        || SUCCEEDED (t->GetBlob (MF_MT_GEOMETRIC_APERTURE, (UINT8*) &area, sizeof (area), nullptr)))
    {
        const int ax = area.OffsetX.value, ay = area.OffsetY.value;
        const int aw = (int) area.Area.cx, ah = (int) area.Area.cy;
        if (ax >= 0 && ay >= 0 && aw > 0 && ah > 0 && ax + aw <= frameW && ay + ah <= frameH)
        {   x = ax; y = ay; w = aw; h = ah; }
    }
}

class WinStream : public VideoFrameStream
{
public:
    ~WinStream() override
    {
        reader.Reset();
        if (mfStarted) MFShutdown();
        if (comInit) CoUninitialize();
    }

    bool open (const juce::File& f, juce::String* error, bool* noTrack)
    {
        auto fail = [error] (const juce::String& m) { if (error) *error = m; return false; };

        if (! mediaFoundationPresent())
            return fail ("Media Foundation is not installed (Windows N edition: install the "
                         "Media Feature Pack).");

        // The source reader is free-threaded; MTA unless the thread already
        // chose an apartment (then that one is used as is).
        const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        comInit = SUCCEEDED (co);

        HRESULT hr = MFStartup (MF_VERSION, MFSTARTUP_LITE);
        if (FAILED (hr))
            return fail ("Media Foundation could not start (" + hrText (hr) + ").");
        mfStarted = true;

        // Advanced video processing (Windows 8+) converts to RGB32 *and*
        // resizes, so 4K is decoded straight to the shown size. Older
        // systems / decoders it cannot handle: the basic converter, which
        // keeps the source size.
        for (const bool advanced : { true, false })
        {
            juce::String err;
            hr = createReader (f, advanced, noTrack, err);
            if (FAILED (hr))
            {
                if (noTrack != nullptr && *noTrack)
                    return fail (err);
                if (advanced) continue;
                return fail (err);
            }
            if (SUCCEEDED (hr = configureOutput (0, 0)))
            {
                canScale = advanced;
                break;
            }
            reader.Reset();
            if (! advanced)
                return fail ("No Windows decoder for this video codec (" + hrText (hr)
                             + "). HEVC, VP9 and AV1 need the codec extensions from the Microsoft Store.");
        }

        PROPVARIANT var;
        PropVariantInit (&var);
        if (SUCCEEDED (reader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE,
                                                         MF_PD_DURATION, &var))
            && var.vt == VT_UI8)
            inf.duration = (double) var.uhVal.QuadPart / 1.0e7;
        PropVariantClear (&var);
        return true;
    }

    const VideoInfo& info() const override { return inf; }

    bool setOutputSize (int width, int height) override
    {
        if (! canScale)
            return false;
        wantW = std::max (0, width);
        wantH = std::max (0, height);
        return true;
    }

    bool seek (double t) override
    {
        if (canScale && (wantW != outW || wantH != outH))
        {
            // New output size: reconfigure the processor (it takes effect
            // with the samples read after the seek).
            if (FAILED (configureOutput (wantW, wantH)))
            {
                canScale = false;
                configureOutput (0, 0);
            }
        }

        PROPVARIANT var;
        PropVariantInit (&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = (LONGLONG) std::llround (std::max (0.0, t) * 1.0e7);
        const HRESULT hr = reader->SetCurrentPosition (GUID_NULL, var);
        PropVariantClear (&var);
        return SUCCEEDED (hr);
    }

    std::unique_ptr<Native> next() override
    {
        // Gaps / stream ticks return no sample: skip a bounded number.
        for (int guard = 0; guard < 10000; ++guard)
        {
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            const HRESULT hr = reader->ReadSample ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                   &idx, &flags, &ts, &s);
            if (FAILED (hr) || (flags & MF_SOURCE_READERF_ERROR) != 0
                || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                return nullptr;
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 && ! updateLayout())
                return nullptr;
            if (s == nullptr)
                continue;

            auto f = std::make_unique<WinFrame>();
            LONGLONG dur = 0;
            if (SUCCEEDED (s->GetSampleDuration (&dur)))
                f->duration = (double) dur / 1.0e7;
            f->time = (double) ts / 1.0e7;
            f->sample = std::move (s);
            f->layout = layout;
            return f;
        }
        return nullptr;
    }

private:
    HRESULT createReader (const juce::File& f, bool advanced, bool* noTrack, juce::String& err)
    {
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes (&attrs, 2);
        if (FAILED (hr))
        {   err = "Media Foundation error " + hrText (hr) + "."; return hr; }
        // One or the other, never both (the reader refuses the pair).
        attrs->SetUINT32 (advanced ? kReaderAdvancedVideoProcessing : MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                          TRUE);

        hr = MFCreateSourceReaderFromURL (f.getFullPathName().toWideCharPointer(), attrs.Get(), &reader);
        if (FAILED (hr))
        {   err = "The file could not be opened by Media Foundation (" + hrText (hr) + ")."; return hr; }

        reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (FAILED (hr))
        {
            if (noTrack) *noTrack = true;
            err = "The file has no video track.";
            return hr;
        }

        ComPtr<IMFMediaType> native;
        hr = reader->GetNativeMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
        if (FAILED (hr))
        {   err = "The video format could not be read (" + hrText (hr) + ")."; return hr; }

        UINT32 w = 0, h = 0, num = 0, den = 0, rot = 0;
        if (SUCCEEDED (MFGetAttributeSize (native.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0)
        {
            int ax, ay, aw, ah;
            apertureOf (native.Get(), (int) w, (int) h, ax, ay, aw, ah);
            inf.width = aw;
            inf.height = ah;
        }
        if (SUCCEEDED (MFGetAttributeRatio (native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0)
            inf.frameRate = (double) num / (double) den;
        if (SUCCEEDED (native->GetUINT32 (kMtVideoRotation, &rot)))
            inf.rotation = (int) ((rot % 360 + 360) % 360) / 90 * 90;
        return S_OK;
    }

    /** RGB32 output, at w x h when both are > 0 (advanced processor only). */
    HRESULT configureOutput (int w, int h)
    {
        ComPtr<IMFMediaType> want;
        HRESULT hr = MFCreateMediaType (&want);
        if (FAILED (hr)) return hr;
        want->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (w > 0 && h > 0)
        {
            MFSetAttributeSize (want.Get(), MF_MT_FRAME_SIZE, (UINT32) w, (UINT32) h);
            MFSetAttributeRatio (want.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        }
        hr = reader->SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get());
        if (FAILED (hr)) return hr;
        if (! updateLayout()) return E_FAIL;
        outW = w > 0 && h > 0 ? w : 0;
        outH = w > 0 && h > 0 ? h : 0;
        return S_OK;
    }

    bool updateLayout()
    {
        ComPtr<IMFMediaType> cur;
        if (FAILED (reader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
            return false;
        UINT32 w = 0, h = 0;
        if (FAILED (MFGetAttributeSize (cur.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
            return false;

        Layout l;
        l.frameW = (int) w;
        l.frameH = (int) h;
        UINT32 stride = 0;
        l.defaultStride = SUCCEEDED (cur->GetUINT32 (MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG) stride
                                                                                      : (LONG) w * 4;
        apertureOf (cur.Get(), l.frameW, l.frameH, l.cropX, l.cropY, l.cropW, l.cropH);
        layout = l;
        return true;
    }

    ComPtr<IMFSourceReader> reader;
    Layout layout;
    VideoInfo inf;
    bool comInit = false, mfStarted = false, canScale = false;
    int wantW = 0, wantH = 0, outW = 0, outH = 0;   // 0 = source size
};

} // namespace

std::unique_ptr<VideoFrameStream> openVideoFrameStream (const juce::File& file, juce::String* error,
                                                        bool* noVideoTrack)
{
    if (noVideoTrack != nullptr) *noVideoTrack = false;
    if (! file.existsAsFile())
    {   if (error) *error = "File not found.";
        return nullptr; }
    auto s = std::make_unique<WinStream>();
    if (! s->open (file, error, noVideoTrack))
        return nullptr;
    return s;
}

} // namespace sc
