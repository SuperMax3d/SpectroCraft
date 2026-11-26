// SpectroCraft Mobile - the screen edges the app must not draw under: the
// notch or Dynamic Island, the status bar and the home indicator.
//
// JUCE's Displays::safeAreaInsets cannot be trusted at start-up on iOS: JUCE
// reads it from a temporary window of the app's scene, and when the display
// list is built before the scene connects that window has no scene and the
// insets are zero. JUCE refreshes the list again only on a rotation or the
// keyboard, so the app could stay laid out under the status bar. On iOS the
// insets therefore come from the component's own window (SafeArea_ios.mm),
// and MainScreen lays out again whenever they change.
#pragma once
#include <JuceHeader.h>

namespace scm::safearea
{
/** The insets of c's window, in c's coordinates (c fills its window). Zero on
    the desktop. */
juce::BorderSize<int> insetsFor (const juce::Component& c);

/** The screen c is on: its last ancestor under the window (the component that
    fills the window). Things drawn over the other views go there. */
inline juce::Component& screenOf (juce::Component& c)
{
    juce::Component* host = &c;
    while (auto* p = host->getParentComponent())
    {
        if (p->isOnDesktop()) break;
        host = p;
    }
    return *host;
}

/** The part of screenOf (c) that may be drawn on, in that screen's coordinates. */
inline juce::Rectangle<int> safeBoundsOf (juce::Component& c)
{
    auto& screen = screenOf (c);
    return insetsFor (screen).subtractedFrom (screen.getLocalBounds());
}
} // namespace scm::safearea
