/**
 * SpectroCraft - Audio Engine Implementation
 */

#include "AudioEngine.h"
#include "Diagnostics.h"
#include "FilterEQ.h"
#include "TimeStretch.h"
#include "VideoAudio.h"
#include "SpectralMaskProcessor.h"
#include "Resampler.h"
#include "UndoManager.h"
#include "MarkerManager.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <thread>

// Exclusive access to `buffer` for code that swaps, resizes or frees it.
// Protocol with the audio thread (getNextAudioBlock): the callback first
// increments audioInBufferSection, THEN checks bufferSwapPending; the scope
// first raises bufferSwapPending, THEN waits for audioInBufferSection to
// drain (both seq_cst), so a block either sees the pending swap and outputs
// silence, or is waited for. Other readers (readSamples on the FFT pool, the
// paint, workers) are excluded by bufferMutex. The audio thread never waits
// on bufferMutex while a writer holds or awaits it: by then it has already
// seen bufferSwapPending, or it has been drained. Never nest scopes, and
// never call readSamples() while holding one.
struct AudioEngine::BufferSwapScope
{
    AudioEngine& eng;
    explicit BufferSwapScope (AudioEngine& e) : eng (e)
    {
        eng.bufferSwapPending.fetch_add (1);
        while (eng.audioInBufferSection.load () > 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        eng.bufferMutex.lock ();
    }
    ~BufferSwapScope ()
    {
        eng.bufferMutex.unlock ();
        eng.bufferSwapPending.fetch_sub (1);
    }
    BufferSwapScope (const BufferSwapScope&) = delete;
    BufferSwapScope& operator= (const BufferSwapScope&) = delete;
};

void AudioEngine::swapInBuffer (std::unique_ptr<juce::AudioBuffer<float>> nb, int newChans,
                                int64_t newTotal, double newRate)
{
    std::vector<std::unique_ptr<float[]>> oldStore;
    {
        BufferSwapScope swap (*this);
        buffer.swap (nb);         // nb now holds the old buffer
        oldStore.swap (recStore); // recording storage the old buffer may refer to
        recCapacity = 0;
        nchans = newChans;
        totalSamples = newTotal;
        rate = newRate;
        durationSecs = (double) totalSamples / rate;
    }
    // The old buffer and storage are freed here, unreachable by any reader.
}

// juce::AudioBuffer is int-indexed: in-memory documents are limited to
// INT_MAX frames per channel.
static bool fitsAudioBuffer (int64_t frames)
{
    return frames >= 0 && frames <= (int64_t) std::numeric_limits<int>::max ();
}

AudioEngine::AudioEngine()
{
    fm = std::make_unique<juce::AudioFormatManager>();
    fm->registerBasicFormats();
    sc::registerVideoFormats (*fm);   // video containers (.mp4/.mkv/...) — no-op without FFmpeg
}

AudioEngine::~AudioEngine()
{
    closeFile();
}

bool AudioEngine::loadFile (const juce::File& file, std::function<void(float)> onProgress,
                            const std::atomic<bool>* cancel)
{
    closeFile();
    auto* rawReader = fm->createReaderFor (file);
    if (!rawReader)
        return false;

    currentFile = file;
    nchans = rawReader->numChannels;
    totalSamples = rawReader->lengthInSamples;
    rate = rawReader->sampleRate > 0 ? rawReader->sampleRate : 44100.0;
    durationSecs = (double) totalSamples / rate;
    hasFile = true;

    // Materialize the whole file only when it comfortably fits in memory
    // (~256 MB of float data by default); larger files are streamed.
    if (inMemoryLimit > 0 && (int64_t) nchans * totalSamples <= inMemoryLimit
        && fitsAudioBuffer (totalSamples))
    {
        buffer = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) totalSamples);
        // Read in chunks so a progress callback can report real I/O progress
        // on large files.
        const int64_t chunk = 1ll << 20;
        for (int64_t off = 0; off < totalSamples; off += chunk)
        {
            if (cancel != nullptr && cancel->load())
            {   // Leave the engine exactly as it was before loadFile().
                delete rawReader;
                closeFile();
                return false;
            }
            const int cnt = (int) std::min<int64_t> (chunk, totalSamples - off);
            // startSampleInDest MUST be `off` — writing every chunk to
            // position 0 would overwrite the buffer's beginning each pass.
            // A failed read must not leave uninitialised memory in the
            // document (it would be analysed and saved as real audio).
            if (!rawReader->read (buffer.get(), (int) off, cnt, (int) off, true, true))
                buffer->clear ((int) off, cnt);
            if (onProgress)
                onProgress ((float) ((double) (off + cnt) / (double) totalSamples));
        }
        delete rawReader;
    }
    else
    {
        reader.reset (rawReader); // keep streaming
        if (onProgress) onProgress (0.5f);
    }

    // Mipmap construction is the second half of the load for streamed files.
    bool mipsOk;
    if (onProgress)
    {
        auto p = std::move (onProgress);
        mipsOk = rebuildMipmaps ([p] (float f) { p (0.5f + 0.5f * f); }, cancel);
    }
    else
        mipsOk = rebuildMipmaps (nullptr, cancel);

    if (!mipsOk)
    {   // Cancelled mid-mipmap: discard the half-built document state.
        reader.reset (nullptr);
        closeFile();
        return false;
    }
    contentVer.fetch_add (1);   // new content (closeFile above may already have bumped)
    return true;
}

void AudioEngine::closeFile()
{
    playing = false; paused = false;
    {
        BufferSwapScope swap (*this);
        const std::lock_guard<std::mutex> lock (readerMutex);
        buffer.reset();
        reader.reset();
        recStore.clear();
        recCapacity = 0;
        hasFile = false;
    }
    clearMipmaps();
    setMutedChannels ({});   // a closed file leaves no muted channel behind
    currentFile = juce::File();
    nchans = 0; totalSamples = 0; rate = 44100.0; durationSecs = 0.0;
    hasFile = false; unsaved = false;
    contentVer.fetch_add (1);   // the content is gone
    playing = false; paused = false; looping = false;
    playheadFrac = 0.0;
    playbackRate = 0.0;
}

bool AudioEngine::readSamples (int channel, int64_t start, int64_t count, float* dest) const
{
    // Shared with other readers; excludes buffer/reader swaps (BufferSwapScope).
    const std::shared_lock<std::shared_mutex> bufLock (bufferMutex);
    if (!hasFile || channel < 0 || channel >= nchans || !dest)
        return false;

    // NOTE: std::clamp(value, lo, hi) — value first!
    start = std::clamp<int64_t> (start, (int64_t) 0, totalSamples);
    count = std::clamp<int64_t> (count, (int64_t) 0, totalSamples - start);

    // Nothing to read (e.g. the playhead sits at / past the end of the file):
    // a valid empty read. Return before getReadPointer, which asserts on
    // start == size even for an empty range.
    if (count == 0)
        return true;

    if (buffer)
    {
        const float* src = buffer->getReadPointer (channel, (int) start);
        std::copy (src, src + count, dest);
        return true;
    }

    const std::lock_guard<std::mutex> lock (readerMutex);
    if (reader == nullptr)
        return false;

    // Planar read straight into dest. Only `channel` gets a destination: the
    // reader skips null channel pointers. (The AudioBuffer overload with a
    // one-channel buffer always delivered the file's channel 0, whatever
    // `channel` was.) No heap allocation for the usual channel counts - this
    // runs on the audio thread for streamed files.
    float* stackPtrs[64] = {};
    std::vector<float*> heapPtrs;
    float** ptrs = stackPtrs;
    if (channel >= 64)
    {
        heapPtrs.assign ((size_t) channel + 1, nullptr);
        ptrs = heapPtrs.data ();
    }
    for (int64_t done = 0; done < count;)
    {
        const int n = (int) std::min<int64_t> (count - done, (int64_t) 1 << 30);
        ptrs[channel] = dest + done;
        if (!reader->read (ptrs, channel + 1, start + done, n))
            return false;
        done += n;
    }
    return true;
}

std::unique_ptr<juce::AudioFormatReader> AudioEngine::createReaderFor (const juce::File& file) const
{
    // The format manager is read-only after construction; createReaderFor just
    // probes each registered format, so this is safe off the message thread.
    std::unique_ptr<juce::AudioFormatReader> r (fm->createReaderFor (file));
    return r;
}

static void deinterleaveInto (float* dst, const float* interleaved, int ch, int nchans,
                              int64_t numFrames)
{
    const float* src = interleaved + (size_t) ch;
    for (int64_t i = 0; i < numFrames; ++i)
        dst[i] = src[(size_t) i * (size_t) nchans];
}

