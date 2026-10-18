/**
 * SpectroCraft - Audio Engine Implementation
 */

#include "AudioEngine.h"
#include "UndoManager.h"
#include "MarkerManager.h"
#include <algorithm>
#include <cmath>

AudioEngine::AudioEngine()
{
    fm = std::make_unique<juce::AudioFormatManager>();
    fm->registerBasicFormats();
}

AudioEngine::~AudioEngine()
{
    closeFile();
}

bool AudioEngine::loadFile (const juce::File& file, std::function<void(float)> onProgress)
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

    // Materialize the whole file only when it comfortably fits in memory (~256 MB of samples)
    const int64_t maxInMemory = 64ll * 1024 * 1024; // samples per channel
    if ((int64_t) nchans * totalSamples <= maxInMemory)
    {
        buffer = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) totalSamples);
        // Read in chunks so a progress callback can report real I/O progress
        // on large files.
        const int64_t chunk = 1ll << 20;
        for (int64_t off = 0; off < totalSamples; off += chunk)
        {
            const int cnt = (int) std::min<int64_t> (chunk, totalSamples - off);
            rawReader->read (buffer.get(), 0, cnt, (int) off, true, true);
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
    if (onProgress)
    {
        auto p = std::move (onProgress);
        rebuildMipmaps ([p] (float f) { p (0.5f + 0.5f * f); });
    }
    else
        rebuildMipmaps();
    return true;
}

