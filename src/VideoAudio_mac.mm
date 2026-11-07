/**
 * SpectroCraft - Video container audio extraction, macOS (AVFoundation).
 *
 * The macOS counterpart of the FFmpeg reader in VideoAudio.cpp: decodes the
 * first audio track of a movie (.mp4, .mov, .m4v, ...) to 32-bit float PCM
 * through AVAssetReader and exposes it as a JUCE AudioFormatReader.
 * Sequential reads continue the running AVAssetReader; any other position
 * starts a new reader at that time (sample positions come from the buffer
 * timestamps). Compiled with ARC (cmake/ScVideo.cmake).
 */

// JUCE first: juce_opengl must see the GL headers before AVFoundation
// pulls in the system ones. No `using namespace juce` here: juce::Point /
// juce::Component would clash with the Carbon types the frameworks declare.
#define DONT_SET_USING_JUCE_NAMESPACE 1
#include "VideoAudio.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>


#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if ! __has_feature(objc_arc)
 #error "VideoAudio_mac.mm must be compiled with -fobjc-arc"
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // sync AVAsset getters (worker threads only)

namespace sc {
namespace {

class AvfAudioReader : public juce::AudioFormatReader
{
public:
    AvfAudioReader (const juce::File& file, juce::InputStream* streamToOwn)
        : AudioFormatReader (nullptr, "AVFoundation audio"), ownedStream (streamToOwn)
    {
        @autoreleasepool
        {
            NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];
            asset = [AVURLAsset URLAssetWithURL: url
                                        options: @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
            NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType: AVMediaTypeAudio];
            if (tracks.count == 0)
                return;   // numChannels stays 0 -> createReaderFor() reports failure
            track = tracks[0];

            NSArray* fds = track.formatDescriptions;
            if (fds.count == 0)
                return;
            const auto fd = (__bridge CMAudioFormatDescriptionRef) fds[0];
            const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription (fd);
            if (asbd == nullptr || asbd->mSampleRate <= 0.0 || asbd->mChannelsPerFrame == 0)
                return;

            const CMTimeRange tr = track.timeRange;
            const double secs = CMTIME_IS_NUMERIC (tr.duration) ? CMTimeGetSeconds (tr.duration) : 0.0;
            if (secs <= 0.0)
                return;

            usesFloatingPointData = true;
            bitsPerSample = 32;
            sampleRate = asbd->mSampleRate;
            lengthInSamples = (juce::int64) std::llround (secs * sampleRate);
            numChannels = asbd->mChannelsPerFrame;

            if (! restart (0))
                numChannels = 0;   // undecodable: report failure
        }
    }

    ~AvfAudioReader() override
    {
        @autoreleasepool
        {
            [reader cancelReading];
            reader = nil;
            output = nil;
        }
        delete ownedStream;
    }

    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        for (int c = 0; c < numDestChannels; ++c)
            if (destChannels[c] != nullptr)
                std::memset ((float*) destChannels[c] + startOffsetInDestBuffer, 0,
                             (size_t) numSamples * sizeof (float));
        if (numSamples <= 0 || numChannels == 0)
            return true;

        @autoreleasepool
        {
            // Continue the running reader when the request starts inside (or
            // right after) the buffered block; anything else restarts.
            const bool inside = carryStart >= 0 && startSampleInFile >= carryStart
                                && startSampleInFile <= carryEnd();
            if (! inside && ! restart (startSampleInFile))
                return true;   // zero-filled

            juce::int64 pos = startSampleInFile;
            const juce::int64 end = startSampleInFile + numSamples;
            const int nch = (int) numChannels;
            while (pos < end)
            {
                if (carryStart >= 0 && pos >= carryStart && pos < carryEnd())
                {
                    const juce::int64 take = std::min (end, carryEnd()) - pos;
                    const size_t srcOff = (size_t) (pos - carryStart);
                    for (int c = 0; c < numDestChannels; ++c)
                    {
                        if (destChannels[c] == nullptr || c >= nch)
                            continue;
                        float* d = (float*) destChannels[c] + startOffsetInDestBuffer + (pos - startSampleInFile);
                        for (juce::int64 i = 0; i < take; ++i)
                            d[i] = carry[(srcOff + (size_t) i) * (size_t) nch + (size_t) c];
                    }
                    pos += take;
                    continue;
                }
                if (carryStart >= 0 && pos < carryStart)
                    pos = std::min (end, carryStart);   // gap before the decoded data: zeros
                else if (! fetch())
                    break;                              // end of stream: zeros
            }
        }
        return true;
    }

