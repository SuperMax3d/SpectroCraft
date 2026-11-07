/**
 * SpectroCraft - Video frame decoding, macOS (AVFoundation AVAssetReader).
 * Compiled with ARC (cmake/ScVideo.cmake).
 */

// JUCE first: juce_opengl must see the GL headers before AVFoundation
// pulls in the system ones. No `using namespace juce` here: juce::Point /
// juce::Component would clash with the Carbon types the frameworks declare.
#define DONT_SET_USING_JUCE_NAMESPACE 1
#include "VideoFrames.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#if ! __has_feature(objc_arc)
 #error "VideoFrames_mac.mm must be compiled with -fobjc-arc"
#endif

// The synchronous AVAsset property getters (tracksWithMediaType:, duration,
// naturalSize...) are deprecated in favour of async loaders since macOS 13 /
// 15, but they still work and this code only ever runs on worker threads.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace sc {
namespace {

NSURL* fileUrl (const juce::File& f)
{
    return [NSURL fileURLWithPath: [NSString stringWithUTF8String: f.getFullPathName().toRawUTF8()]];
}

/** Clockwise rotation (0/90/180/270) encoded in a track's preferredTransform. */
int rotationFromTransform (CGAffineTransform t)
{
    const double deg = std::atan2 ((double) t.b, (double) t.a) * 180.0 / M_PI;
    int r = (int) std::lround (deg / 90.0) * 90;
    r = ((r % 360) + 360) % 360;
    return r;
}

struct MacFrame : VideoFrameStream::Native
{
    CMSampleBufferRef sample = nullptr;

    ~MacFrame() override
    {
        if (sample != nullptr)
            CFRelease (sample);
    }

