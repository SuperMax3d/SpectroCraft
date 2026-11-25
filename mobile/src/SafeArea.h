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
} // namespace scm::safearea
