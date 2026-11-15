/**
 * SpectroCraft - Spectral Engine Implementation
 * Tiled FFT spectrogram: LRU tile cache, parallel frame computation (ThreadPool),
 * bilinear resampling, colour LUT.
 *
 * Performance notes:
 *  - Window functions are precomputed once per (fftSize, windowType).
 *  - Tiles are split into 64-frame chunks so even a single missing tile uses
 *    all cores; chunk jobs write disjoint frame ranges (no locks on data).
 *  - FFTs use scfft::realForward: AVX2 vectorized when available, JUCE scalar
 *    otherwise.
 *  - Tiles store raw dB magnitudes; gain/bias/dB-range/colour scaling is applied
 *    at render time, so those settings never force tile recomputation.
 */

#include "SpectralEngine.h"
#include "Settings.h"
#include "Diagnostics.h"
#include "PngWriter.h"
#include "FFTHelpers.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <unordered_map>

int SpectralEngine::chooseFftSize (double visibleSeconds, double sampleRate,
                                   int heightPixels, double minFreqHz, double maxFreqHz,
                                   double targetTimeSlices, double binsPerPixel)
{
    // Time constraint (X zoom): keep about `targetTimeSlices` FFT frames across
    // the visible range; with hop ~ FFT/4 that bounds the largest useful size.
    const double maxHopSeconds = std::max (1e-9,
                                           visibleSeconds / std::max (1e-9, targetTimeSlices));
    int maxFftFromTime = (int) std::round (maxHopSeconds * sampleRate * 4.0);

    // Frequency constraint (Y zoom): keep about `binsPerPixel` spectral bins per
    // pixel of the frequency axis; that sets the smallest size that resolves it.
    int minFftFromFreq = 256;
    const double visibleFreqRange = maxFreqHz - minFreqHz;
    if (visibleFreqRange > 0.0 && heightPixels > 0 && binsPerPixel > 0.0)
        minFftFromFreq = (int) std::ceil (sampleRate
                                          / (visibleFreqRange / ((double) heightPixels * binsPerPixel)));

    // Combine: the frequency zoom sets a floor, the time zoom a ceiling.
    int ideal = std::max (minFftFromFreq, 256);
    ideal = std::min (ideal, maxFftFromTime);

    // Snap to power of two and clamp.
    int fft = 256;
    while (fft < ideal && fft < 16384)
        fft <<= 1;

    return juce::jlimit (256, 16384, fft);
}

namespace
{
    // Precomputed window functions, shared across threads.
    struct WindowCache
    {
        std::mutex mutex;
        std::unordered_map<uint64_t, std::shared_ptr<std::vector<float>>> windows;
    };

    // Returns nullptr for the rectangular window (no windowing needed).
    const std::vector<float>* getWindow (int fftSize, int windowType)
    {
        if (windowType == 0) // rectangular
            return nullptr;

        static WindowCache wc;
        const uint64_t key = ((uint64_t)(unsigned) fftSize << 2) | (uint64_t)(unsigned) windowType;
        std::shared_ptr<std::vector<float>> w;
        {
            const std::lock_guard<std::mutex> lk (wc.mutex);
            auto it = wc.windows.find (key);
            if (it != wc.windows.end())
                return it->second.get();
        }

        w = std::make_shared<std::vector<float>> ((size_t) fftSize, 1.0f);
        const double pi = juce::MathConstants<double>::pi;
        for (int i = 0; i < fftSize; ++i)
        {
            const double denom = (double) (fftSize - 1);
            if (windowType == 1)      // hann: tapers to zero at both ends
                (*w)[(size_t) i] = 0.5f * (1.0f - (float) std::cos (2.0 * pi * (double) i / denom));
            else if (windowType == 2) // hamming
                (*w)[(size_t) i] = 0.54f - 0.46f * (float) std::cos (2.0 * pi * (double) i / denom);
        }

        {
            const std::lock_guard<std::mutex> lk (wc.mutex);
            auto it = wc.windows.find (key);
            if (it == wc.windows.end())
                it = wc.windows.emplace (key, w).first;
            // Always return the pointer of the vector actually stored in the
            // cache: if another thread inserted a window for this key first,
            // our local `w` would be destroyed on return and its data()
            // pointer would dangle.
            return it->second.get();
        }
    }
} // namespace

