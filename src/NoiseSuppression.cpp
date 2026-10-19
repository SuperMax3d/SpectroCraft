// SpectroCraft — noise-suppression pipelines (Phases 1–2 of the AI plan):
// denseunet and DeepFilterNet2/3.
#include "NoiseSuppression.h"

#include "AI/DenseUnetModel.h"
#include "AI/DeepFilter.h"
#include "AI/ModelManager.h"
#include "Resampler.h"

#include <JuceHeader.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace sc {

namespace {

// Compiled models are cached per (modelId, device): the first compile takes
// 10–30 s; re-applying the effect afterwards should start instantly.
std::map<std::string, std::unique_ptr<ai::DenseUnetModel>>& modelCache()
{
    static std::map<std::string, std::unique_ptr<ai::DenseUnetModel>> cache;
    return cache;
}

std::mutex& modelCacheMutex()
{
    static std::mutex m;
    return m;
}

bool report(std::function<bool(float)>& p, float v)
{
    return !p || p (v);
}

ai::DenseUnetModel* getCachedModel(const std::string& modelId, const std::string& device,
                                   const juce::File& xmlPath, std::string* errOut)
{
    const std::string key = modelId + ":" + device;
    {
        std::lock_guard<std::mutex> lock (modelCacheMutex());
        auto it = modelCache().find (key);
        if (it != modelCache().end()) return it->second.get();
    }

    auto model = std::make_unique<ai::DenseUnetModel>();
    if (!model->load (xmlPath.getFullPathName ().toStdString (), device, errOut))
        return nullptr;

    ai::DenseUnetModel* raw = model.get();
    {
        std::lock_guard<std::mutex> lock (modelCacheMutex());
        modelCache()[key] = std::move (model);
    }
    return raw;
}

} // namespace

