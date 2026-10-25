// SpectroCraft - merging several files into one document (Shift + drag & drop)
#include "MergeFiles.h"
#include "FileMeta.h"
#include "Resampler.h"
#include "ScttChunk.h"
#include "VideoAudio.h"

namespace sc {

namespace
{
    // Load a source file's markers with the same detection chain as
    // FileDoc::load: unified .meta.json > legacy sidecar > SCMD chunk > cue/adtl.
    void loadSourceMarkers (const juce::File& f, double nativeRate, std::vector<Marker>& out)
    {
        const bool isWav = f.getFileExtension().equalsIgnoreCase (".wav");
        MarkerManager mm;

        const juce::File unified = fileMetaSidecarPath (f);
        if (unified.existsAsFile ())
        {
            MetaBundle b;
            if (parseBundle (unified.loadFileAsString (), b) && b.haveMarkers)
            { out = std::move (b.markers); return; }
        }

        if (mm.loadSidecar (f))
        { out = mm.markers(); return; }

        std::string scmdJson;
        if (isWav && readRiffChunkPayload (f, "SCMD", scmdJson))
        {
            MetaBundle b;
            if (parseBundle (juce::String::fromUTF8 (scmdJson.c_str ()), b) && b.haveMarkers)
            { out = std::move (b.markers); return; }
        }

        if (isWav && mm.loadFromWavFile (f, nativeRate))
            out = mm.markers();
    }

    bool cancelled (const std::atomic<bool>* c) { return c != nullptr && c->load (); }

    bool failErr (std::string* errOut, const juce::String& msg)
    {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    }

