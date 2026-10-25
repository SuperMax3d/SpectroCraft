/**
 * SpectroCraft - Audio Engine Implementation
 */

#include "AudioEngine.h"
#include "VideoAudio.h"
#include "SpectralMaskProcessor.h"
#include "Resampler.h"
#include "UndoManager.h"
#include "MarkerManager.h"
#include <algorithm>
#include <cmath>
#include <new>

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
    if (inMemoryLimit > 0 && (int64_t) nchans * totalSamples <= inMemoryLimit)
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
            rawReader->read (buffer.get(), (int) off, cnt, (int) off, true, true);
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
    return true;
}

void AudioEngine::closeFile()
{
    buffer.reset();
    reader.reset();
    clearMipmaps();
    currentFile = {};
    nchans = 0; totalSamples = 0; rate = 44100.0; durationSecs = 0.0;
    hasFile = false; unsaved = false;
    playing = false; paused = false; looping = false;
    playheadFrac = 0.0;
    playbackRate = 0.0;
}

bool AudioEngine::readSamples (int channel, int64_t start, int64_t count, float* dest) const
{
    if (!hasFile || channel < 0 || channel >= nchans || !dest)
        return false;

    // NOTE: std::clamp(value, lo, hi) — value first!
    start = std::clamp<int64_t> (start, (int64_t) 0, totalSamples);
    count = std::clamp<int64_t> (count, (int64_t) 0, totalSamples - start);

    if (buffer)
    {
        const float* src = buffer->getReadPointer (channel, (int) start);
        std::copy (src, src + count, dest);
        return true;
    }

    if (reader)
    {
        const std::lock_guard<std::mutex> lock (readerMutex);
        juce::AudioBuffer<float> tmp (1, (int) count);
        if (!reader->read (&tmp, 0, (int) count, start, true, true))
            return false;
        std::copy (tmp.getReadPointer (0), tmp.getReadPointer (0) + count, dest);
        return true;
    }
    return false;
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

    if (!playing || !hasFile)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // Track that we are inside a section reading `buffer`: resampleTo() waits
    // for this to drain before swapping the buffer pointer.
    struct BufferSectionGuard
    {
        std::atomic<int>& counter;
        explicit BufferSectionGuard (std::atomic<int>& c) : counter (c) { ++counter; }
        ~BufferSectionGuard() { --counter; }
    } bufferSectionGuard (audioInBufferSection);

    // Advance the fractional playhead by (deviceRate / fileRate) per output
    // sample; step == 1.0 when the rates match.
    const double step = (playbackRate > 0.0 && std::abs (playbackRate - rate) > 1e-3)
                         ? playbackRate / rate : 1.0;
    double pos = playheadFrac.load();

    if (looping && loopRange_.getLength() > 0)
    {
        const double ls = loopRange_.getStart() * rate;
        const double le = loopRange_.getEnd() * rate;
        const double len = le - ls;
        if (len > 0.0 && pos >= le)
            pos = ls + std::fmod (pos - ls, len);
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

    const int chans = std::min (dst->getNumChannels(), nchans);
    std::vector<std::vector<float>> tmp ((size_t) chans);
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

    const float g = gain_.load();
    for (int i = 0; i < n; ++i)
    {
        const double p = pos + (double) i * step - (double) readStart;
        for (int ch = 0; ch < chans; ++ch)
        {
            float* d = dst->getWritePointer (ch, bufferToFill.startSample);
            if (p >= 0.0 && p < (double) ctxBefore + count - 1.0)
            {
                // Linear interpolation between the two surrounding file samples.
                // (p is relative to readStart, which includes the mask's
                // pre-context; tmp indices are absolute within that buffer.)
                const int64_t a = (int64_t) std::floor (p);
                const float fr = (float) (p - (double) a);
                d[i] = (tmp[(size_t) ch][(size_t) a] * (1.0f - fr)
                        + tmp[(size_t) ch][(size_t) a + 1] * fr) * g;
            }
            else
            {
                d[i] = 0.0f; // past end of file -> silence
            }
        }
    }

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
    playheadFrac = newPos;
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
                ok = eng.reader->read (buf.getArrayOfWritePointers(), nchans,
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
    return true;
}

void AudioEngine::clearMipmaps() { mips.clear(); baseFactor = 1; }

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
                           int newChans = 0)
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

    nchans = numChans;
    rate = sampleRate;
    totalSamples = len;
    durationSecs = (double) len / rate;
    hasFile = true;
    unsaved = len > 0;   // content that exists nowhere else is "unsaved"
    currentFile = juce::File();   // no path until Save As

    buffer = std::make_unique<juce::AudioBuffer<float>> (numChans, (int) len);
    if (data != nullptr && len > 0)
        for (int ch = 0; ch < numChans; ++ch)
            std::copy ((*data)[(size_t) ch].begin(), (*data)[(size_t) ch].end(),
                       buffer->getWritePointer (ch));

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

    // Insert = replace empty region [pos, pos) with newData
    recordReplace (*this, { samplePos, samplePos }, newData, "Pasted samples");

    // Apply forward patch manually (recordReplace only records).
    const int64_t newTotal = totalSamples + pasteLen;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
    for (int ch = 0; ch < nchans; ++ch)
    {
        nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
        nb->copyFrom (ch, (int) samplePos, newData[ch].data(), (int) pasteLen);
        nb->copyFrom (ch, (int) (samplePos + pasteLen), *buffer, ch, (int) samplePos,
                      (int) (totalSamples - samplePos));
    }
    buffer = std::move (nb);
    totalSamples = newTotal;
    durationSecs = (double) totalSamples / rate;
    unsaved = true;
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

    // One undoable patch: region [pos, EOF) replaced by mixed + appended data.
    recordReplace (*this, { samplePos, totalSamples }, newData,
                  "Mixed in pasted samples");

    // Apply forward patch manually (recordReplace only records).
    const int64_t newTotal = totalSamples + tail;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
    for (int ch = 0; ch < nchans; ++ch)
    {
        nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
        nb->copyFrom (ch, (int) samplePos, newData[ch].data(),
                      (int) (totalSamples - samplePos + tail));
    }
    buffer = std::move (nb);
    totalSamples = newTotal;
    durationSecs = (double) totalSamples / rate;
    unsaved = true;
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

    recordReplace (*this, { 0, totalSamples }, newData, "Pasted to new channel(s)",
                  nchans + addChans);

    // Apply the forward patch manually: a whole-file replacement is just a swap.
    auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans + addChans, (int) totalSamples);
    for (int ch = 0; ch < nchans + addChans; ++ch)
        std::copy (newData[(size_t) ch].data(), newData[(size_t) ch].data() + totalSamples,
                   nb->getWritePointer (ch));
    buffer = std::move (nb);
    nchans += addChans;
    unsaved = true;
    rebuildMipmaps();
    return true;
}

