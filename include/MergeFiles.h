// SpectroCraft - merging several files into one document (Shift + drag & drop)
#pragma once
#include <JuceHeader.h>
#include "MarkerManager.h"

#include <atomic>
#include <functional>
#include <vector>

namespace sc {

/** How dropped files are combined with each other (and, for current-file
    merges, with the active document's existing content). */
enum class MergeMode
{
    Sequence,   // one after another along the time axis
    Parallel    // side by side on separate channels
};

/** One fully-read source file. `data` is at `rate` (the merge target rate once
    resampling has been applied; native rate before that). */
struct MergeSource
{
    juce::String name;                    // original file name (used for markers/UI)
    double rate = 0.0;
    int channels = 0;
    int64_t lengthSamples = 0;
    std::vector<std::vector<float>> data; // one vector per channel
    std::vector<Marker> markers;          // original marker times, in seconds
};

/** Layout of the merged result. `segs[i]` describes where source i lands:
    sequence mode -> startSample offset (channelOffset is 0);
    parallel mode -> first output channel (startSample is 0). */
struct MergeLayout
{
    double targetRate = 0.0;
    int outChannels = 0;
    int64_t outLengthSamples = 0;

    struct Seg { int64_t startSample = 0; int channelOffset = 0; };
    std::vector<Seg> segs;                // one per source, in order
};

/** Result of merging a set of files (used for "into a new file" merges). */
struct MergeResult
{
    double rate = 0.0;
    int channels = 0;
    std::vector<std::vector<float>> data; // one vector per channel, all same length
    std::vector<Marker> markers;          // merged marker list (fresh sequential ids)
};

/** Compute the layout for sources that already share one sample rate. */
MergeLayout computeMergeLayout (MergeMode mode, const std::vector<MergeSource>& srcs);

/** Fill `out` with the merged audio per the layout (zero-initialised). */
void buildMergedAudio (MergeMode mode, const MergeLayout& lay,
                       const std::vector<MergeSource>& srcs,
                       juce::AudioBuffer<float>& out);

/** Build the merged marker list: each source's markers shifted by its segment
    start in sequence mode; plus one region marker named after the original file
    around every dropped source (sources from index `firstDropped` onward) in
    sequence mode. All ids are renumbered sequentially. */
std::vector<Marker> buildMergedMarkers (MergeMode mode, const std::vector<Marker>* existing,
                                        const MergeLayout& lay, const std::vector<MergeSource>& srcs,
                                        int firstDropped);

/** Read one file fully into `out` at its native rate plus its markers (same
    detection chain as FileDoc::load). If targetRate > 0 and differs from the
    file's rate the audio is resampled to it. onProgress receives 0..1 for this
    file; cancel, when given, aborts with false. */
bool readMergeSource (const juce::File& f, double targetRate, MergeSource& out,
                      std::function<void(float)> onProgress = nullptr,
                      const std::atomic<bool>* cancel = nullptr);

/** Resample a source in place to `targetRate` (no-op when already there). */
bool resampleMergeSource (MergeSource& s, double targetRate,
                          const std::atomic<bool>* cancel = nullptr);

/** Read + resample every file in `files` to the highest rate involved and merge
    them per `mode`. Pure data work - no documents are touched.
    (This vendored JUCE build has no juce::FileArray, so a plain vector is used.) */
bool mergeFilesToResult (const std::vector<juce::File>& files, MergeMode mode, MergeResult& out,
                         std::function<void(float)> onProgress = nullptr,
                         const std::atomic<bool>* cancel = nullptr,
                         std::string* errOut = nullptr);

} // namespace sc
