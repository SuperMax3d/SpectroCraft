/**
 * SpectroCraft - Video frame decoding for the Video window
 *
 * Decodes the picture of a video file so it can be shown next to the
 * spectrogram of its audio track. The document's audio is played by the
 * normal AudioEngine; the picture is a silent slave of the document time:
 * the Video window asks for "the frame displayed at t" and gets it back
 * asynchronously.
 *
 * Platform decoders (no extra dependency):
 *   - macOS:   AVFoundation (AVAssetReader), src/VideoFrames_mac.mm
 *   - Windows: Media Foundation (IMFSourceReader), src/VideoFrames_win.cpp
 *   - Linux:   not available (videoDecodingAvailable() == false)
 */
#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sc {

//==============================================================================
/** One decoded picture and the time range it is displayed for. */
struct VideoFrame
{
    juce::Image image;        // ARGB, opaque; invalid when nothing was decoded
    double time = -1.0;       // presentation time (seconds)
    double duration = 0.0;    // display duration (seconds), 0 when unknown
    int64_t index = -1;       // decode-order counter since open (diagnostics)

    bool isValid() const noexcept { return image.isValid() && time >= 0.0; }
};

/** Static properties of an opened video track. */
struct VideoInfo
{
    int width = 0, height = 0;   // coded picture size after cropping (pixels)
    double duration = 0.0;       // seconds
    double frameRate = 0.0;      // nominal frames per second, 0 when unknown
    int rotation = 0;            // clockwise display rotation: 0, 90, 180 or 270
};

//==============================================================================
/** Platform frame stream: decodes frames in presentation order from a seek
    point. Implemented per platform; used only through VideoDecoder. Not
    thread-safe: one thread at a time (the Video window uses one worker). */
class VideoFrameStream
{
public:
    /** A decoded frame still in the platform's own buffer. toImage() copies
        it out; frames skipped while stepping are never converted. */
    struct Native
    {
        virtual ~Native() = default;
        virtual juce::Image toImage() = 0;
        double time = 0.0, duration = 0.0;
    };

    virtual ~VideoFrameStream() = default;
    virtual const VideoInfo& info() const = 0;
    /** Positions the stream so the next frames returned are at or before t
        (decoders restart from the preceding key frame). */
    virtual bool seek (double t) = 0;
    /** Next frame in presentation order, nullptr at end of stream / error. */
    virtual std::unique_ptr<Native> next() = 0;
    /** Size the decoded pictures should have (aspect kept, at most the
        source size; 0 x 0 = source size), applied from the next seek().
        The platform scaler does it (VideoToolbox on macOS, the Media
        Foundation video processor on Windows), so a 4K movie shown in a
        small window never moves 4K pictures around. False when this
        decoder cannot scale: frames keep the source size. */
    virtual bool setOutputSize (int width, int height) = 0;
};

/** Picture size for decoding a srcW x srcH video shown with a long side of
    neededLongSide physical pixels: the source size divided by 1, 2, 4 or 8
    (the smallest of those that is still >= neededLongSide, and never below
    a 320 px long side). Few distinct sizes, so resizing the window or
    zooming rarely re-decodes. neededLongSide <= 0 = source size. Even
    dimensions (what the decoders want). */
juce::Point<int> videoDecodeSizeFor (int srcW, int srcH, int neededLongSide);

/** Opens the platform stream for a file; nullptr (with *error) when there is
    no decodable video track or no decoder on this platform. *noVideoTrack is
    set when the file opened but simply carries no picture (an audio-only
    .mp4): not an error worth showing. */
std::unique_ptr<VideoFrameStream> openVideoFrameStream (const juce::File& file,
                                                        juce::String* error = nullptr,
                                                        bool* noVideoTrack = nullptr);

//==============================================================================
/** "Frame displayed at time t" on top of a VideoFrameStream.

    Increasing times close to the previous one decode forward (playback);
    a backward move or a jump of more than kForwardDecodeLimit seconds
    seeks first (scrubbing). Not thread-safe. */
class VideoDecoder
{
public:
    static constexpr double kForwardDecodeLimit = 1.0;   // seconds

    static std::unique_ptr<VideoDecoder> open (const juce::File& file, juce::String* error = nullptr,
                                               bool* noVideoTrack = nullptr);
    explicit VideoDecoder (std::unique_ptr<VideoFrameStream> s);

    const VideoInfo& info() const { return stream->info(); }