void AudioEngine::closeFile()
{
    buffer.reset();
    clipboard.reset();
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
    const int64_t readStart = std::max<int64_t> (0, (int64_t) std::floor (pos));
    const int64_t readEnd   = std::min (totalSamples,
                                       (int64_t) std::ceil (pos + n * step) + 1);
    const int64_t count     = std::max<int64_t> (0, readEnd - readStart);

    const int chans = std::min (dst->getNumChannels(), nchans);
    std::vector<std::vector<float>> tmp ((size_t) chans);
    for (int ch = 0; ch < chans; ++ch)
    {
        tmp[(size_t) ch].resize ((size_t) count);
        if (!readSamples (ch, readStart, count, tmp[(size_t) ch].data()))
            tmp[(size_t) ch].assign ((size_t) count, 0.0f);
    }

    const float g = gain_.load();
    for (int i = 0; i < n; ++i)
    {
        const double p = pos + (double) i * step - (double) readStart;
        for (int ch = 0; ch < chans; ++ch)
        {
            float* d = dst->getWritePointer (ch, bufferToFill.startSample);
            if (p >= 0.0 && p < (double) count - 1.0)
            {
                // Linear interpolation between the two surrounding file samples.
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

void AudioEngine::rebuildMipmaps(std::function<void(float)> onProgress)
{
    clearMipmaps();
    if (!hasFile || totalSamples == 0)
        return;

    // Base factor chosen so the coarsest level has ~100k samples per channel
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
    while (factor <= 16384ll * baseFactor && totalSamples / factor > 100)
    {
        MipLevel lv;
        lv.factor = factor;
        const int64_t n = totalSamples / factor + 1;
        lv.min.resize (nchans);
        lv.max.resize (nchans);

        for (int ch = 0; ch < nchans; ++ch)
        {
            lv.min[ch].resize ((size_t) n);
            lv.max[ch].resize ((size_t) n);

            // Read in blocks to stay memory-bounded even for streaming files
            const int64_t block = 65536;
            std::vector<float> tmp ((size_t) block);
            for (int64_t i = 0; i < n; ++i)
            {
                float mn = 0.0f, mx = 0.0f;
                const int64_t s = i * factor;
                const int64_t e = std::min (s + factor, totalSamples);
                for (int64_t p = s; p < e; p += block)
                {
                    const int64_t cnt = std::min (block, e - p);
                    if (!readSamples (ch, p, cnt, tmp.data()))
                        continue;
                    for (int64_t q = 0; q < cnt; ++q)
                    {
                        const float v = tmp[q];
                        mn = std::min (mn, v);
                        mx = std::max (mx, v);
                    }
                }
                lv.min[ch][i] = mn;
                lv.max[ch][i] = mx;
            }
        }

        mips.push_back (std::move (lv));
        if (onProgress)
            onProgress ((float) mips.size() / (float) std::max (1, totalMipLevels));
        factor *= 2;
    }
}

void AudioEngine::clearMipmaps() { mips.clear(); baseFactor = 1; }

// ---- Editing ----

// NOTE: newData is taken by const reference and copied into the patch — callers
// may still need it afterwards. oldData is read from the buffer at call time,
// so this must be called BEFORE the region is modified in place.
void recordReplace (AudioEngine& eng, const juce::Range<int64_t>& region,
                           const std::vector<std::vector<float>>& newData,
                           const juce::String& description)
{
    Patch p;
    p.oldStart = region.getStart();
    p.oldEnd = region.getEnd();
    p.description = description;
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

void AudioEngine::copySelection (juce::Range<int64_t> samples)
{
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0)
        return;
    clipboard = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) samples.getLength());
    for (int ch = 0; ch < nchans; ++ch)
        readSamples (ch, samples.getStart(), (int) samples.getLength(),
                     clipboard->getWritePointer (ch));
}

void AudioEngine::pasteAt (int64_t samplePos)
{
    if (!clipboard || !buffer)
        return;
    samplePos = std::clamp<int64_t> (samplePos, (int64_t) 0, totalSamples);
    const int64_t pasteLen = clipboard->getNumSamples();

    std::vector<std::vector<float>> newData (nchans);
    for (int ch = 0; ch < nchans; ++ch)
    {
        newData[ch].assign (clipboard->getReadPointer (ch),
                            clipboard->getReadPointer (ch) + pasteLen);
    }

    // Insert = replace empty region [pos, pos) with newData
    recordReplace (*this, { samplePos, samplePos }, std::move (newData), "Pasted samples");

    // Apply forward patch
    Patch p; // re-apply manually since recordReplace only records
    const int64_t newTotal = totalSamples + pasteLen;
    auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
    for (int ch = 0; ch < nchans; ++ch)
    {
        nb->copyFrom (ch, 0, *buffer, ch, 0, (int) samplePos);
        nb->copyFrom (ch, (int) samplePos, *clipboard, ch, 0, (int) pasteLen);
        nb->copyFrom (ch, (int) (samplePos + pasteLen), *buffer, ch, (int) samplePos,
                      (int) (totalSamples - samplePos));
    }
    buffer = std::move (nb);
    totalSamples = newTotal;
    durationSecs = (double) totalSamples / rate;
    unsaved = true;
    rebuildMipmaps();
}

void AudioEngine::deleteSamples (juce::Range<int64_t> samples)
{
    if (!buffer) return;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return;
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
}

// Returns true if the channel is affected by `chans` (nullptr => all).
static bool channelAffected (const std::vector<int>* chans, int ch)
{
    return !chans || std::find (chans->begin(), chans->end(), ch) != chans->end();
}

void AudioEngine::silenceSamples (juce::Range<int64_t> samples, const std::vector<int>* chans)
{
    if (!buffer) return;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return;

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

void AudioEngine::fadeInSamples (juce::Range<int64_t> samples, const std::vector<int>* chans)
{
    applyPerSample (*this, samples, [] (float v, int i, int len)
    {
        return v * (float) i / (float) std::max (1, len - 1);
    }, chans, "Faded in");
}

void AudioEngine::fadeOutSamples (juce::Range<int64_t> samples, const std::vector<int>* chans)
{
    applyPerSample (*this, samples, [] (float v, int i, int len)
    {
        return v * (1.0f - (float) i / (float) std::max (1, len - 1));
    }, chans, "Faded out");
}

void AudioEngine::normalizeSamples (juce::Range<int64_t> samples, float target,
                                    const std::vector<int>* chans)
{
    if (!buffer) return;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return;

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
        return;

    const float g = target / peak;
    applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                   "Normalized");
}

void AudioEngine::gainSamples (juce::Range<int64_t> samples, float gainDb,
                               const std::vector<int>* chans)
{
    if (!buffer || gainDb == 0.0f) return;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return;

    const float g = std::pow (10.0f, gainDb / 20.0f);
    applyPerSample (*this, samples, [g] (float v, int, int) { return v * g; }, chans,
                   juce::String ("Changed gain by ")
                       + juce::String (gainDb >= 0.0f ? "+" : "")
                       + juce::String (gainDb, 2) + " dB");
}

void AudioEngine::reverseSamples (juce::Range<int64_t> samples, const std::vector<int>* chans)
{
    if (!buffer) return;
    samples = samples.getIntersectionWith ({ 0, totalSamples });
    if (samples.getLength() <= 0) return;

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
}

bool AudioEngine::applyPatch (const Patch& p, bool forward, bool rebuild)
{
    if (!buffer)
        return false;

    const int64_t oldLen = p.oldEnd - p.oldStart;
    const int64_t newLen = p.newData.empty() ? 0 : (int64_t) p.newData[0].size();

    if (forward)
    {
        const int64_t newTotal = totalSamples + p.lengthDelta;
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) p.oldStart);
            if (ch < (int) p.newData.size() && newLen > 0)
                nb->copyFrom (ch, (int) p.oldStart, p.newData[ch].data(), (int) newLen);
            nb->copyFrom (ch, (int) (p.oldStart + newLen), *buffer, ch, (int) p.oldEnd,
                          (int) (totalSamples - p.oldEnd));
        }
        buffer = std::move (nb);
        totalSamples = newTotal;
    }
    else
    {
        // Inverse: region in current buffer is [oldStart, oldEnd + lengthDelta)
        const int64_t curEnd = p.oldEnd + p.lengthDelta;
        const int64_t newTotal = totalSamples - p.lengthDelta;
        auto nb = std::make_unique<juce::AudioBuffer<float>> (nchans, (int) newTotal);
        for (int ch = 0; ch < nchans; ++ch)
        {
            nb->copyFrom (ch, 0, *buffer, ch, 0, (int) p.oldStart);
            if (ch < (int) p.oldData.size() && oldLen > 0)
                nb->copyFrom (ch, (int) p.oldStart, p.oldData[ch].data(), (int) oldLen);
            nb->copyFrom (ch, (int) (p.oldStart + oldLen), *buffer, ch, (int) curEnd,
                          (int) (totalSamples - curEnd));
        }
        buffer = std::move (nb);
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

    // Legacy DAW-compatible adtl text sub-chunk ("labl" / "note"):
    //   fourcc + u32(5+len) + u32 cueID + text [+ NUL if len even] + trailing NUL.
    // When the text length is even, one extra byte is written beyond the
    // declared size; it doubles as RIFF alignment padding (legacy DAWs do this).
    void writeAdtlTextEntry (juce::OutputStream& os, const char id[4], int cueId, const juce::String& text)
    {
        putFourCC (os, id);
        putU32 (os, (uint32_t) (5 + text.getNumBytesAsUTF8()));
        putU32 (os, (uint32_t) cueId);
        os.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
        if (text.getNumBytesAsUTF8() % 2 == 0)
            os.writeByte (0);
        os.writeByte (0);
    }

    // Total bytes of the above entry in the file, including its 8-byte header.
    size_t adtlTextEntrySize (const juce::String& text)
    {
        const int len = text.getNumBytesAsUTF8();
        return 8 + 4 + (size_t) len + ((len % 2 == 0) ? 1 : 0) + 1;
    }

    // Legacy DAW ltxt sub-chunk: fourcc + u32(20) + u32 cueID + u32 sampleLength
    // + "rgn " + country/language/dialect/codepage (all zero).
    void writeLtxtEntry (juce::OutputStream& os, int cueId, uint32_t sampleLength)
    {
        putFourCC (os, "ltxt");
        putU32 (os, 20);
        putU32 (os, (uint32_t) cueId);
        putU32 (os, sampleLength);
        putFourCC (os, "rgn ");
        putU16 (os, 0); // country
        putU16 (os, 0); // language
        putU16 (os, 0); // dialect
        putU16 (os, 0); // codepage
    }

    // Legacy DAW cue entry (24 bytes): u32 id + u32 position + "data" +
    // u32 chunkStart(0) + u32 blockStart(0) + u32 sampleOffset (= position).
    void writeCueEntry (juce::OutputStream& os, int cueId, uint32_t startSample)
    {
        putU32 (os, (uint32_t) cueId);
        putU32 (os, startSample);
        putFourCC (os, "data");
        putU32 (os, 0); // chunkStart
        putU32 (os, 0); // blockStart
        putU32 (os, startSample); // sampleOffset (legacy DAWs duplicate the position)
    }
}

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
    const std::vector<Marker>* ms = saveMarkers;
    const int numMarkers = ms ? (int) ms->size() : 0;

    auto startSampleOf = [this] (const Marker& m) -> uint32_t
    {
        return (uint32_t) std::clamp (std::llround (m.start * rate), (int64_t) 0, totalSamples);
    };
    auto regionLengthOf = [this] (const Marker& m) -> uint32_t
    {
        if (m.end <= 0.0 || m.end <= m.start)
            return 0; // point marker
        return (uint32_t) std::clamp (std::llround ((m.end - m.start) * rate), (int64_t) 0, totalSamples);
    };

    size_t listPayload = 0;
    if (numMarkers > 0)
    {
        listPayload += 4; // "adtl"
        for (const auto& m : *ms)
        {
            listPayload += 28; // ltxt sub-chunk: header(8) + data(20)
            if (!m.name.isEmpty())
                listPayload += adtlTextEntrySize (m.name);
            if (!m.comment.isEmpty())
                listPayload += adtlTextEntrySize (m.comment);
        }
    }

    const int64_t cueChunk  = numMarkers > 0 ? 8 + 4 + 24LL * numMarkers : 0;
    const int64_t listChunk = numMarkers > 0 ? 8 + (int64_t) listPayload : 0;

    const int64_t riffSize = 4            // "WAVE"
                           + 8 + 16       // fmt chunk
                           + 8 + dataSize // data chunk
                           + cueChunk
                           + listChunk;

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

    // cue chunk: one 24-byte entry per marker.
    if (numMarkers > 0 && ok)
    {
        putFourCC (*out, "cue ");
        putU32 (*out, (uint32_t) (4 + 24LL * numMarkers));
        putU32 (*out, (uint32_t) numMarkers);
        for (const auto& m : *ms)
            writeCueEntry (*out, m.id, startSampleOf (m));
    }

    // LIST chunk with the adtl sub-chunk: ltxt entries first, then labl titles,
    // then note comments.
    if (numMarkers > 0 && ok)
    {
        putFourCC (*out, "LIST");
        putU32 (*out, (uint32_t) listPayload);
        putFourCC (*out, "adtl");
        for (const auto& m : *ms)
            writeLtxtEntry (*out, m.id, regionLengthOf (m));
        for (const auto& m : *ms)
            if (!m.name.isEmpty())
                writeAdtlTextEntry (*out, "labl", m.id, m.name);
        for (const auto& m : *ms)
            if (!m.comment.isEmpty())
                writeAdtlTextEntry (*out, "note", m.id, m.comment);
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
