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
                                    int fftSize, int hopSize, int windowType) const
{
    if (frameCount <= 0 || frameStart >= tile.numFrames)
        return;

    const int numBins = tile.numBins;
    const int64_t totalSamples = engine.numSamples();

    // Samples needed by this chunk: frames [fs, fs+fc) cover
    // [tileStart + fs*hop, tileStart + (fs+fc-1)*hop + fftSize).
    const int64_t s0 = tileStartSample + (int64_t) frameStart * hopSize;
    if (s0 >= totalSamples)
        return; // remaining frames stay at -300 dB (silence)

    const int64_t n = std::min ((int64_t) (frameCount - 1) * hopSize + fftSize,
                               totalSamples - s0);
    if (n <= 0)
        return;

    std::vector<float> samples ((size_t) n);
    engine.readSamples (channel, s0, n, samples.data());

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
    const double invN = 1.0 / (double) fftSize;

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
            const double m = std::sqrt ((double) re * re + (double) im * im) * invN;
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
                   fftSize, hopSize, windowType);
    return tile;
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

    const int hop = s.hopSize << level;
    const int64_t tileSamples = (int64_t) kFramesPerTile * hop;

    const int64_t firstTile = startSample / tileSamples;
    const int64_t lastTile = std::max (firstTile, (endSample - 1) / tileSamples);

    // Collect cache hits and misses
    struct Job { int64_t tileIndex; std::shared_ptr<SpectralTile> result; };
    std::vector<Job> jobs;

    {
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        for (int64_t t = firstTile; t <= lastTile; ++t)
        {
            TileKey key{ t, channel, level, s.fftSize, s.hopSize, s.windowType };
            auto it = cache_.find (key);
            if (it != cache_.end())
            {
                lru_.erase (std::find (lru_.begin(), lru_.end(), key));
                lru_.push_back (key);
                jobs.push_back ({ t, it->second });
            }
            else
            {
                jobs.push_back ({ t, nullptr });
            }
        }
    }

    // Build fine-grained work items: each missing tile is split into chunks of
    // kFramesPerTile / 4 frames so a single tile still saturates all cores.
    constexpr int kChunksPerTile = 4;
    const int chunkFrames = kFramesPerTile / kChunksPerTile;

    struct Chunk { size_t missIdx; int frameStart; int frameCount; };
    std::vector<Chunk> chunks;
    std::vector<std::shared_ptr<SpectralTile>> missTiles; // indexed by missIdx
    std::vector<int64_t> missTileIndices;                 // parallel to missTiles
    std::vector<size_t> missJobIndices;                   // job index for each miss

    for (size_t ji = 0; ji < jobs.size(); ++ji)
    {
        auto& j = jobs[ji];
        if (j.result)
            continue;
        const int64_t t = j.tileIndex;
        const int64_t tileStart = t * tileSamples;
        const int64_t n = std::min (tileSamples, engine.numSamples() - tileStart);

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

        const size_t mi = missTiles.size();
        missTiles.push_back (std::move (tile));
        missTileIndices.push_back (t);
        missJobIndices.push_back (ji);

        for (int c = 0; c < kChunksPerTile; ++c)
        {
            const int fs = c * chunkFrames;
            if (fs >= missTiles[mi]->numFrames)
                break;
            chunks.push_back ({ mi, fs, std::min (chunkFrames, missTiles[mi]->numFrames - fs) });
        }
    }

    // Compute all chunks in parallel (disjoint frame ranges -> no data locks).
    if (!chunks.empty())
    {
        std::atomic<int> next{ 0 };
        auto work = [&]
        {
            for (;;)
            {
                const int i = next.fetch_add (1);
                if ((size_t) i >= chunks.size())
                    break;
                const Chunk& c = chunks[(size_t) i];
                computeFrames (*missTiles[c.missIdx], engine, channel,
                               missTiles[c.missIdx]->startSample,
                               c.frameStart, c.frameCount,
                               s.fftSize, hop, s.windowType);
            }
        };

        const int nJobs = std::min ((int) chunks.size(), pool_->getNumThreads());
        if (nJobs > 1)
        {
            for (int i = 0; i < nJobs; ++i)
                pool_->addJob ([&] { work(); }); // fresh lambda rvalue per job
            pool_->removeAllJobs (true, 300000);  // block until all chunk jobs finish
        }
        else
        {
            work();
        }

        // Publish completed tiles into the cache and link them to their jobs.
        // If cancelled while computing, do NOT publish: the tiles may be
        // partially filled (-300 dB sentinels) and would poison the cache.
        if (cancel != nullptr && cancel->load())
            return out;
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        for (size_t mi = 0; mi < missTiles.size(); ++mi)
        {
            TileKey key{ missTileIndices[mi], channel, level, s.fftSize, s.hopSize, s.windowType };
            cache_[key] = missTiles[mi];
            lru_.push_back (key);
            jobs[missJobIndices[mi]].result = missTiles[mi];
        }
    }

    // LRU eviction
    {
        const std::lock_guard<std::mutex> lock (cacheMutex_);
        while (cache_.size() > maxTiles_)
        {
            const TileKey victim = lru_.front();
            lru_.pop_front();
            cache_.erase (victim);
        }
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
    const int64_t pos = std::clamp ((int64_t) std::llround (tSec * rate),
                                    (int64_t) 0, engine.numSamples() - 1);
    auto tiles = getTiles (engine, channel, pos, pos + 1, level, s);
    if (tiles.empty()) return -300.0f;

    const int hop = s.hopSize << level;
    const int bin = (int) std::clamp ((double) std::llround (freqHz * s.fftSize / rate),
                                      0.0, (double) (tiles.front()->numBins - 1));

    float best = -300.0f;
    for (const auto& tile : tiles)
    {
        const double frameF = ((double) pos - (double) tile->startSample) / hop;
        for (int f = (int) std::floor (frameF); f <= (int) std::ceil (frameF); ++f)
            best = std::max (best, tile->get (f, bin));
    }
    return best;
}