    /** Long side, in physical pixels, the picture is shown at (0 = source
        size): picks the decode size with videoDecodeSizeFor(). A change
        re-decodes the frame on screen at the new size on the next
        frameAt(). */
    void setNeededLongSide (int px);
    /** Size the decoder delivers now (source size when it cannot scale). */
    juce::Point<int> outputSize() const noexcept { return applied; }

    /** The frame displayed at t: the last one whose time is <= t (the first
        frame for t before it, the last frame past the end). Returns false
        only when nothing could be decoded. */
    bool frameAt (double t, VideoFrame& out);

    /** Number of seeks performed so far (tests, diagnostics). */
    int seekCount() const noexcept { return seeks; }

private:
    void reposition (double t);
    VideoFrame convert (VideoFrameStream::Native& n);

    std::unique_ptr<VideoFrameStream> stream;
    std::unique_ptr<VideoFrameStream::Native> pending;   // read ahead, time > last request
    VideoFrame current;
    double validFrom = 0.0;   // current answers every t in [validFrom, next frame)
    int64_t decoded = 0;
    int seeks = 0;
    bool atEnd = false;
    bool forceSeek = false;          // output size changed: decode again
    juce::Point<int> applied;        // current output size
};

//==============================================================================
/** Background decoder for the Video window: open() and request(t) return at
    once; a worker thread decodes the frame for the most recent request
    (older requests are dropped, so fast scrubbing never queues up) and
    calls onFrame / onOpened on the message thread. */
class VideoFramePump : private juce::AsyncUpdater
{
public:
    VideoFramePump();
    ~VideoFramePump() override;

    /** Opens a file (empty File = close). Any previous file is dropped. */
    void open (const juce::File& file);
    /** Asks for the frame displayed at t (seconds). */
    void request (double t);
    /** Long side (physical pixels) the picture is shown at; 0 = source
        size. Kept across open() calls. See VideoDecoder::setNeededLongSide. */
    void setDisplayLongSide (int px);

    juce::File file() const;
    /** Latest decoded frame (message thread). */
    const VideoFrame& frame() const noexcept { return latest; }
    /** Track info once opened, empty otherwise (message thread). */
    const VideoInfo& info() const noexcept { return openedInfo; }
    bool isOpen() const noexcept { return opened; }
    const juce::String& error() const noexcept { return openError; }
    /** The last open() found no picture at all (audio-only container). */
    bool hasNoVideoTrack() const noexcept { return noTrack; }

    /** Message thread: the file finished opening (ok or not). */
    std::function<void (bool ok)> onOpened;
    /** Message thread: a new frame is in frame(). */
    std::function<void()> onFrame;

    /** Blocks until the worker is idle (tests). False on timeout. */
    bool waitIdle (int timeoutMs);

private:
    void handleAsyncUpdate() override;
    void run();

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;

    // Shared with the worker (guarded by mutex).
    juce::File wantedFile;
    uint64_t fileGeneration = 0;
    double wantedTime = 0.0;
    uint64_t timeGeneration = 0;
    int wantedLongSide = 0;
    bool busy = false;
    uint64_t workerFileGen = 0;    // file generation the worker has opened
    uint64_t workerTimeGen = 0;    // time generation the worker has served
    bool workerHasDecoder = false;

    struct Result
    {
        uint64_t generation = 0;
        bool openDone = false, ok = false, noTrack = false;
        VideoInfo info;
        juce::String error;
        bool haveFrame = false;
        VideoFrame frame;
    } result;

    // Message-thread copies.
    bool opened = false, noTrack = false;
    VideoInfo openedInfo;
    juce::String openError;
    VideoFrame latest;
};

//==============================================================================
/** True when this build can decode video pictures (macOS, Windows). */
bool videoDecodingAvailable();

/** Space-separated extensions that may carry a video track (".mp4 .mov ...").
    Audio-only extensions (.m4a, .wav, ...) are not listed, so opening them
    never offers the Video window. Empty when videoDecodingAvailable() is
    false. */
const juce::String& videoPictureExtensions();

/** True when f has one of videoPictureExtensions() (no file access). */
bool isVideoPictureCandidate (const juce::File& f);

/** Which time the Video window shows:
      - while playing: the playback position;
      - otherwise, when hover-following is on and the mouse is over a plot
        (hoverTime >= 0): the time under the mouse;
      - otherwise: the playhead. */
double videoDisplayTime (bool playing, double playPosition, bool followHover,
                         double hoverTime, double playhead);

} // namespace sc