    void disownStream() noexcept { ownedStream = nullptr; }

private:
    juce::int64 carryEnd() const
    {
        return carryStart + (juce::int64) (carry.size() / std::max (1u, numChannels));
    }

    bool restart (juce::int64 startSample)
    {
        [reader cancelReading];
        reader = nil;
        output = nil;
        carry.clear();
        carryStart = -1;

        NSError* err = nil;
        AVAssetReader* r = [[AVAssetReader alloc] initWithAsset: asset error: &err];
        if (r == nil)
            return false;
        NSDictionary* settings = @{ AVFormatIDKey: @(kAudioFormatLinearPCM),
                                    AVLinearPCMBitDepthKey: @32,
                                    AVLinearPCMIsFloatKey: @YES,
                                    AVLinearPCMIsNonInterleaved: @NO,
                                    AVLinearPCMIsBigEndianKey: @NO };
        AVAssetReaderTrackOutput* o = [[AVAssetReaderTrackOutput alloc] initWithTrack: track
                                                                       outputSettings: settings];
        o.alwaysCopiesSampleData = NO;
        if (! [r canAddOutput: o])
            return false;
        [r addOutput: o];
        if (startSample > 0)
            r.timeRange = CMTimeRangeMake (CMTimeMake (startSample, (int32_t) std::lround (sampleRate)),
                                           kCMTimePositiveInfinity);
        if (! [r startReading])
            return false;
        reader = r;
        output = o;
        // Until the first buffer arrives, the data "starts" at the request.
        carryStart = startSample;
        return true;
    }

    /** Replaces carry with the next decoded buffer. False at end of stream. */
    bool fetch()
    {
        if (reader == nil || reader.status != AVAssetReaderStatusReading)
            return false;
        for (;;)
        {
            CMSampleBufferRef sb = [output copyNextSampleBuffer];
            if (sb == nullptr)
                return false;
            const CMItemCount frames = CMSampleBufferGetNumSamples (sb);
            CMBlockBufferRef bb = CMSampleBufferGetDataBuffer (sb);
            const size_t want = (size_t) frames * numChannels;
            if (frames <= 0 || bb == nullptr || CMBlockBufferGetDataLength (bb) < want * sizeof (float))
            {   CFRelease (sb); continue; }

            const CMTime pts = CMSampleBufferGetPresentationTimeStamp (sb);
            const juce::int64 at = CMTIME_IS_NUMERIC (pts)
                                       ? (juce::int64) std::llround (CMTimeGetSeconds (pts) * sampleRate)
                                       : carryEnd();
            carry.resize (want);
            CMBlockBufferCopyDataBytes (bb, 0, want * sizeof (float), carry.data());
            carryStart = at;
            CFRelease (sb);
            return true;
        }
    }

    juce::InputStream* ownedStream = nullptr;
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    std::vector<float> carry;          // interleaved, frames [carryStart, carryEnd())
    juce::int64 carryStart = -1;

    JUCE_DECLARE_NON_COPYABLE (AvfAudioReader)
};

class AvfAudioFormat : public juce::AudioFormat
{
public:
    AvfAudioFormat()
        : AudioFormat (juce::StringRef ("AVFoundation audio"), juce::StringRef (videoContainerExtensions())) {}

    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths() override { return {}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                             const juce::AudioFormatWriterOptions&) override
    {   return nullptr; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool deleteStreamIfOpeningFails) override
    {
        auto* fin = dynamic_cast<juce::FileInputStream*> (sourceStream);
        if (fin == nullptr)
        {
            if (deleteStreamIfOpeningFails)
                delete sourceStream;
            return nullptr;
        }
        auto* r = new AvfAudioReader (fin->getFile(), sourceStream);
        if (r->numChannels == 0)
        {
            if (! deleteStreamIfOpeningFails)
                r->disownStream();   // the caller keeps its stream
            delete r;
            return nullptr;
        }
        return r;
    }
};

} // namespace

juce::AudioFormat* createAvfAudioFormat()
{
    return new AvfAudioFormat();
}

} // namespace sc

#pragma clang diagnostic pop