SpectralEngine::SpectralEngine()
{
    pool_ = std::make_unique<juce::ThreadPool>(); // default: one thread per CPU
}

SpectralEngine::~SpectralEngine()
{
    clearCache();
}

void SpectralEngine::setColourMap (ColourMap m)
{
    colourMap_ = m;
    lut_ = buildColourLUT (m);
}

// ---------------------------------------------------------------------------
// Frame-range computation: fills tile->mag for frames [frameStart, frameStart+frameCount).
// Called from pool jobs on disjoint ranges of the same tile (no locking needed).
// ---------------------------------------------------------------------------
void SpectralEngine::computeFrames (SpectralTile& tile, const AudioEngine& engine,
                                    int channel, int64_t tileStartSample,
                                    int frameStart, int frameCount,
                                    int fftSize, int hopSize, int windowType,
                                    int64_t windowOffset) const
{
    if (frameCount <= 0 || frameStart >= tile.numFrames)
        return;

    const int numBins = tile.numBins;
    const int64_t totalSamples = engine.numSamples();

    // Samples needed by this chunk: frames [fs, fs+fc) cover
    // [anchor + fs*hop, anchor + (fs+fc-1)*hop + fftSize), anchor =
    // tileStart + windowOffset. With centred frames (negative offset) the
    // first windows reach before sample 0: that part reads as zeros.
    const int64_t s0 = tileStartSample + windowOffset + (int64_t) frameStart * hopSize;
    if (s0 >= totalSamples)
        return; // remaining frames stay at -300 dB (silence)

    const int64_t n = std::min ((int64_t) (frameCount - 1) * hopSize + fftSize,
                               totalSamples - s0);
    if (n <= 0)
        return;

    std::vector<float> samples ((size_t) n, 0.0f);
    const int64_t lead = std::min (n, std::max ((int64_t) 0, -s0));   // zeros before sample 0
    if (n > lead)
        engine.readSamples (channel, s0 + lead, n - lead, samples.data() + lead);

    const float* win = nullptr;
    if (windowType != 0)
        win = getWindow (fftSize, windowType)->data();

    // Thread-local FFT buffer (2*N floats), reused across frames. The
    // thread_local is initialised once per thread, so grow it if a larger
    // FFT size arrives later.
    thread_local std::vector<float> fftBuf;
    const size_t need = (size_t) fftSize * 2;
    if (fftBuf.size() < need)
        fftBuf.resize (need);
    float* buf = fftBuf.data();

    const int order = (int) std::log2 ((double) fftSize);

    // Amplitude calibration: divide by the window sum (coherent gain * N) and
    // apply the one-sided factor 2, then express the level as RMS (/sqrt 2) so
    // a sine of peak amplitude A reads 20*log10(A/sqrt(2)) on its bin - a
    // full-scale sine reads -3.01 dB, i.e. 90.97 dB-SPL with a 1 Pa full scale,
    // the same convention as the FFT panel's 1/3-octave dB-SPL mode. DC has no
    // mirrored negative-frequency half, so it takes no one-sided factor.
    double winSum = (double) fftSize;
    if (win != nullptr)
    {
        winSum = 0.0;
        for (int i = 0; i < fftSize; ++i)
            winSum += (double) win[i];
    }
    const double scaleAc = std::sqrt (2.0) / std::max (1e-30, winSum);
    const double scaleDc = 1.0 / std::max (1e-30, winSum);

    for (int f = frameStart; f < frameStart + frameCount && f < tile.numFrames; ++f)
    {
        const int64_t off = (int64_t) (f - frameStart) * hopSize;
        if (win != nullptr)
            for (int i = 0; i < fftSize; ++i)
                buf[i] = (off + i < n) ? samples[(size_t) (off + i)] * win[i] : 0.0f;
        else
            for (int i = 0; i < fftSize; ++i)
                buf[i] = (off + i < n) ? samples[(size_t) (off + i)] : 0.0f;


        scfft::realForward (buf, order);

        float* row = tile.mag.data() + (size_t) f * numBins;
        for (int b = 0; b < numBins; ++b)
        {
            const float re = buf[b * 2], im = buf[b * 2 + 1];
            const double m = std::sqrt ((double) re * re + (double) im * im)
                           * (b == 0 ? scaleDc : scaleAc);
            row[(size_t) b] = (float) (20.0 * std::log10 (std::max (1e-12, m)));
        }
    }
}

