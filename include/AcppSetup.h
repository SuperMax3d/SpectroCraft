#pragma once
// SpectroCraft - first-use downloads of the audio.cpp tool and its models for
// the Stem Separation and Super Resolution windows (issue #38): Run asks once
// (sc::aidl::confirmDownload), then this downloads what is missing with a
// cancellable progress dialog and hands back to the window, which runs.

#include <juce_gui_basics/juce_gui_basics.h>

#include "AiDownloadPrompt.h"
#include "AudioCpp.h"
#include "WorkProgress.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace sc::acpp
{
/** What Run is missing: the tool, the chosen model, or both. Asks the user
    (with the tool's build to install when it is missing, preselected from
    the Backend setting). Returns false when the user says no, or when there
    is nothing to download. *binaryChoice receives the build picked (-1 = the
    tool is installed). */
inline bool confirmMissing (const juce::String& feature, const juce::File& root, const juce::String& backendCli,
                            const ModelOption* model, juce::Component* parent, int* binaryChoice)
{
    const bool needBinary = ! hasBinary (root);
    const bool needModel = model != nullptr && ! model->installed() && model->url.isNotEmpty();
    *binaryChoice = -1;
    if (! needBinary && ! needModel) return false;

    std::vector<sc::aidl::Item> items;
    juce::StringArray variants;
    int pick = binaryChoiceForBackend (backendCli);
    if (needBinary)
    {
        const auto choices = binaryChoices();
        if (choices.empty()) return false;   // no build for this platform
        for (const auto& c : choices)
            variants.add (c.label + "   (" + sc::aidl::sizeText (c.sizeBytes) + ")");
        pick = juce::jlimit (0, (int) choices.size() - 1, pick);
        items.push_back ({ "the audio.cpp tool (" + choices[(size_t) pick].label + ")", choices[(size_t) pick].sizeBytes });
    }
    if (needModel)
        items.push_back ({ model->name, model->sizeBytes });

    if (! sc::aidl::confirmDownload (feature, items, parent, variants, needBinary ? &pick : nullptr))
        return false;
    *binaryChoice = needBinary ? pick : -1;
    return true;
}

/** Downloads the tool (binaryChoice >= 0: that binaryChoices() build) and/or
    `model` into `root` on a worker thread, with a cancellable progress
    dialog over `parent`. onDone(ok, cancelled, error) runs on the message
    thread unless `alive` was cleared meanwhile (the window closed). */
inline void downloadMissing (juce::Component* parent, const juce::File& root, int binaryChoice,
                             std::optional<ModelOption> model, std::shared_ptr<std::atomic<bool>> alive,
                             std::function<void (bool ok, bool cancelled, juce::String error)> onDone)
{
    auto* prog = new sc::WorkProgressDialog ("Downloading", "Starting...", true, parent);
    prog->setProgress (0.0);
    prog->beginModal();

    std::thread ([prog, root, binaryChoice, model, alive, onDone]
    {
        auto setStatus = [prog] (const juce::String& s)
            { juce::MessageManager::callAsync ([prog, s] { prog->setStatusText (s); }); };
        double lastSent = -1.0;
        auto sendProgress = [prog, &lastSent] (double v)
        {
            if (v == lastSent) return;   // e.g. repeated 1.0 on chunked responses
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([prog, v] { prog->setProgress (v); });
        };

        std::string err;
        bool ok = true;
        if (binaryChoice >= 0)
        {
            const auto choices = binaryChoices();
            const size_t i = (size_t) juce::jlimit (0, juce::jmax (0, (int) choices.size() - 1), binaryChoice);
            if (choices.empty()) { ok = false; err = "no audio.cpp build for this platform"; }
            else
            {
                const juce::File zip = root.getChildFile (archiveFileName());   // .zip or .tar.gz
                setStatus ("Downloading " + choices[i].label + "...");
                ok = downloadFile (choices[i].url, zip,
                    [&] (float f) { sendProgress (f); return ! prog->cancelled.load(); }, &err,
                    choices[i].sizeBytes);
                if (ok)
                {
                    setStatus ("Extracting the audio.cpp tool...");
                    ok = installBinaryFromArchive (zip, choices[i].id, root, &err);
                }
            }
        }
        if (ok && model.has_value() && ! model->installed() && model->url.isNotEmpty())
        {
            lastSent = -1.0;
            model->dest.getParentDirectory().createDirectory();
            setStatus ("Downloading " + model->name + "...");
            ok = downloadFile (model->url.toStdString(), model->dest,
                [&] (float f) { sendProgress (f); return ! prog->cancelled.load(); }, &err,
                model->sizeBytes);
        }

        const bool cancelled = prog->cancelled.load();
        const juce::String error (err);
        juce::MessageManager::callAsync ([prog, ok, cancelled, error, alive, onDone]
        {
            delete prog;
            if (! alive || ! alive->load()) return;   // the window closed meanwhile
            if (onDone) onDone (ok, cancelled, error);
        });
    }).detach();
}
} // namespace sc::acpp