bool AudioEngine::appendSamples (const float* interleaved, int64_t numFrames)
{
    if (!hasFile || buffer == nullptr || reader != nullptr
            || interleaved == nullptr || numFrames <= 0 || nchans <= 0)
        return false;

    const int64_t old = totalSamples;
    const int64_t newTotal = old + numFrames;
    if (!fitsAudioBuffer (newTotal))
        return false;   // in-memory documents are int-indexed

    // `buffer` refers to recStore, per-channel storage with spare capacity
    // that grows geometrically: a tick only writes the new frames, instead of
    // reallocating and copying the whole document (O(N^2) over a recording).
    // Frames past totalSamples are never read, so they are written without
    // a lock; only publishing the new length takes the swap scope.
    // A failed allocation returns false: the data stays in the recorder's
    // ring and the caller retries on the next tick.
    std::vector<std::unique_ptr<float[]>> store;
    std::unique_ptr<juce::AudioBuffer<float>> nb;
    const bool grow = recCapacity < newTotal || (int) recStore.size () != nchans;
    try
    {
        std::vector<float*> ptrs ((size_t) nchans, nullptr);
        if (grow)
        {
            const int64_t cap = std::min<int64_t> (std::max<int64_t> (newTotal + newTotal / 2, 65536),
                                                   (int64_t) std::numeric_limits<int>::max ());
            store.reserve ((size_t) nchans);
            for (int ch = 0; ch < nchans; ++ch)
            {
                store.emplace_back (new float[(size_t) cap]);
                if (old > 0)
                    std::copy (buffer->getReadPointer (ch), buffer->getReadPointer (ch) + old,
                               store.back ().get ());
                ptrs[(size_t) ch] = store.back ().get ();
            }
            for (int ch = 0; ch < nchans; ++ch)
                deinterleaveInto (ptrs[(size_t) ch] + old, interleaved, ch, nchans, numFrames);
            nb = std::make_unique<juce::AudioBuffer<float>> (ptrs.data (), nchans, (int) newTotal);
            std::vector<std::unique_ptr<float[]>> oldStore;
            {
                BufferSwapScope swap (*this);
                buffer.swap (nb);           // nb now holds the old buffer
                oldStore.swap (recStore);
                recStore.swap (store);
                recCapacity = cap;
                totalSamples = newTotal;
                durationSecs = (double) totalSamples / rate;
            }
            // The old buffer and storage are freed here, unreachable by readers.
        }
        else
        {
            for (int ch = 0; ch < nchans; ++ch)
            {
                ptrs[(size_t) ch] = recStore[(size_t) ch].get ();
                deinterleaveInto (ptrs[(size_t) ch] + old, interleaved, ch, nchans, numFrames);
            }
            BufferSwapScope swap (*this);   // setDataToReferTo may reallocate the channel list
            buffer->setDataToReferTo (ptrs.data (), nchans, (int) newTotal);
            totalSamples = newTotal;
            durationSecs = (double) totalSamples / rate;
        }
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    unsaved = true;
    contentVer.fetch_add (1);

    // Keep the overview pyramid fresh while it is still cheap to rebuild; for
    // long recordings the caller does one final rebuild when recording stops.
    if (totalSamples <= 8000000 && (mipBaseTotal == 0 || totalSamples >= mipBaseTotal * 5 / 4))
    {
        rebuildMipmaps ();
        mipBaseTotal = totalSamples;
    }
    return true;
}

bool AudioEngine::overwriteSamples (const float* interleaved, int64_t startFrame,
                                    int64_t numFrames)
{
    if (!hasFile || buffer == nullptr || reader != nullptr
            || interleaved == nullptr || numFrames <= 0 || nchans <= 0 || startFrame < 0)
        return false;
    // The audio thread must not be inside `buffer` while we write to it.
    if (audioInBufferSection.load () > 0)
        return false;

    const int64_t old = totalSamples;
    const int64_t s = std::min (startFrame, old);   // clamp into the buffer
    const int64_t n = std::min (numFrames, old - s);
    if (n <= 0)
        return false;   // fully past the end: nothing to overwrite

    for (int ch = 0; ch < nchans; ++ch)
    {
        float* dst = buffer->getWritePointer (ch, (int) s);
        const float* src = interleaved + (size_t) ch;
        for (int64_t i = 0; i < n; ++i)
            dst[i] = src[(size_t) i * (size_t) nchans];
    }

    unsaved = true;
    contentVer.fetch_add (1);

    // The length never changes, so the growth-based append throttle can't
    // fire here: refresh on a time throttle instead (~4x per second).
    const int64_t nowMs = juce::Time::getMillisecondCounter ();
    if (totalSamples <= 8000000 && (overwriteMipMs == 0 || nowMs - overwriteMipMs >= 250))
    {
        rebuildMipmaps ();
        mipBaseTotal = totalSamples;
        overwriteMipMs = nowMs;
    }
    return true;
}

void AudioEngine::play()
{
    if (hasFile) { playing = true; paused = false; }
}

void AudioEngine::stop()
{
    playing = false; paused = false;
    playheadFrac = 0.0;
}

void AudioEngine::pause()
{
    if (playing) { playing = false; paused = true; }
}

void AudioEngine::setPosition (double secs)
{
    secs = std::clamp (secs, 0.0, durationSecs);
    playheadFrac = secs * rate;
}

void AudioEngine::setLooping (bool shouldLoop) { looping = shouldLoop; }

void AudioEngine::setLoopRange (juce::Range<double> r)
{
    r = r.getIntersectionWith ({ 0, durationSecs });
    if (r.getLength() > 0.001)
        loopRange_ = r;
}

void AudioEngine::setGain (float g) { gain_ = std::clamp<float> (g, 0.0f, 1.0f); }

void AudioEngine::setChannelMuted (int ch, bool muted)
{
    if (ch < 0 || isChannelMuted (ch) == muted) return;
    auto m = muted_;
    if ((int) m.size() <= ch) m.resize ((size_t) ch + 1, false);
    m[(size_t) ch] = muted;
    setMutedChannels (std::move (m));
}

void AudioEngine::setMutedChannels (std::vector<bool> muted)
{
    while (!muted.empty() && !muted.back()) muted.pop_back();   // trailing "not muted" is implied
    muted_ = std::move (muted);

    std::shared_ptr<const std::vector<uint8_t>> forAudio;
    if (!muted_.empty())
        forAudio = std::make_shared<const std::vector<uint8_t>> (muted_.begin(), muted_.end());
    const std::lock_guard<std::mutex> lk (muteMutex);
    mutedForAudio_ = std::move (forAudio);
}

std::shared_ptr<const std::vector<uint8_t>> AudioEngine::mutesForAudio() const
{
    const std::lock_guard<std::mutex> lk (muteMutex);
    return mutedForAudio_;
}

int AudioEngine::playbackOutputFor (int channel, int numOutputs) noexcept
{
    return numOutputs > 0 && channel >= 0 ? channel % numOutputs : -1;
}

float AudioEngine::playbackFoldGain (int output, int numChannels, int numOutputs) noexcept
{
    if (numOutputs <= 0 || output < 0 || output >= numOutputs)
        return 0.0f;
    const int k = (numChannels - output + numOutputs - 1) / numOutputs;   // channels on this output
    return k > 1 ? 1.0f / (float) k : 1.0f;
}

void AudioEngine::prepareToPlay (int, double sampleRate)
{
    // The device tells us its actual output rate. When it differs from the
    // file's rate we resample in getNextAudioBlock() with linear interpolation.
    playbackRate = sampleRate;
}

void AudioEngine::releaseResources() {}

void AudioEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* dst = bufferToFill.buffer;
    const int n = bufferToFill.numSamples;

    // Track that we are inside a section reading `buffer`: quiesceForEdit()
    // and BufferSwapScope wait for this to drain. It MUST be entered before
    // the checks below - testing `playing` first and registering afterwards
    // let an edit swap the buffer between the two (use-after-free).
    struct BufferSectionGuard
    {
        std::atomic<int>& counter;
        explicit BufferSectionGuard (std::atomic<int>& c) : counter (c) { ++counter; }
        ~BufferSectionGuard() { --counter; }
    } bufferSectionGuard (audioInBufferSection);

    if (!playing || !hasFile || bufferSwapPending.load () > 0)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // Advance the fractional playhead by (deviceRate / fileRate) per output
    // sample; step == 1.0 when the rates match.
    const double step = (playbackRate > 0.0 && std::abs (playbackRate - rate) > 1e-3)
                         ? playbackRate / rate : 1.0;
    // Seeks (setPosition/stop) that land while this block renders win over
    // the block's own advance: see the compare-exchange at the end.
    double posAtStart = playheadFrac.load();
    double pos = posAtStart;

    if (looping && loopRange_.getLength() > 0)
    {
        const double ls = loopRange_.getStart() * rate;
        const double le = loopRange_.getEnd() * rate;
        const double len = le - ls;
        if (len > 0.0 && pos >= le)
            pos = ls + std::fmod (pos - ls, len);
    }

    // Advance the playhead for this block up front: both fill paths below only
    // read it, and a seek that lands while the block renders must win either way.
    double newPos = pos + (double) n * step;
    if (newPos >= (double) totalSamples)
    {
        if (looping && loopRange_.getLength() > 0)
            newPos = loopRange_.getStart() * rate;
        else
        {
            newPos = (double) totalSamples;
            playing.store (false); // auto-stop at end of file
        }
    }
    playheadFrac.compare_exchange_strong (posAtStart, newPos);

    // "Solo": a video track's own audio replaces the document content while
    // transport runs; the document clock still drives the position.
    std::shared_ptr<const SoloAudio> soloLocal;
    {
        std::lock_guard<std::mutex> lk (soloMutex);
        soloLocal = solo_;
    }
    if ((bool) soloLocal && soloLocal->rate > 0.0
        && !soloLocal->data.empty() && !soloLocal->data[0].empty())
    {
        const double sRate = soloLocal->rate;
        const int64_t soloLen = (int64_t) soloLocal->data[0].size();
        // Document-sample position -> solo-sample position.
        const double a = sRate / rate;              // solo samples per document sample
        const double offS = soloLocal->offsetSecs * sRate;
        const double pFirst = pos * a - offS;
        const double pLast  = (pos + n * step) * a - offS;
        const int64_t rs = std::max<int64_t> (0, (int64_t) std::floor (pFirst));
        const int64_t re = std::min (soloLen, (int64_t) std::ceil (pLast) + 1);
        const int64_t cnt = std::max<int64_t> (0, re - rs);

        // Every solo channel is heard, folded when the device has fewer
        // outputs (playbackOutputFor).
        const int outs = dst->getNumChannels();
        const int soloChans = (int) soloLocal->data.size();
        const int sChans = outs > 0 ? soloChans : 0;
        auto& tmp = playTmp;
        tmp.resize ((size_t) sChans);
        for (int ch = 0; ch < sChans; ++ch)
        {
            const auto& src = soloLocal->data[(size_t) ch];
            tmp[(size_t) ch].resize ((size_t) cnt);
            const int64_t avail = std::max<int64_t> (0, soloLen - rs);
            for (int64_t i = 0; i < avail && i < cnt; ++i)
                tmp[(size_t) ch][(size_t) i] = src[(size_t) (rs + i)];   // short tail: stale data
            if (cnt > avail)
                std::fill (tmp[(size_t) ch].begin() + (size_t) avail, tmp[(size_t) ch].end(), 0.0f);
        }

        // Silence first (outputs nothing reaches, the solo's start or end),
        // then each solo channel adds itself onto its output.
        for (int o = 0; o < outs; ++o)
            dst->clear (o, bufferToFill.startSample, n);
        const auto muted = mutesForAudio();
        // A mono document's single channel stands for every output here too:
        // its mute silences the whole solo.
        auto isMuted = [&] (int ch)
        {
            const int m = nchans == 1 ? 0 : ch;
            return muted != nullptr && m < (int) muted->size() && (*muted)[(size_t) m] != 0;
        };
        const float g = gain_.load();
        for (int ch = 0; ch < sChans; ++ch)
        {
            if (isMuted (ch))
                continue;
            const int o = playbackOutputFor (ch, outs);
            const float gc = g * playbackFoldGain (o, soloChans, outs);
            float* d = dst->getWritePointer (o, bufferToFill.startSample);
            const float* src = tmp[(size_t) ch].data();
            for (int i = 0; i < n; ++i)
            {
                const double p = (pos + (double) i * step) * a - offS - (double) rs;
                if (p >= 0.0 && p < (double) cnt - 1.0)
                {
                    const int64_t ia = (int64_t) std::floor (p);
                    const float fr = (float) (p - (double) ia);
                    d[i] += (src[ia] * (1.0f - fr) + src[ia + 1] * fr) * gc;
                }
            }
        }
        return;
    }

    // File-sample range needed for this block.
    const int64_t baseReadStart = std::max<int64_t> (0, (int64_t) std::floor (pos));
    const int64_t baseReadEnd = std::min (totalSamples,
                                          (int64_t) std::ceil (pos + n * step) + 1);
    const int64_t count       = std::max<int64_t> (0, baseReadEnd - baseReadStart);

    // "Play Spectral Selection": the STFT mask needs extra context samples so
    // its last analysis window is complete; read them alongside this block.
    std::shared_ptr<const SpectralSelection> selLocal;
    {
        std::lock_guard<std::mutex> lk (maskMutex);
        selLocal = playSel;
    }
    const bool masking = (bool) selLocal && count > 0;

    // When the mask will re-prime for this block (seek, loop wrap, toggle,
    // first block), also read fftSize()-1 samples before baseReadStart so
    // every analysis window is complete and the fast-forward is exact.
    int64_t ctxBefore = 0;
    if (masking)
    {
        std::lock_guard<std::mutex> lk (maskMutex);
        if (!playMask || playMask->willReprimeAt (baseReadStart))
            ctxBefore = SpectralMask::fftSize() - 1;
    }
    const int64_t readStart = std::max<int64_t> (0, baseReadStart - ctxBefore);

    const int64_t readEnd = std::min (totalSamples, baseReadEnd
                                      + (masking ? SpectralMask::contextSamples() : 0));
    const int64_t extCount = std::max<int64_t> (0, readEnd - readStart);

    // Every channel is read, even past the device's outputs: the extra ones
    // fold onto the outputs there are (playbackOutputFor), so none is lost.
    const int outs = dst->getNumChannels();
    const int chans = outs > 0 ? nchans : 0;
    // Scratch reused across blocks (no per-block heap allocation once grown).
    auto& tmp = playTmp;
    tmp.resize ((size_t) chans);
    for (int ch = 0; ch < chans; ++ch)
    {
        tmp[(size_t) ch].resize ((size_t) extCount);
        if (!readSamples (ch, readStart, extCount, tmp[(size_t) ch].data()))
            tmp[(size_t) ch].assign ((size_t) extCount, 0.0f);
        // Zero anything beyond the file end: reused vectors may hold stale
        // data in the mask's context region (and readSamples only fills what
        // is actually available).
        const int64_t avail = std::max<int64_t> (0, totalSamples - readStart);
        if ((int64_t) tmp[(size_t) ch].size() > avail)
            std::fill (tmp[(size_t) ch].begin() + (size_t) avail,
                       tmp[(size_t) ch].end(), 0.0f);
    }

    if (masking)
    {
        std::lock_guard<std::mutex> lk (maskMutex);
        if (!playMask || !playMask->matches (rate, nchans))
        {
            if (!playMask)
                playMask = std::make_shared<SpectralMask::PlaybackMask>();
            playMask->prepare (rate, nchans);
        }
        // Masks tmp[ch][ctxBefore .. ctxBefore+count) in place; the extra
        // context samples around it are only used by the mask's analysis
        // windows.
        playMask->process (tmp, readStart, baseReadStart, (int) count, *selLocal);
    }

    // Silence first (past the end of the file, outputs beyond a
    // multichannel file's channels, never stale data), then each channel
    // that is not muted (channel numbers in the tracks) adds itself onto its
    // output.
    for (int o = 0; o < outs; ++o)
        dst->clear (o, bufferToFill.startSample, n);
    const auto muted = mutesForAudio();
    const float g = gain_.load();
    for (int ch = 0; ch < chans; ++ch)
    {
        if (muted != nullptr && ch < (int) muted->size() && (*muted)[(size_t) ch] != 0)
            continue;
        const int o = playbackOutputFor (ch, outs);
        const float gc = g * playbackFoldGain (o, nchans, outs);
        float* d = dst->getWritePointer (o, bufferToFill.startSample);
        const float* src = tmp[(size_t) ch].data();
        for (int i = 0; i < n; ++i)
        {
            // Linear interpolation between the two surrounding file samples.
            // (p is relative to readStart, which includes the mask's
            // pre-context; tmp indices are absolute within that buffer.)
            const double p = pos + (double) i * step - (double) readStart;
            if (p >= 0.0 && p < (double) ctxBefore + count - 1.0)
            {
                const int64_t a = (int64_t) std::floor (p);
                const float fr = (float) (p - (double) a);
                d[i] += (src[a] * (1.0f - fr) + src[a + 1] * fr) * gc;
            }
        }
    }

    // A mono file plays on every output (muted: output 0 is silent, so all are).
    if (nchans == 1)
        for (int o = 1; o < outs; ++o)
            dst->copyFrom (o, bufferToFill.startSample, *dst, 0, bufferToFill.startSample, n);
}