bool SpectralEngine::renderToPixels (const AudioEngine& engine, const Settings& s,
                                     int width, int height,
                                     double t0, double t1,
                                     const std::vector<int>& channels,
                                     std::vector<uint8_t>& rgba,
                                     const std::atomic<bool>* cancel)
{
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    rgba.assign ((size_t) width * height * 4, 0);
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
    const int hop = s.hopSize << level;

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
    const double freqMax = rate * 0.5;
    const double logMin = std::log ((double) std::max (1.0, (double) s.minFrequency));
    const double logMax = std::log ((double) std::min (freqMax, (double) s.maxFrequency));

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

    for (int y = 0; y < height; ++y)
    {
        if (cancelled())
            return false;

        // Frequency for this row (row 0 = top = max frequency)
        const double rowT = 1.0 - ((double) y + 0.5) / height;
        double freq;
        if (s.logFrequency)
            freq = std::exp (logMin + rowT * (logMax - logMin));
        else
            freq = (double) s.minFrequency + rowT * ((double) s.maxFrequency - (double) s.minFrequency);

        const double binF = (freq / rate) * 2.0 * (numBins - 1);
        const double bin = std::clamp (binF, -0.5, (double) numBins - 0.5);

        // dB -> display value: normalise into the [dbMinDb, dbMaxDb] window,
        // then apply gain/bias.
        const double range = std::max (1e-6, (double) s.dbMaxDb - (double) s.dbMinDb);

        for (int x = 0; x < width; ++x)
        {
            const int64_t sample = s0 + (int64_t) (((double) x + 0.5) / width * (s1 - s0));
            const double frame = (double) (sample - firstTileStart) / hop;

            double v = -300.0;
            for (auto& tiles : perCh)
                v = std::max (v, sampleMag (tiles, frame, bin));

            const double norm = ((v - (double) s.dbMinDb) / range) * (double) s.gain + (double) s.bias;
            const int idx = (int) std::clamp (norm * 255.0, 0.0, 255.0);
            const size_t o = ((size_t) y * width + x) * 4;
            rgba[o + 0] = lut_[idx * 4 + 0];
            rgba[o + 1] = lut_[idx * 4 + 1];
            rgba[o + 2] = lut_[idx * 4 + 2];
            rgba[o + 3] = 255;
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
}

size_t SpectralEngine::cacheSize() const
{
    const std::lock_guard<std::mutex> lock (cacheMutex_);
    return cache_.size();
}
