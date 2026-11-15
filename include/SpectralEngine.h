#pragma once
// SpectroCraft - Spectral engine: tiled FFT spectrogram with LRU cache + thread pool
#include <JuceHeader.h>
#include "Settings.h"
#include "AudioEngine.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <list>
#include <cstdint>

// Frames of the tiles getTiles() returns are CENTRED on startSample + f*hopSize
// (standard STFT "center" convention): the analysis window of frame f spans
// [startSample + f*hop - fftSize/2, startSample + f*hop + fftSize/2), with the
// samples outside the document read as zeros. Every displayed time therefore
// has a frame centred near it, the first and last fftSize/2 samples included.
// computeTile() frames instead START at startSample + f*hop (raw data export).
class SpectralTile
{
public:
    int64_t startSample = 0;
    int64_t numSamples = 0;
    int fftSize = 2048;
    int hopSize = 512;
    int numBins = 0;
    int numFrames = 0;
    std::vector<float> mag; // [frame * numBins + bin], dB values (e.g. -120..0)

    // Validity stamp for the tile cache: the engine and its content version
    // the tile was computed from, and how many samples the engine held then
    // (a tile computed while recording may be cut short by the end of data).
    const void* source = nullptr;   // identity only, never dereferenced
    int64_t contentVersion = -1;
    int64_t availSamples = 0;

    float get (int frame, int bin) const
    {
        if (frame >= 0 && frame < numFrames && bin >= 0 && bin < numBins)
            return mag[(size_t) frame * numBins + bin];
        return -300.0f;
    }
};

struct TileKey
{
    int64_t tileIndex;
    int channel;
    int level;
    int fftSize;      // tiles depend on FFT size + base hop + window
    int hopBase;
    int windowType;
    bool operator== (const TileKey& o) const
    {
        return tileIndex == o.tileIndex && channel == o.channel && level == o.level
            && fftSize == o.fftSize && hopBase == o.hopBase && windowType == o.windowType;
    }
};

struct TileKeyHash
{
    size_t operator() (const TileKey& k) const noexcept
    {
        size_t h = (size_t) k.tileIndex;
        h = h * 31u + (size_t) k.channel;
        h = h * 31u + (size_t) k.level;
        h = h * 31u + (size_t) k.fftSize;
        h = h * 31u + (size_t) k.hopBase;
        h = h * 31u + (size_t) k.windowType;
        return h;
    }
};

class SpectralEngine
{
public:
    SpectralEngine();
    ~SpectralEngine();

    void setColourMap (ColourMap m);
    ColourMap getColourMap() const { return colourMap_; }

    static constexpr int kFramesPerTile = 256;

    // Get tiles covering [startSample, endSample) at a level (0 = base hop).
    // Missing tiles are computed in parallel via the thread pool. If cancel
    // points at an atomic set to true, returns empty without caching partials.
    std::vector<std::shared_ptr<SpectralTile>>
        getTiles (const AudioEngine& engine, int channel, int64_t startSample, int64_t endSample,
                  int level, const Settings& s,
                  const std::atomic<bool>* cancel = nullptr);

    // Pick the mipmap level so one FFT frame ~ one pixel column.
    static int pickMipLevel (double samplesPerPixel, int baseHop)
    {
        int level = 0;
        while (((double) (baseHop << (level + 1)) <= samplesPerPixel) && level < 10)
            ++level;
        return level;
    }

    // Effective frame hop at mipmap level `level`: the configured hop doubled
    // per level. Level 0 is additionally clamped to fftSize/4 when the FFT
    // window is smaller than that, so frames always overlap and no sample can
    // fall between analysis windows (short transients in such gaps would be
    // invisible). Coarser levels keep their sparse mipmap hops: beyond one
    // FFT window per pixel column sub-pixel transients cannot be resolved.
    static int effectiveHop (int level, const Settings& s)
    {
        int hop = s.hopSize << level;
        if (level == 0 && hop > s.fftSize / 4)
            hop = std::max (1, s.fftSize / 4);
        return hop;
    }

    /** Auto FFT-size mode: balances time resolution against frequency
        resolution from the current zoom levels.

        The TIME constraint caps the size: about `targetTimeSlices` FFT frames
        should fit across the visible range, and with hop ~ FFT/4 that bounds
        the largest useful window. The FREQUENCY constraint sets a floor:
        keeping about `binsPerPixel` spectral bins per pixel of the frequency
        axis needs deltaF = (maxFreq - minFreq) / (heightPixels * binsPerPixel),
        i.e. an FFT of at least sampleRate / deltaF samples.

        The result is snapped up to a power of two and clamped to 256..16384. */
    static int chooseFftSize (double visibleSeconds, double sampleRate,
                              int heightPixels, double minFreqHz, double maxFreqHz,
                              double targetTimeSlices = 80.0, double binsPerPixel = 1.25);

    /** Multi-resolution mode (Settings::fftMultiRes, issue #17): the picture
        is drawn from three FFT sizes, N, 4N and 16N with N the power of two
        closest to 10.7 ms of audio (512 / 2048 / 8192 at 48 kHz), returned
        shortest first. N is kept within 128..4096. */
    static std::array<int, 3> multiResFftSizes (double sampleRate);

