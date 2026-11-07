/**
 * SpectroCraft - Video frame decoding: platform-independent part
 * (frame stepping, background pump, extension lists, Linux stub).
 */

#include "VideoFrames.h"

#include <algorithm>
#include <chrono>

namespace sc {

//==============================================================================
std::unique_ptr<VideoDecoder> VideoDecoder::open (const juce::File& file, juce::String* error,
                                                  bool* noVideoTrack)
{
    if (noVideoTrack != nullptr) *noVideoTrack = false;
    auto s = openVideoFrameStream (file, error, noVideoTrack);
    if (s == nullptr)
        return nullptr;
    return std::make_unique<VideoDecoder> (std::move (s));
}

VideoDecoder::VideoDecoder (std::unique_ptr<VideoFrameStream> s) : stream (std::move (s))
{
    applied = { info().width, info().height };
}

juce::Point<int> videoDecodeSizeFor (int srcW, int srcH, int neededLongSide)
{
    if (srcW <= 0 || srcH <= 0)
        return { std::max (0, srcW), std::max (0, srcH) };
    const int longSide = std::max (srcW, srcH);
    if (neededLongSide <= 0 || neededLongSide >= longSide)
        return { srcW, srcH };

    constexpr int kMinLongSide = 320;
    int div = 1;
    while (div < 8 && longSide / (div * 2) >= std::max (neededLongSide, kMinLongSide))
        div *= 2;
    if (div == 1)
        return { srcW, srcH };
    auto even = [] (double v) { return std::max (2, (int) std::lround (v / 2.0) * 2); };
    return { even ((double) srcW / div), even ((double) srcH / div) };
}

void VideoDecoder::setNeededLongSide (int px)
{
    const auto want = videoDecodeSizeFor (info().width, info().height, px);
    if (want == applied)
        return;
    const bool native = want == juce::Point<int> (info().width, info().height);
    if (stream->setOutputSize (native ? 0 : want.x, native ? 0 : want.y))
    {
        applied = want;
        forceSeek = true;   // the frame on screen is re-decoded at the new size
    }
}

VideoFrame VideoDecoder::convert (VideoFrameStream::Native& n)
{
    VideoFrame f;
    f.image = n.toImage();
    f.time = n.time;
    f.duration = n.duration;
    f.index = decoded;
    return f;
}

void VideoDecoder::reposition (double t)
{
    // Start a little before t: some decoders only return frames whose time
    // is >= the seek point, which would miss the frame still on screen at t.
    const double fps = info().frameRate;
    const double back = fps > 0.0 ? 2.0 / fps : 0.1;
    stream->seek (std::max (0.0, t - back));
    pending.reset();
    current = {};
    atEnd = false;
    ++seeks;
}

bool VideoDecoder::frameAt (double t, VideoFrame& out)
{
    // Timestamps are rounded differently by every container/decoder: a
    // request exactly on a frame boundary must land on that frame.
    constexpr double eps = 1.0e-4;

    t = std::max (0.0, t);
    if (info().duration > 0.0)
        t = std::min (t, info().duration);

    if (forceSeek)
    {
        forceSeek = false;
        reposition (t);
    }
    else if (current.isValid() && t + eps >= validFrom)
    {
        if (pending != nullptr && t + eps < pending->time)
        {   out = current; return true; }   // still the frame on screen
        if (pending == nullptr && atEnd)
        {   out = current; return true; }   // past the last frame
        if (t - current.time > kForwardDecodeLimit)
            reposition (t);                  // long jump: cheaper to seek
    }
    else
        reposition (t);                      // backwards, or nothing yet

    // Step forward: keep the last frame at or before t, read one ahead.
    // Frames passed over are never converted to an image.
    std::unique_ptr<VideoFrameStream::Native> best;
    for (;;)
    {
        std::unique_ptr<VideoFrameStream::Native> f = pending != nullptr ? std::move (pending)
                                                                         : stream->next();
        if (f == nullptr) { atEnd = true; break; }
        ++decoded;
        if (f->time <= t + eps) { best = std::move (f); continue; }
        pending = std::move (f);
        break;
    }

    if (best != nullptr)
    {
        current = convert (*best);
        validFrom = current.time;
    }
    else if (!current.isValid())
    {
        // t is before the first frame (or the seek landed after it): show
        // the first frame there is.
        if (pending == nullptr)
            return false;
        current = convert (*pending);
        pending.reset();
        validFrom = std::min (t, current.time);
    }

    if (!current.image.isValid())
        return false;
    out = current;
    return true;
}

//==============================================================================
VideoFramePump::VideoFramePump()
{
    worker = std::thread ([this] { run(); });
}

VideoFramePump::~VideoFramePump()
{
    {
        const std::lock_guard<std::mutex> lk (mutex);
        quit = true;
    }
    cv.notify_all();
    worker.join();
    cancelPendingUpdate();
}

juce::File VideoFramePump::file() const
{
    const std::lock_guard<std::mutex> lk (mutex);
    return wantedFile;
}

void VideoFramePump::open (const juce::File& f)
{
    {
        const std::lock_guard<std::mutex> lk (mutex);
        wantedFile = f;
        ++fileGeneration;
        result = {};
    }
    opened = false;
    noTrack = false;
    openedInfo = {};
    openError = {};
    latest = {};
    cv.notify_all();
}

void VideoFramePump::setDisplayLongSide (int px)
{
    {
        const std::lock_guard<std::mutex> lk (mutex);
        if (px == wantedLongSide)
            return;
        wantedLongSide = px;
        ++timeGeneration;   // wake the worker: the frame is decoded again
    }
    cv.notify_all();
}

void VideoFramePump::request (double t)
{
    {
        const std::lock_guard<std::mutex> lk (mutex);
        if (t == wantedTime && timeGeneration != 0)
            return;
        wantedTime = t;
        ++timeGeneration;
    }
    cv.notify_all();
}

bool VideoFramePump::waitIdle (int timeoutMs)
{
    std::unique_lock<std::mutex> lk (mutex);
    return cv.wait_for (lk, std::chrono::milliseconds (timeoutMs), [this] {
        return !busy && workerFileGen == fileGeneration
               && (!workerHasDecoder || workerTimeGen == timeGeneration);
    });
}

void VideoFramePump::run()
{
    std::unique_ptr<VideoDecoder> dec;
    double lastPublished = -1.0;

    for (;;)
    {
        juce::File f;
        uint64_t fg = 0, tg = 0;
        double t = 0.0;
        bool reopen = false;
        {
            std::unique_lock<std::mutex> lk (mutex);
            cv.wait (lk, [&] {
                return quit || fileGeneration != workerFileGen
                       || (dec != nullptr && timeGeneration != workerTimeGen);
            });
            if (quit)
                break;
            reopen = fileGeneration != workerFileGen;
            f = wantedFile;
            fg = fileGeneration;
            busy = true;
        }

        if (reopen)
        {
            dec.reset();
            juce::String err;
            bool none = false;
            if (f != juce::File())
                dec = VideoDecoder::open (f, &err, &none);
            lastPublished = -1.0;

            const std::lock_guard<std::mutex> lk (mutex);
            workerFileGen = fg;
            workerHasDecoder = dec != nullptr;
            if (fileGeneration == fg)
            {
                result.generation = fg;
                result.openDone = true;
                result.ok = dec != nullptr;
                result.noTrack = none;
                result.info = dec != nullptr ? dec->info() : VideoInfo {};
                result.error = err;
                result.haveFrame = false;
            }
        }

        int longSide = 0;
        {
            const std::lock_guard<std::mutex> lk (mutex);
            t = wantedTime;
            tg = timeGeneration;
            longSide = wantedLongSide;
        }

        bool published = false;
        if (dec != nullptr)
        {
            const auto sizeBefore = dec->outputSize();
            dec->setNeededLongSide (longSide);
            const bool resized = dec->outputSize() != sizeBefore;
            VideoFrame fr;
            if (dec->frameAt (t, fr) && (reopen || resized || fr.time != lastPublished))
            {
                lastPublished = fr.time;
                const std::lock_guard<std::mutex> lk (mutex);
                if (fileGeneration == fg)
                {
                    result.generation = fg;
                    result.haveFrame = true;
                    result.frame = std::move (fr);
                    published = true;
                }
            }
        }

        {
            const std::lock_guard<std::mutex> lk (mutex);
            workerTimeGen = tg;
            busy = false;
        }
        cv.notify_all();
        if (reopen || published)
            triggerAsyncUpdate();
    }

    dec.reset();   // released on the thread that created it (COM apartment)
}

void VideoFramePump::handleAsyncUpdate()
{
    Result r;
    {
        const std::lock_guard<std::mutex> lk (mutex);
        if (result.generation != fileGeneration)
            return;   // stale: a newer open() is in flight
        r = result;
        result.openDone = false;
        result.haveFrame = false;
        result.frame = {};
    }

    if (r.openDone)
    {
        opened = r.ok;
        noTrack = r.noTrack;
        openedInfo = r.info;
        openError = r.error;
        if (onOpened) onOpened (r.ok);
    }
    if (r.haveFrame)
    {
        latest = std::move (r.frame);
        if (onFrame) onFrame();
    }
}

//==============================================================================
double videoDisplayTime (bool playing, double playPosition, bool followHover,
                         double hoverTime, double playhead)
{
    if (playing)
        return playPosition;
    if (followHover && hoverTime >= 0.0)
        return hoverTime;
    return playhead;
}

#if defined(__APPLE__) || defined(_WIN32)
bool videoDecodingAvailable() { return true; }
#else
bool videoDecodingAvailable() { return false; }
#endif

const juce::String& videoPictureExtensions()
{
#if defined(__APPLE__)
    // What AVFoundation opens natively.
    static const juce::String exts = ".mp4 .mov .m4v .3gp .3g2";
#elif defined(_WIN32)
    // Media Foundation's built-in sources (some need the codec extensions
    // from the Microsoft Store, e.g. HEVC/VP9; those fail with a message).
    static const juce::String exts = ".mp4 .mov .m4v .3gp .3g2 .mkv .webm .avi .wmv .asf .ts .m2ts .mts";
#else
    static const juce::String exts;
#endif
    return exts;
}

bool isVideoPictureCandidate (const juce::File& f)
{
    const juce::String ext = f.getFileExtension().toLowerCase();
    if (ext.isEmpty())
        return false;
    juce::StringArray list;
    list.addTokens (videoPictureExtensions(), " ", "");
    return list.contains (ext);
}

#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<VideoFrameStream> openVideoFrameStream (const juce::File&, juce::String* error,
                                                        bool* noVideoTrack)
{
    if (noVideoTrack != nullptr) *noVideoTrack = false;
    if (error != nullptr)
        *error = "Video pictures cannot be decoded on this platform.";
    return nullptr;
}
#endif

} // namespace sc