    // Map a 0..1 sub-progress into the band [lo, hi).
    void bandProgress (std::function<void(float)> onProgress, double lo, double hi, float p)
    {
        if (onProgress)
            onProgress ((float) (lo + (hi - lo) * std::min (1.0f, std::max (0.0f, p))));
    }

} // namespace

bool resampleMergeSource (MergeSource& s, double targetRate, const std::atomic<bool>* cancel)
{
    if (std::abs (s.rate - targetRate) <= 1e-9 * std::max (s.rate, targetRate))
        return true;

    juce::AudioBuffer<float> in (s.channels, (int) s.lengthSamples);
    for (int ch = 0; ch < s.channels; ++ch)
        std::copy (s.data[ch].begin (), s.data[ch].end (), in.getWritePointer (ch));

    juce::AudioBuffer<float> rs;
    if (!Resampler::resample (in, s.rate, targetRate, rs, nullptr, cancel))
        return false;

    s.data.clear ();
    for (int ch = 0; ch < rs.getNumChannels (); ++ch)
        s.data.push_back (std::vector<float> (rs.getReadPointer (ch),
                                              rs.getReadPointer (ch) + std::max<int64_t> (0, rs.getNumSamples ())));
    s.rate = targetRate;
    s.lengthSamples = rs.getNumSamples ();
    return true;
}

MergeLayout computeMergeLayout (MergeMode mode, const std::vector<MergeSource>& srcs)
{
    MergeLayout lay;
    if (srcs.empty ())
        return lay;

    lay.targetRate = srcs[0].rate;
    int64_t totalLen = 0, maxLen = 0;
    int maxChans = 0, totalChans = 0;
    for (const auto& s : srcs)
    {
        totalLen += s.lengthSamples;
        totalChans += s.channels;
        maxChans = std::max (maxChans, s.channels);
        maxLen = std::max (maxLen, s.lengthSamples);
    }

    lay.segs.resize (srcs.size ());
    if (mode == MergeMode::Sequence)
    {
        lay.outChannels = maxChans;
        lay.outLengthSamples = totalLen;
        int64_t off = 0;
        for (size_t i = 0; i < srcs.size (); ++i)
        {
            lay.segs[i].startSample = off;
            lay.segs[i].channelOffset = 0;
            off += srcs[i].lengthSamples;
        }
    }
    else // Parallel: side by side on separate channels, zero-padded to the max length.
    {
        lay.outChannels = totalChans;
        lay.outLengthSamples = maxLen;
        int chOff = 0;
        for (size_t i = 0; i < srcs.size (); ++i)
        {
            lay.segs[i].startSample = 0;
            lay.segs[i].channelOffset = chOff;
            chOff += srcs[i].channels;
        }
    }
    return lay;
}

void buildMergedAudio (MergeMode mode, const MergeLayout& lay, const std::vector<MergeSource>& srcs,
                       juce::AudioBuffer<float>& out)
{
    out.setSize (lay.outChannels, (int) lay.outLengthSamples);
    out.clear ();   // setSize does not zero a fresh buffer in this JUCE build
    for (size_t i = 0; i < srcs.size (); ++i)
    {
        const MergeSource& s = srcs[i];
        if (s.data.empty ()) continue;
        const int64_t len = std::min<int64_t> (s.lengthSamples, lay.outLengthSamples);
        if (mode == MergeMode::Sequence)
        {
            for (int ch = 0; ch < s.channels && ch < lay.outChannels; ++ch)
                out.copyFrom (ch, (int) lay.segs[i].startSample, s.data[ch].data(), (int) len);
        }
        else // Parallel
        {
            for (int ch = 0; ch < s.channels && lay.segs[i].channelOffset + ch < lay.outChannels; ++ch)
                out.copyFrom (lay.segs[i].channelOffset + ch, 0, s.data[ch].data(), (int) len);
        }
    }
}

std::vector<Marker> buildMergedMarkers (MergeMode mode, const std::vector<Marker>* existing,
                                        const MergeLayout& lay, const std::vector<MergeSource>& srcs,
                                        int firstDropped)
{
    std::vector<Marker> out;
    if (existing != nullptr)
        out.insert (out.end (), existing->begin (), existing->end ());

    for (size_t i = 0; i < srcs.size (); ++i)
    {
        const double shiftSecs = (mode == MergeMode::Sequence && lay.targetRate > 0.0)
                                   ? (double) lay.segs[i].startSample / lay.targetRate : 0.0;
        for (const auto& m : srcs[i].markers)
        {
            Marker nm = m;
            nm.start += shiftSecs;
            if (nm.end >= 0.0) nm.end += shiftSecs;
            out.push_back (nm);
        }

        // A region marker around each dropped file, named after its original name.
        if (mode == MergeMode::Sequence && (int) i >= firstDropped && lay.targetRate > 0.0)
        {
            Marker region;
            region.name = srcs[i].name;
            region.start = (double) lay.segs[i].startSample / lay.targetRate;
            region.end = ((double) lay.segs[i].startSample + (double) srcs[i].lengthSamples)
                         / lay.targetRate;
            out.push_back (region);
        }
    }

    int nid = 1;   // fresh sequential ids - sources may share original ids
    for (auto& m : out) m.id = nid++;
    return out;
}

bool readMergeSource (const juce::File& f, double targetRate, MergeSource& out,
                      std::function<void(float)> onProgress, const std::atomic<bool>* cancel)
{
    if (!f.existsAsFile ())
        return false;

    juce::AudioFormatManager fm;   // local: safe to use off the message thread
    fm.registerBasicFormats ();
    registerVideoFormats (fm);     // allow video containers as merge sources too
    auto* reader = fm.createReaderFor (f);
    if (reader == nullptr)
        return false;

    const unsigned int nch = std::max (1u, reader->numChannels);
    const int64_t len = reader->lengthInSamples;
    const double nativeRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;

    juce::AudioBuffer<float> buf (nch, (int) len);
    const int64_t chunk = 1ll << 20;
    for (int64_t off = 0; off < len; off += chunk)
    {
        if (cancelled (cancel)) { delete reader; return false; }
        const int cnt = (int) std::min<int64_t> (chunk, len - off);
        reader->read (&buf, (int) off, cnt, (int) off, true, true);
        if (onProgress && len > 0)
            onProgress ((float) ((double) (off + cnt) / (double) len));
    }
    delete reader;

    const double wantRate = targetRate > 0.0 ? targetRate : nativeRate;

    out.name = f.getFileName ();
    out.rate = nativeRate;
    out.channels = buf.getNumChannels ();
    out.lengthSamples = buf.getNumSamples ();
    for (int ch = 0; ch < out.channels; ++ch)
        out.data.push_back (std::vector<float> (buf.getReadPointer (ch),
                                                buf.getReadPointer (ch) + std::max<int64_t> (0, out.lengthSamples)));

    if (!resampleMergeSource (out, wantRate, cancel))
        return false;

    loadSourceMarkers (f, nativeRate, out.markers);   // seconds - valid after resample too
    return true;
}

bool mergeFilesToResult (const std::vector<juce::File>& files, MergeMode mode, MergeResult& out,
                         std::function<void(float)> onProgress, const std::atomic<bool>* cancel,
                         std::string* errOut)
{
    if (files.empty ())
        return failErr (errOut, "No files to merge.");

    // Pass 1: read every file at its native rate (+ markers). Band: 0..0.75.
    std::vector<MergeSource> srcs;
    srcs.reserve (files.size ());
    const int n = files.size ();
    for (int i = 0; i < n; ++i)
    {
        if (cancelled (cancel)) return false;
        MergeSource s;
        const double lo = 0.75 * (double) i / n, hi = 0.75 * (double) (i + 1) / n;
        auto subProgress = [&onProgress, lo, hi] (float p) { bandProgress (onProgress, lo, hi, p); };
        if (!readMergeSource (files[i], 0.0, s, subProgress, cancel))
            return failErr (errOut, "Could not read '" + files[i].getFileName () + "'.");
        srcs.push_back (std::move (s));
    }

    // Pass 2: resample everything to the highest rate involved. Band: 0.75..1.0.
    double targetRate = 0.0;
    for (const auto& s : srcs) targetRate = std::max (targetRate, s.rate);
    int rsIdx = 0;
    for (auto& s : srcs)
    {
        const double lo = 0.75 + 0.25 * (double) rsIdx / srcs.size ();
        const double hi = 0.75 + 0.25 * (double) (rsIdx + 1) / srcs.size ();
        if (!resampleMergeSource (s, targetRate, cancel))
            return failErr (errOut, "Resampling '" + s.name + "' failed.");
        bandProgress (onProgress, lo, hi, 1.0f);
        ++rsIdx;
    }

    // Pass 3: layout + audio + markers.
    const MergeLayout lay = computeMergeLayout (mode, srcs);
    juce::AudioBuffer<float> merged;
    buildMergedAudio (mode, lay, srcs, merged);

    out.rate = targetRate;
    out.channels = merged.getNumChannels ();
    out.data.clear ();
    for (int ch = 0; ch < out.channels; ++ch)
        out.data.push_back (std::vector<float> (merged.getReadPointer (ch),
                                                merged.getReadPointer (ch) + std::max<int64_t> (0, merged.getNumSamples ())));
    out.markers = buildMergedMarkers (mode, nullptr, lay, srcs, /*firstDropped=*/0);
    return true;
}

} // namespace sc