    /** Share of each multi-resolution size (same order as multiResFftSizes)
        at a frequency; the three add up to 1. Each frequency takes the
        shortest window that still separates partials 5% apart: a window of
        M samples has bins rate/M apart, so it takes over above 20 * rate / M.
        Around each crossover the two sizes blend over one octave (linear in
        log frequency), so no seam shows. Low notes therefore come from the
        long window (fine frequency detail), transients in the upper range from
        the short one (sharp in time). */
    static std::array<double, 3> multiResWeights (double freqHz, double sampleRate);

    // Probe the magnitude (dB, e.g. -120..0) at an exact time/frequency point.
    // Pass the mipmap level the view is currently displaying so the tiles are
    // already cached; missing tiles fall back to a synchronous compute.
    float probeMagnitude (const AudioEngine& engine, int channel, double tSec,
                          double freqHz, const Settings& s, int level);

    /** dB magnitude of every pixel of a region (row-major, row 0 = top =
        highest frequency), before the dB window and colour map: what
        renderToPixels() colours. In multi-resolution mode (s.fftMultiRes)
        three single-size pictures are blended per row with multiResWeights();
        a size with no share in the visible frequency range is not computed.
        Pixels beyond the audio read -300. Returns false when cancelled. */
    bool renderToDb (const AudioEngine& engine, const Settings& s,
                     int width, int height, double t0, double t1,
                     const std::vector<int>& channels, std::vector<float>& db,
                     const std::atomic<bool>* cancel = nullptr);

    // Render a region to RGBA pixels (bilinear sampling, colour LUT, optional smoothing):
    // renderToDb() mapped through the dB window and the colour map.
    // Fills rgba with width*height*4 bytes. Returns false if cancel points at an
    // atomic set to true (rendering stops early; no partial tiles are cached).
    bool renderToPixels (const AudioEngine& engine, const Settings& s,
                         int width, int height,
                         double t0, double t1,
                         const std::vector<int>& channels,
                         std::vector<uint8_t>& rgba,
                         const std::atomic<bool>* cancel = nullptr);

    // Offline: render whole-file spectrogram to a PNG.
    bool renderToPng (const AudioEngine& engine, const Settings& s,
                      const juce::File& audioFile, const juce::File& pngFile,
                      int width, int height, double t0, double t1,
                      const std::atomic<bool>* cancel = nullptr);

    void clearCache();
    size_t cacheSize() const;
    /** Bytes of magnitude data held by the tile cache. */
    size_t cacheBytes() const;
    /** Memory budget of the tile cache (evicts least-recently-used tiles). */
    void setMaxCacheBytes (size_t bytes);

    // Exposed for unit tests / diagnostics. Returns dB magnitudes (no gain/bias/dB scaling).
    // Frame f's window starts at startSample + f*hopSize (not centred).
    std::shared_ptr<SpectralTile> computeTile (const AudioEngine& engine, int channel,
                                               int64_t startSample, int64_t numSamples,
                                               int fftSize, int hopSize, int windowType) const;

private:
    // renderToDb() for one FFT size (s.fftSize; fftMultiRes ignored). Only rows
    // [yBegin, yEnd) are computed (yEnd < 0 = all); the others read -300.
    bool renderSingleSizeDb (const AudioEngine& engine, const Settings& s,
                             int width, int height, double t0, double t1,
                             const std::vector<int>& channels, std::vector<float>& db,
                             const std::atomic<bool>* cancel, int yBegin = 0, int yEnd = -1);

    // Fill tile->mag for frames [frameStart, frameStart+frameCount). Frame f's
    // window starts at tileStartSample + f*hopSize + windowOffset (-fftSize/2
    // for centred frames); samples before 0 or past the end read as zeros.
    // Called from pool jobs on disjoint ranges of the same tile (no locking needed).
    void computeFrames (SpectralTile& tile, const AudioEngine& engine,
                        int channel, int64_t tileStartSample,
                        int frameStart, int frameCount,
                        int fftSize, int hopSize, int windowType,
                        int64_t windowOffset) const;
    ColourMap colourMap_ = ColourMap::Classic;
    std::array<uint8_t, 256 * 4> lut_ = buildColourLUT (ColourMap::Classic);

    // Cache validity (called with cacheMutex_ held): records an in-place
    // overwrite when the content version moved without the length growing,
    // and tells whether a cached tile must be recomputed.
    void noteContentLocked (const AudioEngine& engine, int64_t version, int64_t total);
    bool tileStaleLocked (const SpectralTile& t, const AudioEngine& engine,
                          int64_t version, int64_t total) const;
    void evictLocked();

    std::unordered_map<TileKey, std::shared_ptr<SpectralTile>, TileKeyHash> cache_;
    std::list<TileKey> lru_;
    mutable std::mutex cacheMutex_;
    size_t maxTiles_ = 512;
    // Byte budget: the tile size grows with fftSize ((fftSize/2) * 256 * 4 bytes),
    // so a tile count alone does not bound memory (512 tiles of 65536 = 16 GB).
    size_t maxCacheBytes_ = (size_t) 1 << 30;   // 1 GiB
    size_t cacheBytes_ = 0;

    const void* seenEngine_ = nullptr;   // last engine seen by getTiles (identity only)
    int64_t seenVersion_ = -1;
    int64_t seenSamples_ = -1;
    int64_t overwriteVersion_ = -1;      // tiles older than this were overwritten in place

    std::unique_ptr<juce::ThreadPool> pool_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralEngine)
};