// Builds the finest mipmap level in ONE sequential pass over the file.
// Source priority:
//   1. Memory-mapped reader (WAV/AIFF): readMaxLevels() scans mapped pages
//      directly - no user-buffer copy, one call per output block.
//   2. In-memory buffer: direct pointer scan (no copies).
//   3. Streaming reader: large sequential chunks with ALL channels at once.
//
// The legacy code re-read the whole file once PER LEVEL and PER CHANNEL, with
// a seek + fresh heap allocation per 64k-sample block - that was the load-time
// bottleneck for multi-GB files (a 4 GB stereo WAV cost ~20 full passes over
// the disk plus hundreds of thousands of seeks).
//
// Values are bit-identical to the legacy per-level re-scan: each block's min/
// max is clamped against zero exactly as before, and coarser levels (built by
// pairwise reduction in rebuildMipmaps) equal a direct re-scan because min/max
// decimation composes: the min over a union of blocks is the min of the block
// mins.
bool AudioEngine::buildFinestLevel (AudioEngine& eng, int64_t factor,
                                    MipLevel& lv,
                                    std::function<void(float)> onProgress,
                                    const std::atomic<bool>* cancel)
{
    const int nchans = eng.numChannels();
    const int64_t N  = eng.numSamples();
    const int64_t n  = N / factor + 1;

    lv.factor = factor;
    lv.min.resize (nchans);
    lv.max.resize (nchans);
    for (int ch = 0; ch < nchans; ++ch)
    {
        lv.min[ch].resize ((size_t) n);
        lv.max[ch].resize ((size_t) n);
    }

    // ---- Source 1: memory-mapped reader (WAV/AIFF only) ------------------
    // Only used when there is no in-memory buffer. When a buffer exists it is
    // the authoritative source of truth - after an edit it holds unsaved data
    // that differs from what is still on disk, so mapping the file would
    // build stale mipmaps (the waveform would show pre-edit content while the
    // spectral view, which reads the buffer, shows the edit).
    if (eng.buffer == nullptr)
    {
        std::unique_ptr<juce::MemoryMappedAudioFormatReader> mmr;
        for (int i = 0; i < eng.fm->getNumKnownFormats(); ++i)
        {
            auto* fmt = eng.fm->getKnownFormat (i);
            if (fmt != nullptr && fmt->canHandleFile (eng.currentFile))
            {
                mmr.reset (fmt->createMemoryMappedReader (eng.currentFile));
                if (mmr != nullptr && mmr->mapEntireFile())
                    break;
                mmr.reset();
            }
        }

        if (mmr != nullptr)
        {
            std::vector<juce::Range<float>> res ((size_t) nchans);
            const int64_t reportEvery = std::max<int64_t> (1, n / 64);
            for (int64_t i = 0; i < n; ++i)
            {
                if (cancel != nullptr && cancel->load() && i % reportEvery == 0)
                    return false;
                const int64_t s   = i * factor;
                const int64_t cnt = std::min<int64_t> (factor, N - s);
                mmr->readMaxLevels (s, cnt, res.data(), nchans);
                for (int ch = 0; ch < nchans; ++ch)
                {
                    float mn = res[ch].getStart();
                    float mx = res[ch].getEnd();
                    if (mn > 0.0f) mn = 0.0f;
                    if (mx < 0.0f) mx = 0.0f;
                    lv.min[ch][(size_t) i] = mn;
                    lv.max[ch][(size_t) i] = mx;
                }
                if (onProgress && (i % reportEvery == 0 || i + 1 == n))
                    onProgress ((float) (i + 1) / (float) n);
            }
            return true;
        }
    }

    // ---- Source 2: in-memory buffer --------------------------------------
    if (eng.buffer != nullptr)
    {
        const int64_t reportEvery = std::max<int64_t> (1, n / 64);
        for (int ch = 0; ch < nchans; ++ch)
        {
            const float* base = eng.buffer->getReadPointer (ch, 0);
            for (int64_t i = 0; i < n; ++i)
            {
                if (cancel != nullptr && cancel->load() && i % reportEvery == 0)
                    return false;
                const int64_t s   = i * factor;
                const int64_t cnt = std::min<int64_t> (factor, N - s);
                float mn = 0.0f, mx = 0.0f;
                if (cnt > 0)
                {
                    auto r = juce::FloatVectorOperations::findMinAndMax (base + s, (int) cnt);
                    mn = r.getStart();
                    mx = r.getEnd();
                }
                if (mn > 0.0f) mn = 0.0f;
                if (mx < 0.0f) mx = 0.0f;
                lv.min[ch][(size_t) i] = mn;
                lv.max[ch][(size_t) i] = mx;
            }
            if (onProgress && ch + 1 == nchans)
                onProgress (1.0f);
        }
        return true;
    }

    // ---- Source 3: streaming reader, one sequential pass ------------------
    if (eng.reader != nullptr)
    {
        const int64_t chunk = std::min<int64_t> (1ll << 20, N); // frames per read
        juce::AudioBuffer<float> buf (nchans, (int) chunk);
        std::vector<float> mn ((size_t) nchans, 0.0f), mx ((size_t) nchans, 0.0f);
        int64_t bi = 0;                 // current finest block index
        int64_t nextBoundary = factor;  // sample index ending the current block

        auto flushBlock = [&] {
            for (int ch = 0; ch < nchans; ++ch)
            {
                float a = mn[ch], b = mx[ch];
                if (a > 0.0f) a = 0.0f;
                if (b < 0.0f) b = 0.0f;
                lv.min[ch][(size_t) bi] = a;
                lv.max[ch][(size_t) bi] = b;
            }
            std::fill (mn.begin(), mn.end(), 0.0f);
            std::fill (mx.begin(), mx.end(), 0.0f);
            ++bi;
        };

        for (int64_t off = 0; off < N; off += chunk)
        {
            if (cancel != nullptr && cancel->load())
                return false;
            const int cnt = (int) std::min<int64_t> (chunk, N - off);
            bool ok = false;
            {
                const std::lock_guard<std::mutex> lock (eng.readerMutex);
                ok = eng.reader != nullptr
                     && eng.reader->read (buf.getArrayOfWritePointers(), nchans,
                                          off, cnt);
            }
            if (!ok)
                break;

            for (int q = 0; q < cnt; ++q)
            {
                if ((int64_t) (off + q) >= nextBoundary)
                {
                    flushBlock();
                    nextBoundary += factor;
                }
                for (int ch = 0; ch < nchans; ++ch)
                {
                    const float v = *buf.getReadPointer (ch, q);
                    if (v < mn[ch]) mn[ch] = v;
                    if (v > mx[ch]) mx[ch] = v;
                }
            }
            if (onProgress)
                onProgress ((float) std::min<int64_t> (off + cnt, N) / (float) N);
        }
        flushBlock(); // last partial block
        return true;
    }

    return false;
}

bool AudioEngine::rebuildMipmaps(std::function<void(float)> onProgress,
                                const std::atomic<bool>* cancel)
{
    clearMipmaps();
    if (!hasFile || totalSamples == 0)
        return true;

    // A cancelled rebuild leaves the pyramid empty (clearMipmaps() above).
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    // Base factor chosen so the finest level has ~100k blocks per channel.
    baseFactor = 1;
    while (totalSamples / baseFactor > 100000)
        baseFactor *= 2;

    int64_t factor = baseFactor;
    // Count levels first so progress can be reported as a fraction.
    int totalMipLevels = 0;
    {
        int64_t f2 = baseFactor;
        while (f2 <= 16384ll * baseFactor && totalSamples / f2 > 100)
        { ++totalMipLevels; f2 *= 2; }
    }

    // Phase 1: finest level in a single pass over the file.
    MipLevel lv;
    auto p1 = [onProgress] (float f) { if (onProgress) onProgress (0.9f * f); };
    if (!buildFinestLevel (*this, baseFactor, lv, p1, cancel))
        return false;
    mips.push_back (std::move (lv));

    // Phase 2: coarser levels by pairwise reduction of the level below.
    // Level F=2P entry i covers samples [i*2P, (i+1)*2P) = union of P-level
    // entries 2i and 2i+1, so min/max compose exactly (bit-identical to a
    // direct re-scan). This replaces ~9 full extra passes over the file with
    // pure in-memory work.
    factor = baseFactor * 2;
    int coarseDone = 0;
    const int totalCoarse = std::max (1, totalMipLevels - 1);
    while (factor <= 16384ll * baseFactor && totalSamples / factor > 100)
    {
        if (cancelled())
            break;
        MipLevel up;
        up.factor = factor;
        const int64_t n2 = totalSamples / factor + 1;
        up.min.resize (nchans);
        up.max.resize (nchans);

        const auto& prevMin = mips.back().min;
        const auto& prevMax = mips.back().max;
        const int64_t prevN = totalSamples / mips.back().factor + 1;

        for (int ch = 0; ch < nchans; ++ch)
        {
            up.min[ch].resize ((size_t) n2);
            up.max[ch].resize ((size_t) n2);
        }

        for (int64_t i = 0; i < n2; ++i)
        {
            const int64_t j0 = 2 * i;
            if (j0 >= prevN)
            {
                // Empty tail block: legacy behaviour is exactly 0/0.
                for (int ch = 0; ch < nchans; ++ch)
                {
                    up.min[ch][(size_t) i] = 0.0f;
                    up.max[ch][(size_t) i] = 0.0f;
                }
            }
            else
            {
                const int64_t j1 = std::min<int64_t> (j0 + 2, prevN);
                for (int ch = 0; ch < nchans; ++ch)
                {
                    float a = prevMin[ch][(size_t) j0];
                    float b = prevMax[ch][(size_t) j0];
                    if (j1 > j0 + 1)
                    {
                        a = std::min (a, prevMin[ch][(size_t) (j0 + 1)]);
                        b = std::max (b, prevMax[ch][(size_t) (j0 + 1)]);
                    }
                    up.min[ch][(size_t) i] = a;
                    up.max[ch][(size_t) i] = b;
                }
            }
        }

        mips.push_back (std::move (up));
        if (onProgress)
            onProgress (0.9f + 0.1f * (++coarseDone / (float) totalCoarse));
        factor *= 2;
    }

    if (cancelled())
    {   // Leave the pyramid empty, as documented.
        clearMipmaps();
        return false;
    }
    mipBaseTotal = totalSamples;
    return true;
}