bool runDenseUnetPipeline(const std::string& modelId, const std::string& device,
                          double rate, int nChans,
                          std::vector<std::vector<float>>& data,
                          std::function<bool(float)> onProgress,
                          std::string* errOut,
                          const std::vector<int>* chans)
{
    auto fail = [&errOut] (const std::string& msg) -> bool
    {
        if (errOut) *errOut = msg;
        return false;
    };

    if (nChans <= 0 || data.size() != (size_t) nChans)
        return fail ("invalid channel count");
    for (int ch = 0; ch < nChans; ++ch)
        if (data[ch].empty())
            return fail ("empty selection");

    const int64_t len = data[0].size();
    for (int ch = 1; ch < nChans; ++ch)
        if (data[ch].size() != (size_t) len)
            return fail ("channel length mismatch");

    auto& mm = ai::ModelManager::instance();
    const ai::ModelSpec* spec = mm.find (modelId);
    if (!spec)
        return fail ("unknown model: " + modelId);

    // ---- Phase 1: make sure the weights are installed (progress 0..0.15).
    std::string installErr;
    const bool installed = mm.ensureInstalled (*spec,
        [&onProgress] (float f, const std::string&) { return report (onProgress, 0.15f * f); },
        &installErr);
    if (!installed)
        return fail ("model not available: " + installErr);

    // ---- Phase 2: load/compile the model (~progress 0.2).
    // The first registry file is the model's .xml (the .bin sits beside it);
    // destination filename == URL basename.
    const std::string& xmlUrl = spec->files[0].url;
    const juce::File xmlPath = mm.dirFor (spec->id).getChildFile (
        xmlUrl.substr (xmlUrl.find_last_of ('/') + 1));
    if (!report (onProgress, 0.18f)) return false;

    ai::DenseUnetModel* model = getCachedModel (modelId, device, xmlPath, &installErr);
    if (!model)
        return fail ("failed to load model: " + installErr);
    const double mRate = (double) model->sampleRate();
    if (!report (onProgress, 0.2f)) return false;

    // ---- Phase 3: per-channel inference (progress 0.2..1.0).
    for (int ch = 0; ch < nChans; ++ch)
    {
        const float base = 0.2f + 0.8f * ((float) ch / (float) nChans);
        const float span = 0.8f / (float) nChans;

        if (chans != nullptr &&
            std::find (chans->begin (), chans->end (), ch) == chans->end ())
        {
            report (onProgress, base + span);   // untouched channel: skip
            continue;
        }

        juce::AudioBuffer<float> inBuf (1, (int) len);
        std::memcpy (inBuf.getWritePointer (0), data[ch].data(), (size_t) len * sizeof (float));

        // File rate -> model rate.
        juce::AudioBuffer<float> mid;
        if (!Resampler::resample (inBuf, rate, mRate, mid))
            return fail ("resampling to the model rate failed");
        if (!report (onProgress, base + 0.1f * span)) return false;

        // Streaming inference at the model rate.
        const size_t midLen = (size_t) mid.getNumSamples();
        juce::AudioBuffer<float> backBuf (1, (int) model->outputLengthFor (midLen));
        if (!model->process (mid.getReadPointer (0), midLen, backBuf.getWritePointer (0),
                             [&onProgress, base, span] (float v) {
                                 return report (onProgress, base + (0.1f + 0.85f * v) * span);
                             }))
            return false;   // cancelled

        // Model rate -> file rate.
        juce::AudioBuffer<float> outBuf;
        if (!Resampler::resample (backBuf, mRate, rate, outBuf))
            return fail ("resampling back to the original rate failed");
        if (!report (onProgress, base + span)) return false;

        // Trim / pad to exactly the original length. The round trip is not
        // sample-exact: hold the last sample for any missing tail so no click
        // appears at the selection edge.
        data[ch].resize ((size_t) len);
        const int64_t got = outBuf.getNumSamples();
        if (got >= len)
            std::memcpy (data[ch].data(), outBuf.getReadPointer (0), (size_t) len * sizeof (float));
        else
        {
            std::memcpy (data[ch].data(), outBuf.getReadPointer (0), (size_t) got * sizeof (float));
            const float last = got > 0 ? outBuf.getReadPointer (0)[(int) got - 1] : 0.0f;
            std::fill (data[ch].begin () + (size_t) got, data[ch].end (), last);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// DeepFilterNet2/3 pipeline (Phase 2).
// ---------------------------------------------------------------------------

std::map<std::string, std::unique_ptr<ai::DeepFilter>>& dfModelCache()
{
    static std::map<std::string, std::unique_ptr<ai::DeepFilter>> cache;
    return cache;
}

ai::DeepFilter* getCachedDfModel(const std::string& modelId, const std::string& device,
                                 const juce::File& folder, ai::DeepFilterNetModel::Selection sel,
                                 std::string* errOut)
{
    const std::string key = modelId + ":" + device;
    {
        std::lock_guard<std::mutex> lock (modelCacheMutex());
        auto it = dfModelCache().find (key);
        if (it != dfModelCache().end()) return it->second.get();
    }

    auto model = std::make_unique<ai::DeepFilter>();
    if (!model->load (folder.getFullPathName ().toStdString (), device, sel, errOut))
        return nullptr;

    ai::DeepFilter* raw = model.get();
    {
        std::lock_guard<std::mutex> lock (modelCacheMutex());
        dfModelCache()[key] = std::move (model);
    }
    return raw;
}

bool runDeepFilterPipeline(const std::string& modelId, const std::string& device,
                           double rate, int nChans,
                           std::vector<std::vector<float>>& data,
                           float attenLimDb, bool df3PostFilter,
                           std::function<bool(float)> onProgress,
                           std::string* errOut,
                           const std::vector<int>* chans)
{
    auto fail = [&errOut] (const std::string& msg) -> bool
    {
        if (errOut) *errOut = msg;
        return false;
    };

    if (nChans <= 0 || data.size() != (size_t) nChans)
        return fail ("invalid channel count");
    for (int ch = 0; ch < nChans; ++ch)
        if (data[ch].empty())
            return fail ("empty selection");

    const int64_t len = data[0].size();
    for (int ch = 1; ch < nChans; ++ch)
        if (data[ch].size() != (size_t) len)
            return fail ("channel length mismatch");

    auto& mm = ai::ModelManager::instance();
    const ai::ModelSpec* spec = mm.find (modelId);
    if (!spec)
        return fail ("unknown model: " + modelId);

    const bool isDF3 = (modelId == "deepfilternet3");
    const auto sel = isDF3 ? ai::DeepFilterNetModel::Selection::DF3
                           : ai::DeepFilterNetModel::Selection::DF2;

    // ---- Phase 1: make sure all six weight files are installed (0..0.15).
    std::string installErr;
    const bool installed = mm.ensureInstalled (*spec,
        [&onProgress] (float f, const std::string&) { return report (onProgress, 0.15f * f); },
        &installErr);
    if (!installed)
        return fail ("model not available: " + installErr);

    // ---- Phase 2: load/compile the three sub-models (~progress 0.2).
    const juce::File folder = mm.dirFor (spec->id);
    if (!report (onProgress, 0.18f)) return false;

    ai::DeepFilter* model = getCachedDfModel (modelId, device, folder, sel, &installErr);
    if (!model)
        return fail ("failed to load model: " + installErr);
    const double mRate = (double) ai::DeepFilter::kModelRate;   // 48 kHz
    if (!report (onProgress, 0.2f)) return false;

    // ---- Phase 3: per-channel inference (progress 0.2..1.0).
    for (int ch = 0; ch < nChans; ++ch)
    {
        const float base = 0.2f + 0.8f * ((float) ch / (float) nChans);
        const float span = 0.8f / (float) nChans;

        if (chans != nullptr &&
            std::find (chans->begin (), chans->end (), ch) == chans->end ())
        {
            report (onProgress, base + span);   // untouched channel: skip
            continue;
        }

        juce::AudioBuffer<float> inBuf (1, (int) len);
        std::memcpy (inBuf.getWritePointer (0), data[ch].data(), (size_t) len * sizeof (float));

        // File rate -> 48 kHz.
        juce::AudioBuffer<float> mid;
        if (!Resampler::resample (inBuf, rate, mRate, mid))
            return fail ("resampling to the model rate failed");
        if (!report (onProgress, base + 0.1f * span)) return false;

        // Chunked inference at 48 kHz.
        const size_t midLen = (size_t) mid.getNumSamples();
        std::vector<float> back ((size_t) midLen);
        if (!model->filter (mid.getReadPointer (0), midLen, back,
                            attenLimDb, 20.f, isDF3 && df3PostFilter,
                            [&onProgress, base, span] (float v) {
                                return report (onProgress, base + (0.1f + 0.85f * v) * span);
                            }))
            return false;   // cancelled

        // 48 kHz -> file rate.
        juce::AudioBuffer<float> midBack (1, (int) back.size());
        std::memcpy (midBack.getWritePointer (0), back.data(), back.size() * sizeof (float));
        juce::AudioBuffer<float> outBuf;
        if (!Resampler::resample (midBack, mRate, rate, outBuf))
            return fail ("resampling back to the original rate failed");
        if (!report (onProgress, base + span)) return false;

        // Trim / pad to exactly the original length (last-sample hold).
        data[ch].resize ((size_t) len);
        const int64_t got = outBuf.getNumSamples();
        if (got >= len)
            std::memcpy (data[ch].data(), outBuf.getReadPointer (0), (size_t) len * sizeof (float));
        else
        {
            std::memcpy (data[ch].data(), outBuf.getReadPointer (0), (size_t) got * sizeof (float));
            const float last = got > 0 ? outBuf.getReadPointer (0)[(int) got - 1] : 0.0f;
            std::fill (data[ch].begin () + (size_t) got, data[ch].end (), last);
        }
    }

    return true;
}

} // namespace sc
