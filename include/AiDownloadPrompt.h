#pragma once
// SpectroCraft - one behaviour for every AI feature whose model (or the
// audio.cpp tool) is not on this computer yet (issue #38): the model lists
// show every version, a missing one marked "(Download Required)"; Run then
// asks once, downloads, and runs. Used by Noise Suppression, Transcription
// (single file and folder), Stem Separation and Super Resolution.

#include <JuceHeader.h>
#include "AlertGate.h"

#include <cstdint>
#include <vector>

namespace sc::aidl
{
/** Suffix of a model that still has to be downloaded. */
inline const juce::String& downloadTag()
{
    static const juce::String t (" (Download Required)");
    return t;
}

/** `name`, plus " (Download Required)" when it is not installed. */
inline juce::String label (const juce::String& name, bool installed)
{
    return installed ? name : name + downloadTag();
}

/** The name without the tag (a combo's text back to the model's name). */
inline juce::String stripTag (const juce::String& text)
{
    return text.endsWith (downloadTag()) ? text.dropLastCharacters (downloadTag().length()) : text;
}

/** "84 MB", "2.38 GB" ("" for an unknown size). */
inline juce::String sizeText (int64_t bytes)
{
    if (bytes <= 0) return {};
    if (bytes >= (int64_t) 1000000000) return juce::String ((double) bytes / 1.0e9, 2) + " GB";
    return juce::String (juce::jmax (1.0, (double) bytes / 1.0e6), 1) + " MB";
}

/** One thing to download: what it is and how big. */
struct Item
{
    juce::String name;
    int64_t bytes = 0;
};

/** The confirmation shown when Run needs files that are not installed:
    lists them with their sizes and asks "Download and Run" / "Cancel".
    With `variants` (the audio.cpp builds, when the tool itself is missing)
    a combo lets the user pick one; *variantIndex holds the initial choice
    and receives the one picked. Returns true to go ahead. Headless (unit
    tests) it records the question and answers no. */
inline bool confirmDownload (const juce::String& feature, const std::vector<Item>& items,
                             juce::Component* parent,
                             const juce::StringArray& variants = {}, int* variantIndex = nullptr)
{
    juce::String msg = feature + " needs files that are not on this computer yet:\n\n";
    int64_t total = 0;
    for (const auto& it : items)
    {
        msg << "  - " << it.name;
        if (it.bytes > 0) msg << "  (" << sizeText (it.bytes) << ")";
        msg << "\n";
        total += juce::jmax ((int64_t) 0, it.bytes);
    }
    msg << "\nDownload them now";
    if (total > 0) msg << " (" << sizeText (total) << " in all)";
    msg << "? " << feature << " runs as soon as the download is done.";

    if (sc::alert::swallow ("Download required", msg))
        return false;

    juce::AlertWindow w ("Download required", msg, juce::AlertWindow::QuestionIcon, parent);
    w.setColour (juce::AlertWindow::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));
    if (!variants.isEmpty() && variantIndex != nullptr)
    {
        w.addComboBox ("variant", variants, "audio.cpp build");
        if (auto* c = w.getComboBoxComponent ("variant"))
        {
            c->setSelectedItemIndex (juce::jlimit (0, variants.size() - 1, *variantIndex), juce::dontSendNotification);
            c->setTooltip ("Build of the audio.cpp tool to install. Pick the one that matches "
                           "your processor (the Backend setting).");
        }
    }
    w.addButton ("Download and Run", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    const bool ok = w.runModalLoop() == 1;
    if (ok && variantIndex != nullptr)
        if (auto* c = w.getComboBoxComponent ("variant"))
            *variantIndex = juce::jmax (0, c->getSelectedItemIndex());
    return ok;
}
} // namespace sc::aidl