void AudioEngine::clearMipmaps() { mips.clear(); baseFactor = 1; mipBaseTotal = 0; overwriteMipMs = 0; }

// ---- Editing ----

// NOTE: newData is taken by const reference and copied into the patch — callers
// may still need it afterwards. oldData is read from the buffer at call time,
// so this must be called BEFORE the region is modified in place.
// newChans (when != 0) records a channel-count change alongside the edit;
// newData then has one row per NEW channel and applyPatch handles both
// directions of the count change.
void recordReplace (AudioEngine& eng, const juce::Range<int64_t>& region,
                           const std::vector<std::vector<float>>& newData,
                           const juce::String& description,
                           int newChans = 0, double newRate = 0.0)
{
    Patch p;
    p.oldStart = region.getStart();
    p.oldEnd = region.getEnd();
    p.description = description;
    if (newChans > 0 && newChans != eng.numChannels())
    {
        p.oldChans = eng.numChannels();
        p.newChans = newChans;
    }
    if (newRate > 0.0 && std::abs (newRate - eng.sampleRate ()) > 1e-9 * std::max (newRate, eng.sampleRate ()))
    {
        p.oldRate = eng.sampleRate ();
        p.newRate = newRate;
    }
    p.oldData.resize (eng.numChannels());
    for (int ch = 0; ch < eng.numChannels(); ++ch)
    {
        const int64_t len = region.getLength();
        p.oldData[ch].resize ((size_t) len);
        eng.readSamples (ch, region.getStart(), len, p.oldData[ch].data());
    }
    p.newData = newData;
    p.lengthDelta = (int64_t) (p.newData.empty() ? 0 : (int64_t) p.newData[0].size()) - region.getLength();
    if (eng.undo) eng.undo->record (std::move (p));
}