// ---- Whole-tile computation (public API kept for tests/diagnostics) ----
std::shared_ptr<SpectralTile> SpectralEngine::computeTile (
    const AudioEngine& engine, int channel,
    int64_t startSample, int64_t numSamples,
    int fftSize, int hopSize, int windowType) const
{
    auto tile = std::make_shared<SpectralTile>();
    tile->startSample = startSample;
    tile->numSamples = numSamples;
    tile->fftSize = fftSize;
    tile->hopSize = hopSize;
    tile->numBins = fftSize / 2;
    tile->numFrames = (int) ((numSamples + hopSize - 1) / hopSize);
    if (tile->numFrames <= 0)
        tile->numFrames = 1;
    tile->mag.assign ((size_t) tile->numBins * tile->numFrames, -300.0f);

    computeFrames (*tile, engine, channel, startSample, 0, tile->numFrames,
                   fftSize, hopSize, windowType, 0);
    return tile;
}

void SpectralEngine::noteContentLocked (const AudioEngine& engine, int64_t version, int64_t total)
{
    // A content change that did not grow the document is an in-place
    // overwrite (loop-mode recording): no range is reported, so every tile
    // computed before it is stale. Growth alone (appendSamples) only affects
    // tiles that were cut short by the old end of data (see tileStaleLocked).
    if (seenEngine_ == &engine && version != seenVersion_ && total <= seenSamples_)
        overwriteVersion_ = version;
    seenEngine_ = &engine;
    seenVersion_ = version;
    seenSamples_ = total;
}

bool SpectralEngine::tileStaleLocked (const SpectralTile& t, const AudioEngine& engine,
                                      int64_t version, int64_t total) const
{
    if (t.source != &engine)
        return true;
    if (t.contentVersion == version)
        return false;
    if (t.contentVersion < overwriteVersion_)
        return true;   // samples were overwritten in place since this tile was computed
    // Appended since: the tile is stale if the samples its frames read
    // (windows centred on [start, start + (kFramesPerTile-1)*hop], so up to
    // that + fftSize/2) were not all available when it was computed but
    // more are now.
    const int64_t need = std::min (total, t.startSample
                                          + (int64_t) (kFramesPerTile - 1) * t.hopSize + t.fftSize / 2);
    return t.availSamples < need;
}

void SpectralEngine::evictLocked()
{
    while (!lru_.empty() && (cache_.size() > maxTiles_ || cacheBytes_ > maxCacheBytes_))
    {
        const TileKey victim = lru_.front();
        lru_.pop_front();
        auto it = cache_.find (victim);
        if (it != cache_.end())
        {
            cacheBytes_ -= std::min (cacheBytes_, it->second->mag.size() * sizeof (float));
            cache_.erase (it);
        }
    }
}

