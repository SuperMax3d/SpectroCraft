#pragma once
// SpectroCraft - Spectral engine: tiled FFT spectrogram with LRU cache + thread pool
#include <JuceHeader.h>
#include "Settings.h"
#include "AudioEngine.h"
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <list>
#include <cstdint>

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

    // Probe the magnitude (dB, e.g. -120..0) at an exact time/frequency point.
    // Pass the mipmap level the view is currently displaying so the tiles are
    // already cached; missing tiles fall back to a synchronous compute.
    float probeMagnitude (const AudioEngine& engine, int channel, double tSec,
                          double freqHz, const Settings& s, int level);

    // Render a region to RGBA pixels (bilinear sampling, colour LUT, optional smoothing).
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

    // Exposed for unit tests / diagnostics. Returns dB magnitudes (no gain/bias/dB scaling).
    std::shared_ptr<SpectralTile> computeTile (const AudioEngine& engine, int channel,
                                               int64_t startSample, int64_t numSamples,
                                               int fftSize, int hopSize, int windowType) const;

private:
    // Fill tile->mag for frames [frameStart, frameStart+frameCount). Called from
    // pool jobs on disjoint ranges of the same tile (no locking needed).
    void computeFrames (SpectralTile& tile, const AudioEngine& engine,
                        int channel, int64_t tileStartSample,
                        int frameStart, int frameCount,
                        int fftSize, int hopSize, int windowType) const;
    ColourMap colourMap_ = ColourMap::Classic;
    std::array<uint8_t, 256 * 4> lut_ = buildColourLUT (ColourMap::Classic);

    std::unordered_map<TileKey, std::shared_ptr<SpectralTile>, TileKeyHash> cache_;
    std::list<TileKey> lru_;
    mutable std::mutex cacheMutex_;
    size_t maxTiles_ = 512;

    std::unique_ptr<juce::ThreadPool> pool_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralEngine)
};