bool AudioEngine::initNew (int numChans, double sampleRate,
                           const std::vector<std::vector<float>>* data)
{
    if (numChans <= 0 || !std::isfinite (sampleRate) || sampleRate <= 0.0)
        return false;

    int64_t len = 0;
    if (data != nullptr && !data->empty())
    {
        len = (int64_t) (*data)[0].size();
        for (const auto& v : *data)
            if ((int64_t) v.size() != len)
                return false;   // inconsistent channel lengths
    }

    if (!fitsAudioBuffer (len))
        return false;

    std::unique_ptr<juce::AudioBuffer<float>> nb;
    try
    {
        nb = std::make_unique<juce::AudioBuffer<float>> (numChans, (int) len);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    if (data != nullptr && len > 0)
        for (int ch = 0; ch < numChans; ++ch)
            std::copy ((*data)[(size_t) ch].begin(), (*data)[(size_t) ch].end(),
                       nb->getWritePointer (ch));

    {
        // Drop any previous source (initNew may reuse an engine).
        const std::lock_guard<std::mutex> lock (readerMutex);
        reader.reset();
    }
    swapInBuffer (std::move (nb), numChans, len, sampleRate);
    hasFile = true;
    unsaved = len > 0;   // content that exists nowhere else is "unsaved"
    contentVer.fetch_add (1);   // new content
    currentFile = juce::File();   // no path until Save As

    playheadFrac = 0.0;
    rebuildMipmaps();   // no-op for empty documents
    return true;
}

bool AudioEngine::readRange (int64_t start, int64_t count,
                             std::vector<std::vector<float>>& out) const
{
    if (!hasFile || count <= 0)
        return false;
    auto r = juce::Range<int64_t> { start, start + count }
                 .getIntersectionWith ({ 0, totalSamples });
    if (r.getLength() <= 0)
        return false;

    out.resize ((size_t) nchans);
    for (int ch = 0; ch < nchans; ++ch)
    {
        out[(size_t) ch].resize ((size_t) r.getLength());
        if (!readSamples (ch, r.getStart(), r.getLength(), out[(size_t) ch].data()))
            return false;
    }
    return true;
}

static bool failErr (std::string* errOut, const juce::String& msg)
{
    if (errOut != nullptr) *errOut = msg.toStdString ();
    return false;
}

// Edits catch std::bad_alloc and report it instead of terminating the app.
// They build the new buffer BEFORE recording the undo patch, so a failed
// allocation leaves both the document and the history untouched.
static bool failNoMemory (std::string* errOut)
{
    return failErr (errOut, "Not enough memory for this edit.");
}

static bool failTooLong (std::string* errOut)
{
    return failErr (errOut, "The result would exceed the longest editable length "
                            "(2^31 - 1 samples per channel).");
}

bool AudioEngine::pasteAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                           std::string* errOut)
{
    if (!ensureEditable (errOut))
        return false;
    if (clip.getNumSamples() <= 0)
        return failErr (errOut, "Nothing in the clipboard to paste.");
    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);
    const int64_t pasteLen = clip.getNumSamples();
    const int clipChans = clip.getNumChannels();

    const int64_t newTotal = totalSamples + pasteLen;
    if (!fitsAudioBuffer (newTotal))
        return failTooLong (errOut);

    try
    {
        // Target channels beyond the clipboard's own count receive silence.
        std::vector<std::vector<float>> newData (nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            newData[ch].resize ((size_t) pasteLen, 0.0f);
            if (ch < clipChans)
                std::copy (clip.getReadPointer (ch),
                           clip.getReadPointer (ch) + pasteLen,
                           newData[ch].data());
        }

        // Build the new buffer first (recordReplace only records).
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
            nb->copyFrom (ch, (int) samplePos, newData[ch].data(), (int) pasteLen);
            nb->copyFrom (ch, (int) (samplePos + pasteLen), *buffer, ch, (int) samplePos,
                          (int) (totalSamples - samplePos));
        }

        // Insert = replace empty region [pos, pos) with newData
        recordReplace (*this, { samplePos, samplePos }, newData, "Pasted samples");
        swapInBuffer (std::move (nb), nchans, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::insertSamples (int64_t samplePos, const std::vector<std::vector<float>>& data,
                                 const juce::String& description, std::string* errOut)
{
    if (!ensureEditable (errOut))
        return false;
    if (data.empty () || data[0].empty ())
        return failErr (errOut, "Nothing to insert.");
    const int64_t pasteLen = (int64_t) data[0].size ();
    for (const auto& c : data)
        if ((int64_t) c.size () != pasteLen || (int) data.size () != nchans)
            return failErr (errOut, "Inconsistent generated data.");

    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);

    const int64_t newTotal = totalSamples + pasteLen;
    if (!fitsAudioBuffer (newTotal))
        return failTooLong (errOut);
    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
            nb->copyFrom (ch, (int) samplePos, data[(size_t) ch].data (), (int) pasteLen);
            nb->copyFrom (ch, (int) (samplePos + pasteLen), *buffer, ch, (int) samplePos,
                          (int) (totalSamples - samplePos));
        }

        // Insert = replace empty region [pos, pos) with the new data.
        recordReplace (*this, { samplePos, samplePos }, data, description);
        swapInBuffer (std::move (nb), nchans, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::pasteMixAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                              std::string* errOut)
{
    if (!ensureEditable (errOut))
        return false;
    if (clip.getNumSamples() <= 0)
        return failErr (errOut, "Nothing in the clipboard to mix.");
    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);
    const int64_t pasteLen = clip.getNumSamples();
    const int clipChans = clip.getNumChannels();

    // Mix over the overlap [pos, pos+overlap); if the clipboard extends past
    // EOF the remainder is appended (a plain paste at the end).
    const int64_t tail = std::max<int64_t> (0, pasteLen - (totalSamples - samplePos));
    const int64_t newTotal = totalSamples + tail;
    if (!fitsAudioBuffer (newTotal))
        return failTooLong (errOut);

    try
    {
        std::vector<std::vector<float>> newData (nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            const int64_t oldLen = totalSamples - samplePos;
            newData[ch].resize ((size_t) (oldLen + tail), 0.0f);
            const float* old = buffer->getReadPointer (ch, (int) samplePos);
            for (int64_t i = 0; i < oldLen; ++i)
            {
                float v = old[i];
                if (ch < clipChans && i < pasteLen)
                    v += clip.getReadPointer (ch)[(size_t) i];
                newData[ch][(size_t) i] = v;
            }

            // The part of the clipboard that extends past EOF is appended as-is.
            for (int64_t i = oldLen; i < oldLen + tail; ++i)
                newData[ch][(size_t) i] = (ch < clipChans)
                                             ? clip.getReadPointer (ch)[(size_t) i]
                                             : 0.0f;
        }

        // Build the new buffer first (recordReplace only records).
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
            nb->copyFrom (ch, (int) samplePos, newData[ch].data(),
                          (int) (totalSamples - samplePos + tail));
        }

        // One undoable patch: region [pos, EOF) replaced by mixed + appended data.
        recordReplace (*this, { samplePos, totalSamples }, newData,
                      "Mixed in pasted samples");
        swapInBuffer (std::move (nb), nchans, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

// Pastes the clipboard into brand-new channels appended after the existing
// ones: the file keeps its length, every existing channel stays byte-for-byte
// identical, and each added channel is silence except for the pasted clip at
// samplePos (truncated if it would run past the end of the file).
bool AudioEngine::pasteToNewChannelsAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                                        std::string* errOut)
{
    if (!ensureEditable (errOut))
        return false;
    if (clip.getNumSamples() <= 0)
        return failErr (errOut, "Nothing in the clipboard to paste.");
    const int addChans = clip.getNumChannels();
    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);
    if (samplePos >= totalSamples)
        return failErr (errOut, "No room in the file for the paste.");
    const int64_t usable = std::min<int64_t> (clip.getNumSamples(), totalSamples - samplePos);

    try
    {
        // Whole-file replacement patch that also carries the channel-count change:
        // existing channels are copied through unchanged; the added channels are
        // silence except for the pasted clipboard data.
        std::vector<std::vector<float>> newData ((size_t) (nchans + addChans),
                                                 std::vector<float> ((size_t) totalSamples, 0.0f));
        for (int ch = 0; ch < nchans; ++ch)
            std::copy (buffer->getReadPointer (ch), buffer->getReadPointer (ch) + totalSamples,
                       newData[(size_t) ch].data());
        for (int ch = 0; ch < addChans; ++ch)
            std::copy (clip.getReadPointer (ch), clip.getReadPointer (ch) + usable,
                       newData[(size_t) (nchans + ch)].data() + samplePos);

        // Build the new buffer first: a whole-file replacement is just a swap.
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans + addChans, (int) totalSamples);
        for (int ch = 0; ch < nchans + addChans; ++ch)
            std::copy (newData[(size_t) ch].data(), newData[(size_t) ch].data() + totalSamples,
                       nb->getWritePointer (ch));

        recordReplace (*this, { 0, totalSamples }, newData, "Pasted to new channel(s)",
                      nchans + addChans);
        swapInBuffer (std::move (nb), nchans + addChans, totalSamples, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::writeDataToNewChannels (int64_t samplePos,
                                          const std::vector<std::vector<float>>& data,
                                          const juce::String& description,
                                          std::string* errOut)
{
    if (!ensureEditable (errOut))
        return false;
    const int addChans = (int) data.size();
    if (addChans <= 0 || data[0].empty())
        return failErr (errOut, "Nothing to write.");
    for (const auto& ch : data)
        if ((int64_t) ch.size() != (int64_t) data[0].size())
            return failErr (errOut, "The channels have different lengths.");

    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);
    const int64_t len = (int64_t) data[0].size();
    // Extend the document with silence when the data runs past its end.
    const int64_t newLen = std::max<int64_t> (totalSamples, samplePos + len);

    try
    {
        std::vector<std::vector<float>> newData ((size_t) (nchans + addChans),
                                                 std::vector<float> ((size_t) newLen, 0.0f));
        for (int ch = 0; ch < nchans; ++ch)
            std::copy (buffer->getReadPointer (ch), buffer->getReadPointer (ch) + totalSamples,
                       newData[(size_t) ch].data());   // tail stays silence
        for (int ch = 0; ch < addChans; ++ch)
            std::copy (data[(size_t) ch].begin(), data[(size_t) ch].end(),
                       newData[(size_t) (nchans + ch)].data() + samplePos);

        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans + addChans, (int) newLen);
        for (int ch = 0; ch < nchans + addChans; ++ch)
            std::copy (newData[(size_t) ch].data(), newData[(size_t) ch].data() + newLen,
                       nb->getWritePointer (ch));

        recordReplace (*this, { 0, totalSamples }, newData, description, nchans + addChans);
        swapInBuffer (std::move (nb), nchans + addChans, newLen, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::deleteSamples (juce::Range<int64_t> samples, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;   // nothing to delete

    const int64_t delLen = samples.getLength();
    const int64_t newTotal = totalSamples - delLen;
    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samples.getStart());
            nb->copyFrom (ch, (int) samples.getStart(), *buffer, ch, (int) samples.getEnd(),
                          (int) (totalSamples - samples.getEnd()));
        }
        recordReplace (*this, samples, {}, "Deleted samples");
        swapInBuffer (std::move (nb), nchans, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::trimToRange (juce::Range<int64_t> keep, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    keep = keep.getIntersectionWith ({ 0, totalSamples });
    const int64_t keepLen = keep.getLength();
    if (keepLen <= 0 || keepLen >= totalSamples) return true;   // nothing to trim

    try
    {
        // One undoable patch: the whole old buffer is replaced by the kept range,
        // so a single Undo restores everything that was trimmed away.
        std::vector<std::vector<float>> newData (nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            newData[ch].resize ((size_t) keepLen);
            readSamples (ch, keep.getStart(), keepLen, newData[ch].data());
        }

        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) keepLen);
        for (int ch = 0; ch < nchans; ++ch)
            nb->copyFrom (ch, 0, *buffer, ch, (int) keep.getStart(), (int) keepLen);

        recordReplace (*this, { 0, totalSamples }, newData, "Trimmed to selection");
        swapInBuffer (std::move (nb), nchans, keepLen, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

// Returns true if the channel is affected by `chans` (nullptr => all).
static bool channelAffected (const std::vector<int>* chans, int ch)
{
    return !chans || std::find (chans->begin(), chans->end(), ch) != chans->end();
}

bool AudioEngine::silenceSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;   // nothing to silence

    // Record the replacement data explicitly — an empty newData vector would
    // mean "delete" and undo would remove the samples instead of restoring them.
    // Only the selected channels are zeroed; all others keep their current data.
    const int64_t len = samples.getLength();
    try
    {
        std::vector<std::vector<float>> newData (nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            newData[ch].resize ((size_t) len, 0.0f);
            if (!channelAffected (chans, ch))
                readSamples (ch, samples.getStart(), len, newData[ch].data());
        }
        recordReplace (*this, samples, std::move (newData), "Silenced selection");
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }

    for (int ch = 0; ch < nchans; ++ch)
        if (channelAffected (chans, ch))
            buffer->clear (ch, (int) samples.getStart(), (int) len);
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

// Applies fn to the selected channels only; other channels are copied
// unchanged so the recorded undo patch stays complete for all channels.
void applyPerSample (AudioEngine& eng, juce::Range<int64_t> samples,
                            std::function<float (float, int, int)> fn, const std::vector<int>* chans,
                            const juce::String& description)
{
    if (!eng.buffer) return;
    samples = samples.getIntersectionWith ({ 0, eng.totalSamples });
    if (samples.getLength() <= 0) return;

    // Build the replacement data first (read-only access to the buffer).
    std::vector<std::vector<float>> newData (eng.numChannels());
    for (int ch = 0; ch < eng.numChannels(); ++ch)
    {
        const float* src = eng.buffer->getReadPointer (ch, (int) samples.getStart());
        newData[ch].resize ((size_t) samples.getLength());
        if (channelAffected (chans, ch))
            for (int64_t i = 0; i < samples.getLength(); ++i)
                newData[ch][i] = fn (src[i], (int) i, (int) samples.getLength());
        else
            std::copy (src, src + samples.getLength(), newData[ch].data()); // unchanged copy
    }

    // Record the patch BEFORE touching the buffer so oldData is pristine.
    recordReplace (eng, samples, newData, description);

    for (int ch = 0; ch < eng.numChannels(); ++ch)
        if (channelAffected (chans, ch))
            std::copy (newData[ch].data(), newData[ch].data() + samples.getLength(),
                      eng.buffer->getWritePointer (ch, (int) samples.getStart()));

    eng.unsaved = true;
    eng.contentVer.fetch_add (1);
    eng.rebuildMipmaps();
}

bool AudioEngine::ensureEditable (std::string* errOut,
                                  std::function<void(float)> onProgress,
                                  const std::atomic<bool>* cancel)
{
    auto fail = [errOut] (const juce::String& m) -> bool
    {
        if (errOut != nullptr) *errOut = m.toStdString ();
        return false;
    };

    if (!hasFile)
        return fail ("No audio loaded.");
    if (buffer != nullptr)
        return true;   // already editable in place
    if (reader == nullptr)
        return fail ("This file cannot be edited in place.");

    if (!fitsAudioBuffer (totalSamples))
        return fail ("This file is too long to be edited in memory "
                     "(more than 2^31 - 1 samples per channel).");

    // Stop playback and wait for any in-flight audio block to finish reading
    // before we swap the source out from under it (same pattern as resampleTo).
    quiesceForEdit ();

    const int64_t needBytes = (int64_t) nchans * totalSamples * sizeof (float);
    std::unique_ptr<juce::AudioBuffer<float>> nb;
    try
    {
        nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) totalSamples);
    }
    catch (const std::bad_alloc&)
    {
        return fail ("Not enough memory to edit this file (~"
                     + juce::String ((double) needBytes / 1073741824.0, 1) + " GB needed).");
    }

    // Read the whole file from the streaming reader in chunks.
    const int64_t chunk = 1ll << 20;
    for (int64_t off = 0; off < totalSamples; off += chunk)
    {
        if (cancel != nullptr && cancel->load())
            return fail ("Cancelled");
        const int cnt = (int) std::min<int64_t> (chunk, totalSamples - off);
        // startSampleInDest MUST be `off` (see loadFile for the same reason).
        const bool ok = [&] { 
            const std::lock_guard<std::mutex> lock (readerMutex);
            return reader != nullptr
                   && reader->read (nb.get(), (int) off, cnt, off, true, true);
        } ();
        if (!ok)
            return fail ("Failed to read the file for editing.");
        if (onProgress != nullptr)
            onProgress ((float) ((double) (off + cnt) / (double) totalSamples));
    }

    // The buffer is now the authoritative source of truth; drop the reader.
    // Both happen under the swap scope and readerMutex: readers on other
    // threads (FFT pool, paint) may be in readSamples() right now.
    std::unique_ptr<juce::AudioFormatReader> oldReader;
    {
        BufferSwapScope swap (*this);
        const std::lock_guard<std::mutex> lock (readerMutex);
        buffer = std::move (nb);
        oldReader = std::move (reader);
        recStore.clear ();
        recCapacity = 0;
    }
    return true;
}

bool AudioEngine::replaceSamples (const juce::Range<int64_t>& samplesIn,
                                  const std::vector<std::vector<float>>& newData,
                                  const juce::String& description,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    auto samples = samplesIn.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;   // nothing to replace

    // Validate BEFORE recording: a patch whose rows do not match the region
    // would corrupt the buffer on every later undo/redo.
    const int64_t len = samples.getLength();
    if ((int) newData.size () != nchans)
        return failErr (errOut, "Replacement data must have one vector per channel.");
    for (const auto& v : newData)
        if ((int64_t) v.size () != len)
            return failErr (errOut, "Replacement data does not match the range length.");

    // Record the patch BEFORE touching the buffer so oldData is pristine.
    try
    {
        recordReplace (*this, samples, newData, description);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }

    for (int ch = 0; ch < nchans; ++ch)
        std::copy (newData[ch].begin (), newData[ch].end (),
                   buffer->getWritePointer (ch, (int) samples.getStart ()));

    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    return true;
}

bool AudioEngine::replaceRangeWithDifferentLength (const juce::Range<int64_t>& regionIn,
                                                    const std::vector<std::vector<float>>& newData,
                                                    const juce::String& description,
                                                    std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    auto region = regionIn.getIntersectionWith ({ 0, totalSamples });
    if (region.getLength() <= 0) return true;   // nothing to replace

    if ((int) newData.size () != nchans)
        return failErr (errOut, "Replacement data must have one vector per channel.");

    const int64_t newLen = (int64_t) newData[0].size ();
    for (const auto& v : newData)
        if ((int64_t) v.size () != (size_t) std::max<int64_t> (0, newLen))
            return failErr (errOut, "Inconsistent channel lengths in the replacement data.");

    const int64_t start = region.getStart ();
    const int64_t end   = region.getEnd ();
    const int64_t newTotal = totalSamples - (end - start) + newLen;
    if (newTotal <= 0)
        return failErr (errOut, "The replacement would leave the file empty.");
    if (!fitsAudioBuffer (newTotal))
        return failTooLong (errOut);

    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) start);
            if (newLen > 0)
                nb->copyFrom (ch, (int) start, newData[ch].data (), (int) newLen);
            nb->copyFrom (ch, (int) (start + newLen), *buffer, ch, (int) end,
                          (int) (totalSamples - end));
        }

        // Record the patch BEFORE swapping the buffer so oldData is pristine.
        recordReplace (*this, region, newData, description);
        swapInBuffer (std::move (nb), nchans, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

void AudioEngine::setSpectralPlaybackMask (std::shared_ptr<const SpectralSelection> selIn)
{
    std::lock_guard<std::mutex> lk (maskMutex);
    playSel = std::move (selIn);
    if (!playSel)
        playMask.reset();   // free the state machine when masking is off
}

void AudioEngine::setSoloAudio (std::shared_ptr<const SoloAudio> s)
{
    std::lock_guard<std::mutex> lk (soloMutex);
    solo_ = std::move (s);
}

bool AudioEngine::hasSoloAudio() const
{
    std::lock_guard<std::mutex> lk (soloMutex);
    return (bool) solo_;
}

bool AudioEngine::fadeInSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                 std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    try
    {
        applyPerSample (*this, samples, [] (float v, int i, int len)
        {
            return v * (float) i / (float) std::max (1, len - 1);
        }, chans, "Faded in");
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    return true;
}

bool AudioEngine::fadeOutSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    try
    {
        applyPerSample (*this, samples, [] (float v, int i, int len)
        {
            return v * (1.0f - (float) i / (float) std::max (1, len - 1));
        }, chans, "Faded out");
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    return true;
}

bool AudioEngine::normalizeSamples (juce::Range<int64_t> samples, float target,
                                    const std::vector<int>* chans, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;   // nothing to normalize

    // Peak is measured over the selected channels only.
    float peak = 0.0f;
    for (int ch = 0; ch < nchans; ++ch)
    {
        if (!channelAffected (chans, ch)) continue;
        const float* d = buffer->getReadPointer (ch, (int) samples.getStart());
        for (int64_t i = 0; i < samples.getLength(); ++i)
            peak = std::max (peak, std::abs (d[i]));
    }
    if (peak <= 0.0f)
        return true;   // already at target - not an error

    const float g = target / peak;
    try
    {
        applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                       "Normalized");
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    return true;
}

bool AudioEngine::gainSamples (juce::Range<int64_t> samples, float gainDb,
                               const std::vector<int>* chans, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    if (gainDb == 0.0f) return true;   // nothing to do - not an error
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;

    const float g = std::pow (10.0f, gainDb / 20.0f);
    try
    {
        applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                       juce::String ("Changed gain by ")
                           + juce::String (gainDb >= 0.0f ? "+" : "")
                           + juce::String (gainDb, 2) + " dB");
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    return true;
}

bool AudioEngine::reverseSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;

    // Only the selected channels are reversed; others keep their data.
    try
    {
        std::vector<std::vector<float>> newData (nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            float* d = buffer->getWritePointer (ch, (int) samples.getStart());
            const int64_t len = samples.getLength();
            newData[ch].resize ((size_t) len);
            if (channelAffected (chans, ch))
                for (int64_t i = 0; i < len; ++i)
                    newData[ch][i] = d[len - 1 - i];
            else
                std::copy (d, d + len, newData[ch].begin());
        }
        // Record BEFORE writing so the patch captures the pre-reverse data.
        recordReplace (*this, samples, newData, "Reversed selection");

        for (int ch = 0; ch < nchans; ++ch)
            if (channelAffected (chans, ch))
                std::copy (newData[ch].begin(), newData[ch].end(),
                           buffer->getWritePointer (ch, (int) samples.getStart()));
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::stretchRange (juce::Range<int64_t> samples, float tempoPercent,
                                std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    const int64_t len = samples.getLength();
    if (len <= 0 || std::abs ((double) tempoPercent) < 1e-6)
        return true;   // nothing to stretch

    try
    {
        // The engine holds one fixed-length buffer, so every channel is stretched
        // by the same factor - stretching a subset would desynchronise the tracks.
        const int nch = numChannels ();
        std::vector<std::vector<float>> newData ((size_t) nch);
        for (int ch = 0; ch < nch; ++ch)
            sc::stretch::stretchTempo (buffer->getReadPointer (ch, (int) samples.getStart()),
                                       (int) len, rate, tempoPercent,
                                       newData[(size_t) ch]);

        const int64_t newLen = newData.empty () ? 0 : (int64_t) newData[0].size ();
        if (newLen <= 0 || newLen == len)
            return true;   // degenerate result: leave the file untouched

        // One undoable patch over the whole buffer, so a single Undo restores both
        // the old content and the old length.
        const int64_t totalNew = totalSamples + (newLen - len);
        if (!fitsAudioBuffer (totalNew))
            return failTooLong (errOut);
        std::vector<std::vector<float>> fullNew ((size_t) nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            fullNew[(size_t) ch].resize ((size_t) totalNew, 0.0f);
            const float* src = buffer->getReadPointer (ch, 0);
            std::copy (src, src + samples.getStart (),
                       fullNew[(size_t) ch].begin ());
            std::copy (newData[(size_t) ch].data (), newData[(size_t) ch].data () + newLen,
                       fullNew[(size_t) ch].begin () + samples.getStart ());
            std::copy (src + samples.getEnd (), src + totalSamples,
                       fullNew[(size_t) ch].begin () + samples.getStart () + newLen);
        }

        auto nb = std::make_unique<juce::AudioBuffer<float>> (nch, (int) totalNew);
        for (int ch = 0; ch < nch; ++ch)
            nb->copyFrom (ch, 0, fullNew[(size_t) ch].data (), (int) totalNew);

        recordReplace (*this, { 0, totalSamples }, fullNew, "Time-stretched selection");
        swapInBuffer (std::move (nb), nch, totalNew, rate);
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps ();
    return true;
}

bool AudioEngine::eqFilterRange (juce::Range<int64_t> samples,
                                 const std::vector<sc::eq::EqPoint>& pts,
                                 const std::vector<int>* chans, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    const int64_t len = samples.getLength ();
    if (len <= 0 || sc::eq::isFlatCurve (pts))
        return true;   // nothing to filter

    try
    {
        std::vector<std::vector<float>> newData ((size_t) nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            const float* d = buffer->getReadPointer (ch, (int) samples.getStart ());
            newData[(size_t) ch].resize ((size_t) len);
            if (channelAffected (chans, ch))
            {
                std::copy (d, d + len, newData[(size_t) ch].data ());
                sc::eq::applyEq (newData[(size_t) ch].data (), (int) len, rate, pts);
            }
            else
                std::copy (d, d + len, newData[(size_t) ch].begin ());   // unchanged copy
        }

        recordReplace (*this, samples, newData, "Filter EQ applied");
        for (int ch = 0; ch < nchans; ++ch)
            if (channelAffected (chans, ch))
                std::copy (newData[(size_t) ch].data (), newData[(size_t) ch].data () + len,
                          buffer->getWritePointer (ch, (int) samples.getStart ()));
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps ();
    return true;
}

bool AudioEngine::envelopeRange (juce::Range<int64_t> samples,
                                 const std::vector<sc::env::EnvPoint>& pts,
                                 const std::vector<int>* chans, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    const int64_t len = samples.getLength ();
    if (len <= 0 || sc::env::isFlatCurve (pts))
        return true;   // nothing to envelope

    try
    {
        std::vector<std::vector<float>> newData ((size_t) nchans);
        for (int ch = 0; ch < nchans; ++ch)
        {
            const float* d = buffer->getReadPointer (ch, (int) samples.getStart ());
            newData[(size_t) ch].resize ((size_t) len);
            if (channelAffected (chans, ch))
            {
                std::copy (d, d + len, newData[(size_t) ch].data ());
                sc::env::applyEnvelope (newData[(size_t) ch].data (), (int) len, rate, pts);
            }
            else
                std::copy (d, d + len, newData[(size_t) ch].begin ());   // unchanged copy
        }

        recordReplace (*this, samples, newData, "Envelope applied");
        for (int ch = 0; ch < nchans; ++ch)
            if (channelAffected (chans, ch))
                std::copy (newData[(size_t) ch].data (), newData[(size_t) ch].data () + len,
                          buffer->getWritePointer (ch, (int) samples.getStart ()));
    }
    catch (const std::bad_alloc&)
    {
        return failNoMemory (errOut);
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps ();
    return true;
}

bool AudioEngine::resampleTo (double newRate, std::function<void(float)> onProgress,
                              const std::atomic<bool>* cancel, std::string* errOut,
                              const std::vector<std::vector<double>>* remixWeights)
{
    if (!hasFile)
        return failErr (errOut, "No audio loaded.");
    // Streaming files are materialised first so the resampler can work on a
    // contiguous buffer; this also reports out-of-memory clearly instead of
    // failing with no explanation.
    if (!ensureEditable (errOut, nullptr, cancel))
        return false;
    if (!std::isfinite (newRate) || newRate <= 0.0)
        return false;
    const bool doRemix = (remixWeights != nullptr && !remixWeights->empty());
    if (!doRemix
        && std::abs (newRate - rate) < 1e-9 * std::max (rate, newRate))
        return true; // already at that rate and no channel change requested

    // Stop playback and wait for any in-flight audio block to finish reading
    // the buffer before we swap the pointer out from under it.
    quiesceForEdit ();

    const double posSecs = position();

    try
    {
        juce::AudioBuffer<float> out;
        // On cancellation the resampler returns false before any buffer swap,
        // so the engine state is left exactly as it was.
        if (doRemix)
        {
            if (!sc::Resampler::resampleAndRemix (*buffer, rate, newRate, *remixWeights,
                                                  out, onProgress, cancel))
                return false;
        }
        else
        {
            if (!sc::Resampler::resample (*buffer, rate, newRate, out, onProgress, cancel))
                return false;
        }

        // Swap in the resampled buffer under the swap scope: playback may have
        // been restarted meanwhile and other threads read the buffer too.
        // nchans is unchanged unless a remix was applied. Engine convention:
        // totalSamples is frames PER CHANNEL (matches
        // juce::AudioFormatReader::lengthInSamples used in loadFile()).
        const int outChans = out.getNumChannels();
        const int64_t outLen = out.getNumSamples();
        swapInBuffer (std::make_unique<juce::AudioBuffer<float>> (std::move (out)),
                      outChans, outLen, newRate);
    }
    catch (const std::bad_alloc&)
    {
        return failErr (errOut, "Not enough memory to resample this file.");
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches

    // Not undoable: every recorded patch addresses positions and lengths at
    // the old rate (and possibly the old channel layout), so applying one now
    // would corrupt the document or splice out of bounds. Drop the history.
    if (undo != nullptr)
        undo->clear ();

    // (The app-level clipboard is cleared by the caller after a successful
    // resample - its samples belong to the old rate.)

    // Resampling preserves real-time duration, so seconds-based state
    // (playhead, selection, loop range, markers) stays valid as-is.
    playheadFrac.store ((double) (std::min (posSecs, durationSecs) * rate));

    clearMipmaps();   // caller rebuilds on its worker thread
    return true;
}

void AudioEngine::quiesceForEdit ()
{
    playing.store (false);
    paused.store (false);
    // No time limit: giving up after a deadline and carrying on anyway is
    // exactly the use-after-free this is meant to prevent. A block always
    // finishes (at worst after one slow streaming read).
    while (audioInBufferSection.load () > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
}

bool AudioEngine::replaceWholeFile (const juce::AudioBuffer<float>& data, double newRate,
                                    std::string* errOut)
{
    if (!hasFile || !buffer)
        return failErr (errOut, "No audio loaded.");
    if (!std::isfinite (newRate) || newRate <= 0.0
        || data.getNumChannels () <= 0 || data.getNumSamples () < 0)
        return failErr (errOut, "Invalid replacement data.");

    // Stop playback and wait for any in-flight audio block to finish reading
    // the buffer before we swap the pointer out from under it.
    quiesceForEdit ();

    const double posSecs = position();

    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> (data.getNumChannels (),
                                                             data.getNumSamples ());
        for (int ch = 0; ch < data.getNumChannels (); ++ch)
            nb->copyFrom (ch, 0, data.getReadPointer (ch), data.getNumSamples ());
        swapInBuffer (std::move (nb), data.getNumChannels (),
                      data.getNumSamples (), newRate);   // frames per channel
    }
    catch (const std::bad_alloc&)
    {
        return failErr (errOut, "Not enough memory to replace the audio.");
    }
    {
        // Streaming is no longer needed - we own the data now. (A buffer
        // exists, so readSamples() never reaches the reader any more.)
        const std::lock_guard<std::mutex> lock (readerMutex);
        reader.reset();
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches

    playheadFrac.store ((double) (std::min (posSecs, durationSecs) * rate));
    clearMipmaps();   // caller rebuilds on its worker thread
    return true;
}

bool AudioEngine::replaceWholeFileUndoable (const std::vector<std::vector<float>>& data,
                                            const juce::String& description, double newRate)
{
    if (!ensureEditable ())
        return false;
    if (data.empty () || data[0].empty ())
        return false;
    const int64_t len = (int64_t) data[0].size ();
    for (const auto& row : data)
        if ((int64_t) row.size () != len)
            return false;

    if (!fitsAudioBuffer (len))
        return false;

    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> ((int) data.size (),
                                                             (int) len);
        for (size_t ch = 0; ch < data.size (); ++ch)
            std::copy (data[ch].begin (), data[ch].end (),
                       nb->getWritePointer ((int) ch));

        // One undoable patch: old whole file + new whole file (+ channel/rate change).
        recordReplace (*this, { 0, totalSamples }, data, description,
                       (int) data.size (), newRate);

        const double r = (newRate > 0.0 && std::abs (newRate - rate) > 1e-9 * std::max (newRate, rate))
                             ? newRate : rate;
        swapInBuffer (std::move (nb), (int) data.size (), len, r);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps();
    return true;
}

bool AudioEngine::replaceWithNoiseTracks (const juce::Range<int64_t>& samples,
                                          const std::vector<std::vector<float>>& filtered,
                                          const std::vector<std::vector<float>>& noiseTracks,
                                          const juce::String& description)
{
    if (!ensureEditable ())
        return false;
    auto r = samples.getIntersectionWith ({ 0, totalSamples });
    const int64_t len = r.getLength ();
    if (len <= 0 || filtered.size () != (size_t) nchans)
        return false;
    for (const auto& row : filtered)
        if ((int64_t) row.size () != len)
            return false;
    for (const auto& row : noiseTracks)
        if ((int64_t) row.size () != len)
            return false;

    // Assemble the new whole file: existing channels with [r] replaced by the
    // enhanced signal, plus one appended channel per noise track.
    const int64_t total = totalSamples;
    std::vector<std::vector<float>> newData;
    try
    {
        newData.assign ((size_t) (nchans + noiseTracks.size()),
                        std::vector<float> ((size_t) total, 0.0f));
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    for (int ch = 0; ch < nchans; ++ch)
        if (!readSamples (ch, 0, total, newData[(size_t) ch].data ()))
            return false;

    int ai = 0;
    for (const auto& nt : noiseTracks)
    {
        std::copy (nt.begin (), nt.end(),
                   newData[(size_t) (nchans + ai)].begin () + (size_t) r.getStart ());
        ++ai;
    }
    for (int ch = 0; ch < nchans; ++ch)
        std::copy (filtered[(size_t) ch].begin (), filtered[(size_t) ch].end(),
                   newData[(size_t) ch].begin () + (size_t) r.getStart ());

    return replaceWholeFileUndoable (newData, description);
}

bool AudioEngine::appendSamples (const juce::AudioBuffer<float>& append,
                                 const juce::String& description)
{
    if (!ensureEditable ())
        return false;
    if (append.getNumSamples () <= 0 || append.getNumChannels () <= 0)
        return false;

    const int newN = std::max (nchans, append.getNumChannels ());
    const int64_t pos = totalSamples;
    const int64_t len = append.getNumSamples ();

    const int64_t newTotal = totalSamples + len;
    if (!fitsAudioBuffer (newTotal))
        return false;

    try
    {
        // Target channels beyond the appended data's own count receive silence.
        std::vector<std::vector<float>> newData (newN);
        for (int ch = 0; ch < newN; ++ch)
        {
            newData[ch].resize ((size_t) len, 0.0f);
            if (ch < append.getNumChannels ())
                std::copy (append.getReadPointer (ch), append.getReadPointer (ch) + len,
                           newData[ch].data ());
        }

        auto nb = std::make_unique<juce::AudioBuffer<float>> (newN, (int) newTotal);
        for (int ch = 0; ch < newN; ++ch)
        {
            if (ch < nchans)
                nb->copyFrom (ch, 0, *buffer, ch, 0, (int) totalSamples);
            else
                nb->clear (ch, 0, (int) pos);   // new channel: silence before the append
            nb->copyFrom (ch, (int) pos, newData[ch].data (), (int) len);
        }

        // Insert = replace the empty region [pos, pos). The existing content is
        // untouched, so oldData stays empty and undo only truncates.
        recordReplace (*this, { pos, pos }, newData, description, newN);
        swapInBuffer (std::move (nb), newN, newTotal, rate);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    unsaved = true;
    contentVer.fetch_add (1);   // edits invalidate content-keyed caches
    rebuildMipmaps ();
    return true;
}

bool AudioEngine::applyPatch (const Patch& p, bool forward, bool rebuild)
{
    if (!buffer)
        return false;

    const int64_t oldLen = p.oldEnd - p.oldStart;
    const int64_t newLen = p.newData.empty() ? 0 : (int64_t) p.newData[0].size();

    // Patches that change the channel count carry it explicitly; ordinary
    // patches keep the engine's current count.
    const int targetChans = forward ? ((p.newChans > 0) ? p.newChans : nchans)
                                    : ((p.oldChans > 0) ? p.oldChans : nchans);
    // Region of the CURRENT buffer that the patch replaces, and the data put
    // in its place.
    const int64_t curEnd   = forward ? p.oldEnd : p.oldEnd + p.lengthDelta;
    const int64_t newTotal = forward ? totalSamples + p.lengthDelta : totalSamples - p.lengthDelta;
    const auto& rows       = forward ? p.newData : p.oldData;
    const int64_t rowLen   = forward ? newLen : oldLen;

    // Reject a patch that does not fit the current buffer (stale history,
    // inconsistent rows) instead of splicing out of bounds.
    if (p.oldStart < 0 || oldLen < 0 || p.lengthDelta != newLen - oldLen
        || curEnd < p.oldStart || curEnd > totalSamples
        || newTotal < 0 || !fitsAudioBuffer (newTotal) || targetChans <= 0
        || (rowLen > 0 && (int) rows.size () < targetChans))
        return false;
    for (const auto& r : rows)
        if ((int64_t) r.size () != rowLen)
            return false;

    try
    {
        auto nb = std::make_unique<juce::AudioBuffer<float>> (targetChans, (int) newTotal);
        for (int ch = 0; ch < targetChans; ++ch)
        {
            if (ch < nchans)
            {
                // Channel exists in the current buffer: splice around it.
                nb->copyFrom (ch, 0, *buffer, ch, 0, (int) p.oldStart);
                if (ch < (int) rows.size() && rowLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, rows[(size_t) ch].data(), (int) rowLen);
                nb->copyFrom (ch, (int) (p.oldStart + rowLen), *buffer, ch, (int) curEnd,
                              (int) (totalSamples - curEnd));
            }
            else
            {
                // Channel did not exist before: silence everywhere except the
                // restored/inserted region (the new buffer is not zeroed).
                nb->clear (ch, 0, (int) newTotal);
                if (ch < (int) rows.size() && rowLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, rows[(size_t) ch].data(), (int) rowLen);
            }
        }
        // Whole-file replacement at a new rate: forward sets it, inverse
        // restores the pre-edit rate.
        double r = rate;
        if (forward && p.newRate > 0.0) r = p.newRate;
        if (!forward && p.oldRate > 0.0) r = p.oldRate;
        swapInBuffer (std::move (nb), targetChans, newTotal, r);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    contentVer.fetch_add (1);   // undo/redo changed the samples
    if (rebuild)
        rebuildMipmaps();
    return true;
}

// ---- Saving ----

namespace
{
    void putU16 (juce::OutputStream& os, uint16_t v)
    {
        const char b[2] = { (char) (v & 0xff), (char) ((v >> 8) & 0xff) };
        os.write (b, 2);
    }
    void putU32 (juce::OutputStream& os, uint32_t v)
    {
        const char b[4] = { (char) (v & 0xff), (char) ((v >> 8) & 0xff),
                            (char) ((v >> 16) & 0xff), (char) ((v >> 24) & 0xff) };
        os.write (b, 4);
    }
    void putU64 (juce::OutputStream& os, uint64_t v)
    {
        putU32 (os, (uint32_t) (v & 0xffffffffu));
        putU32 (os, (uint32_t) (v >> 32));
    }
    void putFourCC (juce::OutputStream& os, const char* s) { os.write (s, 4); }
    auto writeChunkPayload = [] (juce::OutputStream& os, const std::string& p) -> bool
    {
        bool ok = os.write (p.data (), p.size());
        const char padByte = 0;
        if (ok && (p.size () & 1))   // RIFF word-alignment pad byte
            ok = os.write (&padByte, 1);
        return ok;
    };
}

// The legacy DAW cue / LIST(adtl) payload builders live in ScttChunk.cpp so the
// full WAV writer and the surgical patcher share one byte-exact implementation.

// Writes a RIFF/WAVE file (IEEE float32) with the markers embedded in the
// classic DAW marker layout so that saved files load cleanly elsewhere:
//   fmt -> data -> cue -> LIST(adtl)
// The "cue" chunk holds one 24-byte entry per marker; the adtl list contains
// an "ltxt" region-length entry for every marker, then a "labl" title entry
// and (when non-empty) a "note" comment entry.
bool AudioEngine::saveWavWithMarkers (const juce::File& file) const
{
    if (!hasFile || nchans <= 0)
        return false;

    const int64_t dataSize = totalSamples * (int64_t) nchans * 4; // float32 LE

    // Metadata chunks are only written in embedded mode (see include/FileMeta.h).
    const bool embedded = saveMetaMode == nullptr || *saveMetaMode == sc::MetaMode::Embedded;

    std::string cuePayload, adtlPayload, scmdPayload;
    if (embedded && saveMarkers != nullptr)
    {
        cuePayload  = buildCueChunkPayload (*saveMarkers, rate, totalSamples);
        adtlPayload = buildAdtlListPayload (*saveMarkers, rate, totalSamples);
    }

    // SCTT chunk: transcription segments as JSON (see include/ScttChunk.h).
    const std::string* sctt = (embedded && saveScttJson != nullptr && !saveScttJson->empty ())
                              ? saveScttJson : nullptr;
    if (embedded && saveScmdBuilder)
        scmdPayload = saveScmdBuilder();   // unified bundle JSON for the SCMD chunk

    // "bkdk" calibration chunk: preserved byte-for-byte from the source
    // file whenever it was present there (independent of metadata mode).
    const std::string* bkdk = (saveBkdkPayload != nullptr && !saveBkdkPayload->empty ())
                              ? saveBkdkPayload : nullptr;

    // RIFF chunks must be word-aligned: odd-sized payloads get one pad byte.
    auto chunkBytes = [] (const std::string& p) -> int64
    { return p.empty() ? 0 : 8 + (int64_t) p.size () + ((p.size () & 1) ? 1 : 0); };
    const int64_t cueChunk  = chunkBytes (cuePayload);
    const int64_t listChunk = chunkBytes (adtlPayload);
    const int64_t scttChunk = sctt != nullptr ? chunkBytes (*sctt) : 0;
    const int64_t scmdChunk = chunkBytes (scmdPayload);
    const int64_t bkdkChunk = bkdk != nullptr ? chunkBytes (*bkdk) : 0;

    int64_t riffSize = 4            // "WAVE"
                     + 8 + 16       // fmt chunk
                     + 8 + dataSize // data chunk
                     + cueChunk
                     + listChunk
                     + scttChunk
                     + scmdChunk
                     + bkdkChunk;

    // RIFF sizes are 32-bit: past 4 GiB write RF64 (EBU Tech 3306), whose
    // "ds64" chunk carries the 64-bit RIFF/data sizes and the 32-bit fields
    // hold 0xFFFFFFFF. (A plain RIFF header would wrap modulo 2^32.)
    const bool rf64 = forceRf64 || riffSize + 8 + 28 > (int64_t) 0xffffffffLL;
    if (rf64)
        riffSize += 8 + 28;   // ds64 chunk

    // Write to a temporary file next to the target and only replace the
    // target once everything was written and verified: a failure (disk full,
    // network drop, ...) must never destroy the existing file.
    juce::TemporaryFile temp (file);
    auto out = std::make_unique<juce::FileOutputStream> (temp.getFile());
    if (out->failedToOpen())
        return false;

    if (rf64)
    {
        putFourCC (*out, "RF64"); putU32 (*out, 0xffffffffu); putFourCC (*out, "WAVE");
        putFourCC (*out, "ds64"); putU32 (*out, 28);
        putU64 (*out, (uint64_t) riffSize);
        putU64 (*out, (uint64_t) dataSize);
        putU64 (*out, (uint64_t) totalSamples);   // sample (frame) count
        putU32 (*out, 0);                         // table length
    }
    else
    {
        putFourCC (*out, "RIFF"); putU32 (*out, (uint32_t) riffSize); putFourCC (*out, "WAVE");
    }

    // fmt chunk: IEEE float 32-bit PCM
    putFourCC (*out, "fmt "); putU32 (*out, 16);
    putU16 (*out, 3);                                  // WAVE_FORMAT_IEEE_FLOAT
    putU16 (*out, (uint16_t) nchans);
    const uint32_t rateU = (uint32_t) std::llround (rate);
    putU32 (*out, rateU);
    const uint32_t blockAlign = (uint32_t) (nchans * 4);
    putU32 (*out, rateU * blockAlign);                 // byte rate
    putU16 (*out, (uint16_t) blockAlign);
    putU16 (*out, 32);                                 // bits per sample

    // data chunk comes before the metadata chunks (legacy DAW convention).
    putFourCC (*out, "data");
    putU32 (*out, rf64 ? 0xffffffffu : (uint32_t) dataSize);

    bool ok = true;
    const int64_t block = 1 << 20; // frames per write call
    const int64_t frames = dataSize / ((int64_t) nchans * 4);   // as declared in the header
    const int chans = nchans;
    if (buffer)
    {
        for (int64_t pos = 0; pos < frames && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, frames - pos);
            std::vector<float> tmp ((size_t) n * chans);
            {
                // Saves run on a worker thread: hold off buffer swaps (edits,
                // recording) while reading a block, and fail rather than
                // write a file whose content changed shape mid-save.
                const std::shared_lock<std::shared_mutex> bufLock (bufferMutex);
                ok = buffer != nullptr && nchans == chans && totalSamples == frames;
                for (int i = 0; ok && i < n; ++i)
                    for (int ch = 0; ch < chans; ++ch)
                        tmp[(size_t) i * chans + ch] = buffer->getReadPointer (ch, (int) pos)[i];
            }
            ok = ok && out->write (tmp.data(), tmp.size() * sizeof (float));
        }
    }
    else if (reader)
    {
        juce::AudioBuffer<float> blockBuf (nchans, (int) block);
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, totalSamples - pos);
            {
                // Locked per block, not for the whole save: playback and the
                // views read through the same reader meanwhile.
                const std::lock_guard<std::mutex> lock (readerMutex);
                ok = reader != nullptr
                     && reader->read (blockBuf.getArrayOfWritePointers(), nchans, pos, n);
            }
            if (!ok)
                break;
            std::vector<float> tmp ((size_t) n * nchans);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < nchans; ++ch)
                    tmp[(size_t) i * nchans + ch] = blockBuf.getReadPointer (ch)[i];
            ok = out->write (tmp.data(), tmp.size() * sizeof (float));
        }
    }

    // cue chunk: one 24-byte legacy DAW entry per marker.
    if (!cuePayload.empty() && ok)
    {
        putFourCC (*out, "cue ");
        putU32 (*out, (uint32_t) cuePayload.size());
        ok = writeChunkPayload (*out, cuePayload);
    }

    // LIST chunk with the adtl sub-chunk.
    if (!adtlPayload.empty() && ok)
    {
        putFourCC (*out, "LIST");
        putU32 (*out, (uint32_t) adtlPayload.size());
        ok = writeChunkPayload (*out, adtlPayload);
    }

    // SCTT chunk: transcription segments (JSON payload).
    if (scttChunk > 0 && ok)
    {
        putFourCC (*out, "SCTT");
        putU32 (*out, (uint32_t) sctt->size());
        ok = writeChunkPayload (*out, *sctt);
    }

    // SCMD chunk: the unified metadata bundle as JSON (see include/FileMeta.h).
    if (!scmdPayload.empty() && ok)
    {
        putFourCC (*out, "SCMD");
        putU32 (*out, (uint32_t) scmdPayload.size());
        ok = writeChunkPayload (*out, scmdPayload);
    }

    // bkdk chunk: the source file's calibration data, verbatim.
    if (bkdkChunk > 0 && ok)
    {
        putFourCC (*out, "bkdk");
        putU32 (*out, (uint32_t) bkdk->size());
        ok = writeChunkPayload (*out, *bkdk);
    }

    // Every write error (including the 32/64-bit header writes, whose results
    // are not checked one by one) ends up in the stream status; the final
    // size check also catches short writes that set no error.
    out->flush();
    ok = ok && out->getStatus().wasOk();
    out.reset();   // close before verifying and renaming
    ok = ok && temp.getFile().getSize() == riffSize + 8;
    ok = ok && commitSavedFile (temp, file);

    if (ok)
    {
        unsaved = false;
        currentFile = file;
    }
    return ok;
}

// Moves a complete temporary save over `file`. A streaming reader open on
// that same path is closed for the swap (Windows refuses to replace an open
// file) and reopened on the new file, which holds the same audio. After a
// "Save As" elsewhere the reader also follows the new file, so the document
// no longer depends on the old path.
bool AudioEngine::commitSavedFile (const juce::TemporaryFile& temp, const juce::File& file) const
{
    const std::lock_guard<std::mutex> lock (readerMutex);
    if (reader == nullptr)
        return temp.overwriteTargetFileWithTemporary();

    const bool sameFile = (file == currentFile);
    if (sameFile)
        reader.reset();
    const bool ok = temp.overwriteTargetFileWithTemporary();
    if (ok || sameFile)
    {
        // Same path: the old reader is gone, take any matching reader. Save
        // As: keep the old reader unless the new file matches exactly.
        std::unique_ptr<juce::AudioFormatReader> r (fm->createReaderFor (ok ? file : currentFile));
        if (r != nullptr && (int) r->numChannels == nchans
            && (sameFile || r->lengthInSamples == totalSamples))
            reader = std::move (r);
    }
    return ok;
}

bool AudioEngine::saveFile (const juce::File& file) const
{
    // Diagnostic log: every save (Save, Save As, export through the engine)
    // ends here; the path is written in clear (internal use).
    const double diagT0 = sc::diag::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    const bool ok = saveFileImpl (file);
    if (diagT0 > 0.0)
    {
        const double ms = juce::Time::getMillisecondCounterHiRes() - diagT0;
        sc::diag::event ("file.save", !ok ? sc::diag::Level::error
                                      : ms > 5000.0 ? sc::diag::Level::warn : sc::diag::Level::info,
                         { { "path", file.getFullPathName() },
                           { "fmt", file.getFileExtension().trimCharactersAtStart (".").toLowerCase() },
                           { "ok", ok }, { "ch", nchans },
                           { "sizeMb", ok ? juce::var (std::round ((double) file.getSize() / (1024.0 * 102.4)) / 10.0) : juce::var() },
                           { "durMs", juce::roundToInt (ms) } });
    }
    return ok;
}

bool AudioEngine::saveFileImpl (const juce::File& file) const
{
    if (!hasFile)
        return false;

    // WAV files use the custom writer so markers can be embedded as standard
    // RIFF chunks ("cue" + "LIST"/"adtl"). Other formats go through JUCE.
    if (file.getFileExtension().toLowerCase() == ".wav")
        return saveWavWithMarkers (file);

    auto* format = fm->findFormatForFileExtension (file.getFileExtension());
    if (!format)
        format = fm->getDefaultFormat();
    if (!format)
        return false;

    // Atomic like the WAV path: write a temporary file, then replace.
    juce::TemporaryFile temp (file);
    auto fileStream = std::make_unique<juce::FileOutputStream> (temp.getFile());
    if (fileStream->failedToOpen())
        return false;
    std::unique_ptr<juce::OutputStream> stream (std::move (fileStream)); // writer takes ownership

    const juce::AudioFormatWriterOptions options =
        juce::AudioFormatWriterOptions {}.withNumChannels (nchans).withSampleRate (rate);

    auto writer = format->createWriterFor (stream, options);
    if (!writer)
        return false;

    bool ok = true;
    const int64_t block = 1 << 20; // samples per write call

    if (buffer)
    {
        const int64_t frames = totalSamples;
        const int chans = nchans;
        for (int64_t pos = 0; pos < frames && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, frames - pos);
            // Hold off buffer swaps per block (see saveWavWithMarkers).
            const std::shared_lock<std::shared_mutex> bufLock (bufferMutex);
            ok = buffer != nullptr && nchans == chans && totalSamples == frames
                 && writer->writeFromAudioSampleBuffer (*buffer, (int) pos, n);
        }
    }
    else if (reader)
    {
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int64_t n = std::min ((int64_t) block, totalSamples - pos);
            // Locked per block, not for the whole save (see saveWavWithMarkers).
            const std::lock_guard<std::mutex> lock (readerMutex);
            ok = reader != nullptr && writer->writeFromAudioReader (*reader, pos, n);
        }
    }

    // The writer finalises the header and closes the stream when destroyed;
    // its errors are not reported, so read the result back before replacing
    // the target with it.
    writer.reset();
    if (ok)
    {
        std::unique_ptr<juce::AudioFormatReader> check (fm->createReaderFor (temp.getFile()));
        ok = check != nullptr && (int) check->numChannels == nchans
             && (totalSamples == 0 || check->lengthInSamples > 0);
    }
    ok = ok && commitSavedFile (temp, file);

    if (ok)
    {
        unsaved = false;
        currentFile = file;
    }
    return ok;
}

bool AudioEngine::saveFileAs (const juce::File& file)
{
    if (!hasFile)
        return false;
    const bool ok = saveFile (file);
    if (ok)
        currentFile = file;
    return ok;
}

double AudioEngine::snapToZeroCrossing (double timeSecs, double windowSecs, int channel) const
{
    if (!hasFile || channel < 0 || channel >= nchans)
        return timeSecs;

    const int64_t center = (int64_t) (timeSecs * rate);
    const int64_t win = (int64_t) (windowSecs * rate);
    const int64_t lo = std::max ((int64_t) 0, center - win);
    const int64_t hi = std::min (totalSamples - 1, center + win);
    if (hi <= lo)
        return timeSecs;

    // Read the window (bounded size)
    const int64_t n = hi - lo + 1;
    std::vector<float> buf ((size_t) n);
    if (!readSamples (channel, lo, n, buf.data()))
        return timeSecs;

    // Find nearest sign change to center
    int64_t best = -1;
    int64_t bestDist = std::numeric_limits<int64_t>::max();
    for (int64_t i = 0; i + 1 < n; ++i)
    {
        const float a = buf[i], b = buf[i + 1];
        if ((a < 0.0f && b > 0.0f) || (a > 0.0f && b < 0.0f))
        {
            const double cross = (double) (lo + i) + (double) (0.0 - a) / (b - a);
            const int64_t ci = (int64_t) cross;
            const int64_t dist = std::abs (ci - center);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = ci;
            }
        }
    }
    if (best < 0)
        return timeSecs;
    return (double) best / rate;
}