std::vector<std::shared_ptr<SpectralTile>>
SpectralEngine::getTiles (const AudioEngine& engine, int channel,
                          int64_t startSample, int64_t endSample, int level,
                          const Settings& s,
                          const std::atomic<bool>* cancel)
{
    std::vector<std::shared_ptr<SpectralTile>> out;

    if (cancel != nullptr && cancel->load())
        return out;   // cancelled before any work: nothing to cache

    const int hop = effectiveHop (level, s);
    const int64_t tileSamples = (int64_t) kFramesPerTile * hop;

    const int64_t firstTile = startSample / tileSamples;
    const int64_t lastTile = std::max (firstTile, (endSample - 1) / tileSamples);

    // Snapshot the content state once: tiles computed below are stamped with
    // it, so data appended or overwritten meanwhile makes them stale later.
    const int64_t version = engine.contentVersion();
    const int64_t total = engine.numSamples();

    // Collect cache hits and misses
    struct Job { int64_t tileIndex; std::shared_ptr<SpectralTile> result; };
    std::vector<Job> jobs;

    {
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        noteContentLocked (engine, version, total);
        for (int64_t t = firstTile; t <= lastTile; ++t)
        {
            TileKey key{ t, channel, level, s.fftSize, s.hopSize, s.windowType };
            auto it = cache_.find (key);
            if (it != cache_.end() && !tileStaleLocked (*it->second, engine, version, total))
            {
                lru_.erase (std::find (lru_.begin(), lru_.end(), key));
                lru_.push_back (key);
                jobs.push_back ({ t, it->second });
            }
            else
            {
                jobs.push_back ({ t, nullptr });   // missing or stale: recompute
            }
        }
    }

    // Build fine-grained work items: each missing tile is split into chunks of
    // kFramesPerTile / 4 frames so a single tile still saturates all cores.
    constexpr int kChunksPerTile = 4;
    const int chunkFrames = kFramesPerTile / kChunksPerTile;

    struct Chunk { size_t missIdx; int frameStart; int frameCount; };

    // State shared with the pool jobs. Held by shared_ptr so a job that only
    // starts after this call returned finds no chunk left and exits without
    // touching anything else; the caller waits until every chunk is done.
    struct Work
    {
        std::vector<Chunk> chunks;
        std::vector<std::shared_ptr<SpectralTile>> missTiles; // indexed by missIdx
        std::atomic<int> next{ 0 };
        std::atomic<int> done{ 0 };
        juce::WaitableEvent finished;
    };
    auto w = std::make_shared<Work>();
    std::vector<int64_t> missTileIndices;                 // parallel to missTiles
    std::vector<size_t> missJobIndices;                   // job index for each miss

    for (size_t ji = 0; ji < jobs.size(); ++ji)
    {
        auto& j = jobs[ji];
        if (j.result)
            continue;
        const int64_t t = j.tileIndex;
        const int64_t tileStart = t * tileSamples;
        const int64_t n = std::min (tileSamples, total - tileStart);

        auto tile = std::make_shared<SpectralTile>();
        tile->startSample = tileStart;
        tile->numSamples = n;
        tile->fftSize = s.fftSize;
        tile->hopSize = hop;
        tile->numBins = s.fftSize / 2;
        tile->numFrames = (int) ((n + hop - 1) / hop);
        if (tile->numFrames <= 0)
            tile->numFrames = 1;
        tile->mag.assign ((size_t) tile->numBins * tile->numFrames, -300.0f);
        tile->source = &engine;
        tile->contentVersion = version;
        tile->availSamples = total;

        const size_t mi = w->missTiles.size();
        w->missTiles.push_back (std::move (tile));
        missTileIndices.push_back (t);
        missJobIndices.push_back (ji);

        for (int c = 0; c < kChunksPerTile; ++c)
        {
            const int fs = c * chunkFrames;
            if (fs >= w->missTiles[mi]->numFrames)
                break;
            w->chunks.push_back ({ mi, fs, std::min (chunkFrames, w->missTiles[mi]->numFrames - fs) });
        }
    }

    // Diagnostic log: cache hits, and the time this call spends computing
    // its misses (lock-free counters, aggregated per minute).
    const double diagT0 = sc::diag::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    if (diagT0 > 0.0 && w->chunks.empty())
        sc::diag::recordTiles (0.0, 0, (int) jobs.size(), s.fftSize);

    // Compute all chunks in parallel (disjoint frame ranges -> no data locks).
    if (!w->chunks.empty())
    {
        const int fftSize = s.fftSize, windowType = s.windowType;
        const AudioEngine* eng = &engine;
        // engine / this / cancel are only touched after a chunk was claimed,
        // i.e. while the caller is still waiting below.
        auto work = [this, w, eng, channel, fftSize, hop, windowType, cancel]
        {
            const int numChunks = (int) w->chunks.size();
            for (;;)
            {
                const int i = w->next.fetch_add (1);
                if (i >= numChunks)
                    break;
                if (cancel == nullptr || !cancel->load())
                {
                    const Chunk& c = w->chunks[(size_t) i];
                    computeFrames (*w->missTiles[c.missIdx], *eng, channel,
                                   w->missTiles[c.missIdx]->startSample,
                                   c.frameStart, c.frameCount,
                                   fftSize, hop, windowType, -(int64_t) (fftSize / 2));
                }
                if (w->done.fetch_add (1) + 1 == numChunks)
                    w->finished.signal();
            }
        };

        // Wait only for THIS call's chunks (never removeAllJobs, which would
        // steal or drop the jobs of a concurrent getTiles on the same pool).
        // The calling thread works too, so progress never depends on pool
        // threads busy with other callers.
        const int nJobs = std::min ((int) w->chunks.size(), pool_->getNumThreads());
        for (int i = 1; i < nJobs; ++i)
            pool_->addJob ([work] { work(); });   // fresh lambda rvalue per job
        work();
        if (w->done.load() < (int) w->chunks.size())
            w->finished.wait (-1);
        if (diagT0 > 0.0)
            sc::diag::recordTiles (juce::Time::getMillisecondCounterHiRes() - diagT0, (int) w->missTiles.size(),
                                   (int) (jobs.size() - w->missTiles.size()), s.fftSize);

        // Publish completed tiles into the cache and link them to their jobs.
        // If cancelled while computing, do NOT publish: the tiles may be
        // partially filled (-300 dB sentinels) and would poison the cache.
        if (cancel != nullptr && cancel->load())
            return out;
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        for (size_t mi = 0; mi < w->missTiles.size(); ++mi)
        {
            const auto& tile = w->missTiles[mi];
            jobs[missJobIndices[mi]].result = tile;
            TileKey key{ missTileIndices[mi], channel, level, s.fftSize, s.hopSize, s.windowType };
            auto it = cache_.find (key);
            if (it != cache_.end())
            {
                // A concurrent call may already have published a fresher tile.
                if (it->second->source == tile->source
                    && it->second->contentVersion > tile->contentVersion)
                    continue;
                cacheBytes_ -= std::min (cacheBytes_, it->second->mag.size() * sizeof (float));
                it->second = tile;
                lru_.erase (std::find (lru_.begin(), lru_.end(), key));
            }
            else
            {
                cache_.emplace (key, tile);
            }
            cacheBytes_ += tile->mag.size() * sizeof (float);
            lru_.push_back (key);
        }
    }

    // LRU eviction (tiles already in `jobs` stay alive for this caller)
    {
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        evictLocked();
    }

    for (auto& j : jobs)
        if (j.result)
            out.push_back (j.result);
    return out;
}