bool AudioEngine::deleteSamples (juce::Range<int64_t> samples, std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;   // nothing to delete
    recordReplace (*this, samples, {}, "Deleted samples");

    const int64_t delLen = samples.getLength();
    const int64_t newTotal = totalSamples - delLen;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
    for (int ch = 0; ch < nchans; ++ch)
    {
        nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samples.getStart());
        nb->copyFrom (ch, (int) samples.getStart(), *buffer, ch, (int) samples.getEnd(),
                      (int) (totalSamples - samples.getEnd()));
    }
    buffer = std::move (nb);
    totalSamples = newTotal;
    durationSecs = (double) totalSamples / rate;
    unsaved = true;
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
    std::vector<std::vector<float>> newData (nchans);
    for (int ch = 0; ch < nchans; ++ch)
    {
        newData[ch].resize ((size_t) len, 0.0f);
        if (!channelAffected (chans, ch))
            readSamples (ch, samples.getStart(), len, newData[ch].data());
    }
    recordReplace (*this, samples, std::move (newData), "Silenced selection");

    for (int ch = 0; ch < nchans; ++ch)
        if (channelAffected (chans, ch))
            buffer->clear (ch, (int) samples.getStart(), (int) len);
    unsaved = true;
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

    // Stop playback and wait for any in-flight audio block to finish reading
    // before we swap the source out from under it (same pattern as resampleTo).
    playing.store (false);
    paused.store (false);
    const int64_t deadline = juce::Time::getMillisecondCounter() + 100;
    while (audioInBufferSection.load (std::memory_order_acquire) > 0
           && juce::Time::getMillisecondCounter() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

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
            return reader->read (nb.get(), (int) off, cnt, (int) off, true, true); 
        } ();
        if (!ok)
            return fail ("Failed to read the file for editing.");
        if (onProgress != nullptr)
            onProgress ((float) ((double) (off + cnt) / (double) totalSamples));
    }

    // The buffer is now the authoritative source of truth; drop the reader.
    buffer = std::move (nb);
    reader.reset ();
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

    // Record the patch BEFORE touching the buffer so oldData is pristine.
    recordReplace (*this, samples, newData, description);

    const int64_t len = samples.getLength();
    for (int ch = 0; ch < nchans && ch < (int) newData.size (); ++ch)
        if (newData[ch].size () == (size_t) len)
            std::copy (newData[ch].begin (), newData[ch].end (),
                       buffer->getWritePointer (ch, (int) samples.getStart ()));

    unsaved = true;
    return true;
}