    juce::Image toImage() override
    {
        CVImageBufferRef px = CMSampleBufferGetImageBuffer (sample);
        if (px == nullptr)
            return {};
        if (CVPixelBufferLockBaseAddress (px, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return {};

        const int w = (int) CVPixelBufferGetWidth (px);
        const int h = (int) CVPixelBufferGetHeight (px);
        const size_t stride = CVPixelBufferGetBytesPerRow (px);
        const auto* base = (const uint8_t*) CVPixelBufferGetBaseAddress (px);

        juce::Image img;
        if (base != nullptr && w > 0 && h > 0 && stride >= (size_t) w * 4)
        {
            img = juce::Image (juce::Image::ARGB, w, h, false);
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            // kCVPixelFormatType_32BGRA is JUCE's little-endian ARGB layout;
            // one pass copies and forces opacity (premultiplied ARGB).
            for (int y = 0; y < h; ++y)
            {
                const auto* src = (const uint32_t*) (base + (size_t) y * stride);
                auto* dst = (uint32_t*) bd.getLinePointer (y);
                for (int x = 0; x < w; ++x)
                    dst[x] = src[x] | 0xff000000u;
            }
        }
        CVPixelBufferUnlockBaseAddress (px, kCVPixelBufferLock_ReadOnly);
        return img;
    }
};

class MacStream : public VideoFrameStream
{
public:
    bool open (const juce::File& f, juce::String* error, bool* noTrack)
    {
        @autoreleasepool
        {
            asset = [AVURLAsset URLAssetWithURL: fileUrl (f)
                                        options: @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
            if (asset == nil)
            {   if (error) *error = "The file could not be opened.";
                return false; }

            NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType: AVMediaTypeVideo];
            if (tracks.count == 0)
            {
                const bool readable = [asset tracks].count > 0;
                if (noTrack) *noTrack = readable;
                if (error) *error = readable ? "The file has no video track."
                                             : "The file could not be opened as a movie.";
                return false;
            }
            track = tracks[0];

            const CGSize ns = track.naturalSize;
            inf.width = (int) std::lround (ns.width);
            inf.height = (int) std::lround (ns.height);
            inf.frameRate = (double) track.nominalFrameRate;
            inf.rotation = rotationFromTransform (track.preferredTransform);
            duration = asset.duration;
            inf.duration = CMTIME_IS_NUMERIC (duration) ? CMTimeGetSeconds (duration) : 0.0;

            // Decodability check: a reader that cannot start means no decoder
            // for this codec (or a protected file).
            if (! seek (0.0))
            {
                if (error)
                    *error = "The video track cannot be decoded"
                             + (lastError.isNotEmpty() ? " (" + lastError + ")." : juce::String ("."));
                return false;
            }
            return true;
        }
    }

    ~MacStream() override
    {
        @autoreleasepool
        {
            [reader cancelReading];
            reader = nil;
            output = nil;
        }
    }

    const VideoInfo& info() const override { return inf; }

    bool setOutputSize (int width, int height) override
    {
        outW = std::max (0, width);
        outH = std::max (0, height);
        return true;   // VideoToolbox scales while converting to BGRA
    }

    bool seek (double t) override
    {
        @autoreleasepool
        {
            [reader cancelReading];
            reader = nil;
            output = nil;

            NSError* err = nil;
            AVAssetReader* r = [[AVAssetReader alloc] initWithAsset: asset error: &err];
            if (r == nil)
            {   lastError = err != nil ? juce::String (err.localizedDescription.UTF8String) : juce::String();
                return false; }

            NSMutableDictionary* settings = [NSMutableDictionary dictionary];
            settings[(id) kCVPixelBufferPixelFormatTypeKey] = @(kCVPixelFormatType_32BGRA);
            if (outW > 0 && outH > 0)
            {
                // Scaled by the hardware decoder's pixel transfer: a 4K movie
                // shown in a window is decoded straight to the window size.
                settings[(id) kCVPixelBufferWidthKey] = @(outW);
                settings[(id) kCVPixelBufferHeightKey] = @(outH);
            }
            AVAssetReaderTrackOutput* o = [[AVAssetReaderTrackOutput alloc] initWithTrack: track
                                                                           outputSettings: settings];
            o.alwaysCopiesSampleData = NO;
            if (! [r canAddOutput: o])
                return false;
            [r addOutput: o];

            const CMTime start = CMTimeMakeWithSeconds (std::max (0.0, t), 90000);
            if (t > 0.0 && (! CMTIME_IS_NUMERIC (duration) || CMTimeCompare (start, duration) < 0))
                r.timeRange = CMTimeRangeMake (start, kCMTimePositiveInfinity);

            if (! [r startReading])
            {   lastError = r.error != nil ? juce::String (r.error.localizedDescription.UTF8String) : juce::String();
                return false; }

            reader = r;
            output = o;
            return true;
        }
    }

    std::unique_ptr<Native> next() override
    {
        @autoreleasepool
        {
            if (reader == nil || reader.status != AVAssetReaderStatusReading)
                return nullptr;
            for (;;)
            {
                CMSampleBufferRef sb = [output copyNextSampleBuffer];
                if (sb == nullptr)
                    return nullptr;
                if (CMSampleBufferGetImageBuffer (sb) == nullptr)
                {   CFRelease (sb); continue; }   // marker / empty buffer

                auto f = std::make_unique<MacFrame>();
                f->sample = sb;
                f->time = CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sb));
                const CMTime d = CMSampleBufferGetDuration (sb);
                f->duration = CMTIME_IS_NUMERIC (d) ? CMTimeGetSeconds (d) : 0.0;
                return f;
            }
        }
    }

private:
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    CMTime duration = kCMTimeInvalid;
    int outW = 0, outH = 0;   // 0 = source size
    VideoInfo inf;
    juce::String lastError;
};

} // namespace

std::unique_ptr<VideoFrameStream> openVideoFrameStream (const juce::File& file, juce::String* error,
                                                        bool* noVideoTrack)
{
    if (noVideoTrack != nullptr) *noVideoTrack = false;
    if (! file.existsAsFile())
    {   if (error) *error = "File not found.";
        return nullptr; }
    auto s = std::make_unique<MacStream>();
    if (! s->open (file, error, noVideoTrack))
        return nullptr;
    return s;
}

} // namespace sc

#pragma clang diagnostic pop