float SpectralEngine::probeMagnitude (const AudioEngine& engine, int channel,
                                      double tSec, double freqHz, const Settings& s,
                                      int level)
{
    if (!engine.isFileLoaded() || channel < 0 || channel >= engine.numChannels())
        return -300.0f;

    const double rate = engine.sampleRate();
    if (s.fftMultiRes)
    {
        // Same blend as the picture: the readout matches what is drawn.
        const auto sizes = multiResFftSizes (rate);
        const auto w = multiResWeights (freqHz, rate);
        double v = 0.0, best = -300.0;
        for (int k = 0; k < 3; ++k)
        {
            if (!(w[(size_t) k] > 0.0)) continue;
            Settings sk = s;
            sk.fftMultiRes = false;
            sk.fftSize = sizes[(size_t) k];
            const double pk = probeMagnitude (engine, channel, tSec, freqHz, sk, level);
            if (pk <= -299.0) return -300.0f;   // beyond the audio
            v += w[(size_t) k] * pk;
            best = std::max (best, pk);
        }
        return (float) (best > -299.0 ? v : -300.0);
    }

    const int64_t pos = std::clamp ((int64_t) std::llround (tSec * rate),
                                    (int64_t) 0, engine.numSamples() - 1);
    auto tiles = getTiles (engine, channel, pos, pos + 1, level, s);
    if (tiles.empty()) return -300.0f;

    const int hop = effectiveHop (level, s);
    const int bin = (int) std::clamp ((double) std::llround (freqHz * s.fftSize / rate),
                                      0.0, (double) (tiles.front()->numBins - 1));

    float best = -300.0f;
    for (const auto& tile : tiles)
    {
        // Tile frames are centred on startSample + f*hop: probe the two
        // frames whose centres straddle `pos` (same convention as renderToPixels).
        const double frameF = ((double) pos - (double) tile->startSample) / hop;
        for (int f = (int) std::floor (frameF); f <= (int) std::ceil (frameF); ++f)
            best = std::max (best, tile->get (f, bin));
    }
    return best;
}