void AudioEngine::setSpectralPlaybackMask (std::shared_ptr<const SpectralSelection> selIn)
{
    std::lock_guard<std::mutex> lk (maskMutex);
    playSel = std::move (selIn);
    if (!playSel)
        playMask.reset();   // free the state machine when masking is off
}

bool AudioEngine::fadeInSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                 std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    applyPerSample (*this, samples, [] (float v, int i, int len)
    {
        return v * (float) i / (float) std::max (1, len - 1);
    }, chans, "Faded in");
    return true;
}

bool AudioEngine::fadeOutSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    applyPerSample (*this, samples, [] (float v, int i, int len)
    {
        return v * (1.0f - (float) i / (float) std::max (1, len - 1));
    }, chans, "Faded out");
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
    applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                   "Normalized");
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
    applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                   juce::String ("Changed gain by ")
                       + juce::String (gainDb >= 0.0f ? "+" : "")
                       + juce::String (gainDb, 2) + " dB");
    return true;
}

bool AudioEngine::reverseSamples (juce::Range<int64_t> samples, const std::vector<int>* chans,
                                  std::string* errOut)
{
    if (!ensureEditable (errOut)) return false;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return true;

    // Only the selected channels are reversed; others keep their data.
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
    unsaved = true;
    rebuildMipmaps();
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
    playing.store (false);
    paused.store (false);
    const int64_t deadline = juce::Time::getMillisecondCounter() + 100;
    while (audioInBufferSection.load (std::memory_order_acquire) > 0
           && juce::Time::getMillisecondCounter() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    const double posSecs = position();

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

    // Swap in the resampled buffer. The audio thread is idle (playing==false
    // and no block in flight), so this cannot race with playback reads.
    auto oldBuffer = std::move (buffer);
    buffer = std::make_unique<juce::AudioBuffer<float>> (std::move (out));

    rate = newRate;
    nchans = buffer->getNumChannels();   // unchanged unless a remix was applied
    // Engine convention: totalSamples is frames PER CHANNEL (matches
    // juce::AudioFormatReader::lengthInSamples used in loadFile()).
    totalSamples = buffer->getNumSamples();
    durationSecs = (double) totalSamples / rate;
    unsaved = true;

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
    const int64_t deadline = juce::Time::getMillisecondCounter() + 100;
    while (audioInBufferSection.load (std::memory_order_acquire) > 0
           && juce::Time::getMillisecondCounter() < deadline)
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

    reader.reset();   // streaming is no longer needed - we own the data now
    auto oldBuffer = std::move (buffer);
    buffer = std::make_unique<juce::AudioBuffer<float>> (data.getNumChannels (),
                                                        data.getNumSamples ());
    for (int ch = 0; ch < data.getNumChannels (); ++ch)
        buffer->copyFrom (ch, 0, data.getReadPointer (ch), data.getNumSamples ());

    rate = newRate;
    nchans = buffer->getNumChannels ();
    totalSamples = buffer->getNumSamples ();   // frames per channel
    durationSecs = (double) totalSamples / rate;
    unsaved = true;

    playheadFrac.store ((double) (std::min (posSecs, durationSecs) * rate));
    clearMipmaps();   // caller rebuilds on its worker thread
    return true;
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

    // Target channels beyond the appended data's own count receive silence.
    std::vector<std::vector<float>> newData (newN);
    for (int ch = 0; ch < newN; ++ch)
    {
        newData[ch].resize ((size_t) len, 0.0f);
        if (ch < append.getNumChannels ())
            std::copy (append.getReadPointer (ch), append.getReadPointer (ch) + len,
                       newData[ch].data ());
    }

    // Insert = replace the empty region [pos, pos). The existing content is
    // untouched, so oldData stays empty and undo only truncates.
    recordReplace (*this, { pos, pos }, newData, description, newN);

    const int64_t newTotal = totalSamples + len;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (newN, (int) newTotal);
    for (int ch = 0; ch < newN; ++ch)
    {
        if (ch < nchans)
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) totalSamples);
        nb->copyFrom (ch, (int) pos, newData[ch].data (), (int) len);
    }
    buffer = std::move (nb);
    nchans = newN;
    totalSamples = newTotal;
    durationSecs = (double) totalSamples / rate;
    unsaved = true;
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
    if (forward)
    {
        const int targetChans = (p.newChans > 0) ? p.newChans : nchans;
        const int64_t newTotal = totalSamples + p.lengthDelta;
        auto nb = std::make_unique<juce::AudioBuffer<float>> (targetChans, (int) newTotal);
        for (int ch = 0; ch < targetChans; ++ch)
        {
            if (ch < nchans)
            {
                // Channel exists in the current buffer: splice around it.
                nb->copyFrom (ch, 0, *buffer, ch, 0, (int) p.oldStart);
                if (ch < (int) p.newData.size() && newLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, p.newData[ch].data(), (int) newLen);
                nb->copyFrom (ch, (int) (p.oldStart + newLen), *buffer, ch, (int) p.oldEnd,
                              (int) (totalSamples - p.oldEnd));
            }
            else
            {
                // Channel did not exist before: silence everywhere except the
                // inserted region.
                if (ch < (int) p.newData.size() && newLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, p.newData[ch].data(), (int) newLen);
            }
        }
        buffer = std::move (nb);
        nchans = targetChans;
        totalSamples = newTotal;
    }
    else
    {
        // Inverse: region in current buffer is [oldStart, oldEnd + lengthDelta)
        const int targetChans = (p.oldChans > 0) ? p.oldChans : nchans;
        const int64_t curEnd = p.oldEnd + p.lengthDelta;
        const int64_t newTotal = totalSamples - p.lengthDelta;
        auto nb = std::make_unique<juce::AudioBuffer<float>> (targetChans, (int) newTotal);
        for (int ch = 0; ch < targetChans; ++ch)
        {
            if (ch < nchans)
            {
                // Channel exists in the current buffer: splice around it.
                nb->copyFrom (ch, 0, *buffer, ch, 0, (int) p.oldStart);
                if (ch < (int) p.oldData.size() && oldLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, p.oldData[ch].data(), (int) oldLen);
                nb->copyFrom (ch, (int) (p.oldStart + oldLen), *buffer, ch, (int) curEnd,
                              (int) (totalSamples - curEnd));
            }
            else
            {
                // Channel did not exist before the edit: silence everywhere
                // except where the inverse patch restores data.
                if (ch < (int) p.oldData.size() && oldLen > 0)
                    nb->copyFrom (ch, (int) p.oldStart, p.oldData[ch].data(), (int) oldLen);
            }
        }
        buffer = std::move (nb);
        nchans = targetChans;
        totalSamples = newTotal;
    }

    durationSecs = (double) totalSamples / rate;
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
    void putFourCC (juce::OutputStream& os, const char* s) { os.write (s, 4); }
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

    const int64_t cueChunk  = !cuePayload.empty()  ? 8 + (int64_t) cuePayload.size () : 0;
    const int64_t listChunk = !adtlPayload.empty() ? 8 + (int64_t) adtlPayload.size () : 0;
    const int64_t scttChunk = sctt != nullptr     ? 8 + (int64_t) sctt->size () : 0;
    const int64_t scmdChunk = !scmdPayload.empty() ? 8 + (int64_t) scmdPayload.size () : 0;

    const int64_t riffSize = 4            // "WAVE"
                           + 8 + 16       // fmt chunk
                           + 8 + dataSize // data chunk
                           + cueChunk
                           + listChunk
                           + scttChunk
                           + scmdChunk;

    file.deleteFile(); // FileOutputStream appends to existing files!
    auto out = std::make_unique<juce::FileOutputStream> (file);
    if (out->failedToOpen())
        return false;

    putFourCC (*out, "RIFF"); putU32 (*out, (uint32_t) riffSize); putFourCC (*out, "WAVE");

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
    putU32 (*out, (uint32_t) dataSize);

    bool ok = true;
    const int64_t block = 1 << 20; // frames per write call
    if (buffer)
    {
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, totalSamples - pos);
            std::vector<float> tmp ((size_t) n * nchans);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < nchans; ++ch)
                    tmp[(size_t) i * nchans + ch] = buffer->getReadPointer (ch, (int) pos)[i];
            ok = out->write (tmp.data(), tmp.size() * sizeof (float));
        }
    }
    else if (reader)
    {
        const std::lock_guard<std::mutex> lock (readerMutex);
        juce::AudioBuffer<float> blockBuf (nchans, (int) block);
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, totalSamples - pos);
            if (!reader->read (&blockBuf, 0, n, pos, true, true))
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
        ok = out->write (cuePayload.data(), cuePayload.size());
    }

    // LIST chunk with the adtl sub-chunk.
    if (!adtlPayload.empty() && ok)
    {
        putFourCC (*out, "LIST");
        putU32 (*out, (uint32_t) adtlPayload.size());
        ok = out->write (adtlPayload.data(), adtlPayload.size());
    }

    // SCTT chunk: transcription segments (JSON payload).
    if (scttChunk > 0 && ok)
    {
        putFourCC (*out, "SCTT");
        putU32 (*out, (uint32_t) (scttChunk - 8));
        ok = out->write (sctt->data (), (size_t) sctt->size ());
    }

    // SCMD chunk: the unified metadata bundle as JSON (see include/FileMeta.h).
    if (!scmdPayload.empty() && ok)
    {
        putFourCC (*out, "SCMD");
        putU32 (*out, (uint32_t) scmdPayload.size());
        ok = out->write (scmdPayload.data(), scmdPayload.size());
    }

    if (ok)
    {
        unsaved = false;
        currentFile = file;
    }
    return ok;
}

bool AudioEngine::saveFile (const juce::File& file) const
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

    file.deleteFile(); // FileOutputStream appends to existing files!
    auto fileStream = std::make_unique<juce::FileOutputStream> (file);
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
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int n = (int) std::min ((int64_t) block, totalSamples - pos);
            ok = writer->writeFromAudioSampleBuffer (*buffer, (int) pos, n);
        }
    }
    else if (reader)
    {
        const std::lock_guard<std::mutex> lock (readerMutex);
        for (int64_t pos = 0; pos < totalSamples && ok; pos += block)
        {
            const int64_t n = std::min ((int64_t) block, totalSamples - pos);
            ok = writer->writeFromAudioReader (*reader, pos, n);
        }
    }

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
