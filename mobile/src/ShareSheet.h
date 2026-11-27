// SpectroCraft Mobile - the system's share sheet, without what JUCE's iOS
// sharer leaves behind.
//
// JUCE's iOS sharer (juce_ContentSharer_ios.cpp, NativeModalWrapperComponent)
// puts an invisible component, always on top, over the component it is given
// as parent (the Share or "..." button), and leaves it there after the sheet is
// closed, for as long as the ScopedMessageBox lives. That component takes the
// button's taps (the button lights up, nothing else happens), and opens the
// sheet again each time the button comes back on screen (another tab, then
// back: the sheet "appears by itself"). So the box is let go as soon as the
// sheet is closed.
#pragma once
#include <JuceHeader.h>
#include <functional>

namespace scm::sharesheet
{
/** Shares `files` from `anchor` (the iPad's popover points at it). `box` keeps
    the sheet open and must belong to the anchor's owner; `done (ok, error)`
    runs once the sheet is closed. */
inline void shareFiles (juce::ScopedMessageBox& box, const juce::Array<juce::URL>& files, juce::Component& anchor,
                        std::function<void (bool, const juce::String&)> done = {})
{
    juce::Component::SafePointer<juce::Component> owner (&anchor);
    auto* slot = &box;
    box = juce::ContentSharer::shareFilesScoped (files, [owner, slot, done] (bool ok, const juce::String& error)
    {
        if (done) done (ok, error);
        // later, not inside JUCE's completion handler: the box owns the
        // handler's view controller
        juce::MessageManager::callAsync ([owner, slot]
        {
            if (owner != nullptr) *slot = juce::ScopedMessageBox();
        });
    }, &anchor);
}
} // namespace scm::sharesheet