namespace
{
// Frequency of pixel row y (row 0 = top = highest frequency): the mapping
// shared by renderToDb() and the multi-resolution blend.
double rowFrequency (int y, int height, const Settings& s, double rate)
{
    const double rowT = 1.0 - ((double) y + 0.5) / height;
    if (s.logFrequency)
    {
        const double logMin = std::log ((double) std::max (1.0, (double) s.minFrequency));
        const double logMax = std::log ((double) std::min (rate * 0.5, (double) s.maxFrequency));
        return std::exp (logMin + rowT * (logMax - logMin));
    }
    return (double) s.minFrequency + rowT * ((double) s.maxFrequency - (double) s.minFrequency);
}
} // namespace

std::array<int, 3> SpectralEngine::multiResFftSizes (double sampleRate)
{
    // 512 samples at 48 kHz = 10.7 ms; the nearest power of two in log terms.
    const double target = std::max (1.0, sampleRate) * 512.0 / 48000.0;
    const int order = std::clamp ((int) std::lround (std::log2 (target)), 7, 12);   // 128..4096
    const int n = 1 << order;
    return { n, n * 4, n * 16 };
}

std::array<double, 3> SpectralEngine::multiResWeights (double freqHz, double sampleRate)
{
    const auto n = multiResFftSizes (sampleRate);
    // Above 20 * rate / M a window of M samples separates partials 5% apart.
    const double fLongToMid  = 20.0 * sampleRate / n[1];
    const double fMidToShort = 20.0 * sampleRate / n[0];   // 4x higher: the ramps never overlap
    auto above = [freqHz] (double fc)
    {   // 0 below fc/sqrt(2), 1 above fc*sqrt(2), linear in log2(f) between
        if (!(freqHz > 0.0)) return 0.0;
        return std::clamp (std::log2 (freqHz / fc) + 0.5, 0.0, 1.0);
    };
    const double upper = above (fMidToShort);
    const double midUp = above (fLongToMid);
    return { upper, midUp - upper, 1.0 - midUp };
}

bool SpectralEngine::renderToPixels (const AudioEngine& engine, const Settings& s,
                                     int width, int height,
                                     double t0, double t1,
                                     const std::vector<int>& channels,
                                     std::vector<uint8_t>& rgba,
                                     const std::atomic<bool>* cancel)
{
    rgba.assign ((size_t) std::max (0, width) * (size_t) std::max (0, height) * 4, 0);
    if (!engine.isFileLoaded() || width <= 0 || height <= 0 || channels.empty())
        return true;

    std::vector<float> db;
    if (!renderToDb (engine, s, width, height, t0, t1, channels, db, cancel))
        return false;

    // dB -> display value: normalise into the [dbMinDb, dbMaxDb] window,
    // then apply gain/bias.
    const double range = std::max (1e-6, (double) s.dbMaxDb - (double) s.dbMinDb);
    for (size_t i = 0; i < db.size(); ++i)
    {
        const double norm = (((double) db[i] - (double) s.dbMinDb) / range) * (double) s.gain + (double) s.bias;
        const int idx = (int) std::clamp (norm * 255.0, 0.0, 255.0);
        const size_t o = i * 4;
        rgba[o + 0] = lut_[(size_t) idx * 4 + 0];
        rgba[o + 1] = lut_[(size_t) idx * 4 + 1];
        rgba[o + 2] = lut_[(size_t) idx * 4 + 2];
        rgba[o + 3] = 255;
    }
    return true;
}

bool SpectralEngine::renderToDb (const AudioEngine& engine, const Settings& s,
                                 int width, int height, double t0, double t1,
                                 const std::vector<int>& channels, std::vector<float>& db,
                                 const std::atomic<bool>* cancel)
{
    if (!s.fftMultiRes || !engine.isFileLoaded() || width <= 0 || height <= 0 || channels.empty())
        return renderSingleSizeDb (engine, s, width, height, t0, t1, channels, db, cancel);

    {
        // Multi-resolution: one dB picture per size, blended row by row. A
        // size with no share anywhere in the visible frequency range (e.g. the
        // long window when zoomed into the treble) is not computed at all.
        const double rate = engine.sampleRate();
        const auto sizes = multiResFftSizes (rate);
        // Each size only renders the rows where it has a share: the weights
        // follow the frequency monotonically, so those rows are one band.
        std::vector<std::array<double, 3>> rowW ((size_t) height);
        std::array<int, 3> yBegin { height, height, height }, yEnd { 0, 0, 0 };
        for (int y = 0; y < height; ++y)
        {
            rowW[(size_t) y] = multiResWeights (rowFrequency (y, height, s, rate), rate);
            for (size_t k = 0; k < 3; ++k)
                if (rowW[(size_t) y][k] > 0.0)
                {
                    yBegin[k] = std::min (yBegin[k], y);
                    yEnd[k] = std::max (yEnd[k], y + 1);
                }
        }

        std::array<std::vector<float>, 3> parts;
        for (size_t k = 0; k < 3; ++k)
        {
            if (yBegin[k] >= yEnd[k]) continue;   // no share in this view
            Settings sk = s;
            sk.fftMultiRes = false;
            sk.fftSize = sizes[k];
            if (!renderSingleSizeDb (engine, sk, width, height, t0, t1, channels, parts[k], cancel,
                                     yBegin[k], yEnd[k]))
                return false;
        }

        // Blend in dB. Values below the dB window are lifted to just under it
        // first: a -300 "no data" or a very deep minimum of one size must not
        // drag a blend down far below what either size shows on screen.
        const float floorDb = s.dbMinDb - 1.0f;
        db.assign ((size_t) width * height, -300.0f);
        for (int y = 0; y < height; ++y)
        {
            const auto& w = rowW[(size_t) y];
            for (int x = 0; x < width; ++x)
            {
                const size_t i = (size_t) y * width + x;
                double v = 0.0;
                bool any = false;
                for (int k = 0; k < 3; ++k)
                    if (w[(size_t) k] > 0.0)
                    {
                        const float pk = parts[(size_t) k][i];
                        any = any || pk > -299.0f;
                        v += w[(size_t) k] * (double) std::max (pk, floorDb);
                    }
                if (any) db[i] = (float) v;
            }
        }
    }

    return true;
}

bool SpectralEngine::renderSingleSizeDb (const AudioEngine& engine, const Settings& s,
                                         int width, int height, double t0, double t1,
                                         const std::vector<int>& channels, std::vector<float>& db,
                                         const std::atomic<bool>* cancel, int yBegin, int yEnd)
{
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    db.assign ((size_t) std::max (0, width) * (size_t) std::max (0, height), -300.0f);
    if (!engine.isFileLoaded() || width <= 0 || height <= 0 || channels.empty())
        return true;

    const double rate = engine.sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (t0 * rate));
    const int64_t s1 = std::min (engine.numSamples(), (int64_t) (t1 * rate));
    if (s1 <= s0)
        return true;

    // Choose level so one frame ~ one pixel column
    const double samplesPerPixel = (double) (s1 - s0) / width;
    const int level = pickMipLevel (samplesPerPixel, s.hopSize);
    const int hop = effectiveHop (level, s);

    // Gather tiles per channel
    std::vector<std::vector<std::shared_ptr<SpectralTile>>> perCh;
    for (int ch : channels)
    {
        auto ts = getTiles (engine, ch, s0, s1, level, s, cancel);
        if (cancelled())
            return false;   // stop before sampling; no partial tiles were cached
        perCh.push_back (std::move (ts));
    }


    const int numBins = s.fftSize / 2;

    // Bilinear sampling across the concatenated tile frames of each channel.
    // `frame` is a global frame coordinate relative to firstTileStart. The
    // interpolation crosses tile boundaries seamlessly (a tile's "next frame"
    // is the following tile's first frame, since tiles are contiguous in
    // sample space), so no dark seam appears at tile edges. Out-of-range
    // frames/bins clamp instead of reading -300 dB sentinels.
    auto sampleMag = [] (const std::vector<std::shared_ptr<SpectralTile>>& tiles,
                          double frame, double bin) -> double
    {
        double acc = 0.0;
        for (size_t ti = 0; ti < tiles.size(); ++ti)
        {
            const auto& t = tiles[ti];
            if (frame >= acc && frame <= acc + (double) t->numFrames)
            {
                const double lf = std::clamp (frame - acc, 0.0, (double) t->numFrames);
                int f0 = (int) std::floor (lf);
                if (f0 >= t->numFrames) f0 = t->numFrames - 1;
                const double fy = lf - (double) f0; // == 1.0 exactly at a tile boundary

                const int b0 = std::clamp ((int) std::floor (bin), 0, t->numBins - 1);
                const double bx = std::clamp (bin - (double) b0, 0.0, 1.0);
                const int b1 = std::min (b0 + 1, t->numBins - 1);

                const double v00 = t->get (f0, b0), v01 = t->get (f0, b1);

                // One frame ahead: next frame of this tile, else frame 0 of the
                // following tile, else clamp to this tile's last frame.
                double v10, v11;
                if (f0 + 1 < t->numFrames)
                    v10 = t->get (f0 + 1, b0), v11 = t->get (f0 + 1, b1);
                else if (ti + 1 < tiles.size())
                    v10 = tiles[ti + 1]->get (0, b0), v11 = tiles[ti + 1]->get (0, b1);
                else
                    v10 = v00, v11 = v01;

                const double top = v00 + (v01 - v00) * bx;
                const double bot = v10 + (v11 - v10) * bx;
                return top + (bot - top) * fy;
            }
            acc += t->numFrames;
        }
        return -300.0; // beyond the data: render as silence (dark), never a bright block
    };

    const int64_t firstTileStart = (s0 / ((int64_t) kFramesPerTile * hop)) * ((int64_t) kFramesPerTile * hop);

    const int yFrom = std::clamp (yBegin, 0, height);
    const int yTo = yEnd < 0 ? height : std::clamp (yEnd, yFrom, height);
    for (int y = yFrom; y < yTo; ++y)
    {
        if (cancelled())
            return false;

        // Frequency for this row (row 0 = top = max frequency)
        const double freq = rowFrequency (y, height, s, rate);

        // Bin b is centred on b * rate / fftSize (same mapping as probeMagnitude).
        const double binF = freq * (double) s.fftSize / rate;
        const double bin = std::clamp (binF, -0.5, (double) numBins - 0.5);

        for (int x = 0; x < width; ++x)
        {
            const int64_t sample = s0 + (int64_t) (((double) x + 0.5) / width * (s1 - s0));
            // Tile frames are centred on their analysis window: a pixel at
            // time t shows the spectrum of the window centred on t, so
            // transients line up with the waveform instead of appearing early,
            // and the first N/2 samples (window zero-padded before the start)
            // are covered too.
            const double frame = (double) (sample - firstTileStart) / hop;

            double v = -300.0;
            for (auto& tiles : perCh)
                v = std::max (v, sampleMag (tiles, frame, bin));
            db[(size_t) y * width + x] = (float) v;
        }
    }
    return true;
}

bool SpectralEngine::renderToPng (const AudioEngine& engine, const Settings& s,
                                  const juce::File& audioFile, const juce::File& pngFile,
                                  int width, int height, double t0, double t1,
                                  const std::atomic<bool>* cancel)
{
    if (!engine.isFileLoaded())
        return false;

    if (t0 < 0) t0 = 0;
    if (t1 <= t0) t1 = engine.duration();

    std::vector<uint8_t> rgba;
    std::vector<int> chans;
    for (int c = 0; c < engine.numChannels(); ++c)
        chans.push_back (c);

    if (!renderToPixels (engine, s, width, height, t0, t1, chans, rgba, cancel))
        return false;   // cancelled: no PNG is written
    return PngWriter::writePng (pngFile, width, height, rgba);
}

void SpectralEngine::clearCache()
{
    const std::lock_guard<std::mutex> lock (cacheMutex_);
    cache_.clear();
    lru_.clear();
    cacheBytes_ = 0;
    seenEngine_ = nullptr;
    seenVersion_ = seenSamples_ = overwriteVersion_ = -1;
}

size_t SpectralEngine::cacheSize() const
{
    const std::lock_guard<std::mutex> lock (cacheMutex_);
    return cache_.size();
}

size_t SpectralEngine::cacheBytes() const
{
    const std::lock_guard<std::mutex> lock (cacheMutex_);
    return cacheBytes_;
}

void SpectralEngine::setMaxCacheBytes (size_t bytes)
{
    const std::lock_guard<std::mutex> lock (cacheMutex_);
    maxCacheBytes_ = bytes;
    evictLocked();
}
