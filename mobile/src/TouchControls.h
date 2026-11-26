// SpectroCraft Mobile - controls made for a finger.
//
// Numbers are typed on a small pad of the app's own, not the system keyboard.
// For a number JUCE asks iOS for its full-size keyboard (the numbers page of
// UIKeyboardTypeNumbersAndPunctuation, the only numeric layout with a return
// key), it covers the panel being edited, the layout does not move out of its
// way, and it stays up while the field keeps the focus. The pad opens beside
// the value, holds only what a number needs, changes the value on OK, and goes
// away on OK or a tap anywhere else.
//
// In a scrolled panel, a drag that starts on a TouchSlider never scrolls the
// panel and a tap on its track does not move the value; the panel scrolls from
// its labels and from a scroll bar wide enough to see and grab (setupScrolling).
#pragma once
#include <JuceHeader.h>
#include "NumberEntry.h"
#include <functional>
#include <optional>

namespace scm
{
namespace numpad
{
struct Request
{
    juce::String title;                     // what the number is
    juce::String suffix;                    // its unit, after it (" Hz")
    std::optional<double> value;            // what the pad starts from (none: empty)
    double minimum = 0.0, maximum = 1.0;
    int decimals = 1;
    std::function<void (double)> onEnter;   // the number typed, kept in [minimum, maximum]
};

/** Opens the pad beside `target` (or that part of it), over the screen that holds it. */
void show (juce::Component& target, Request, juce::Rectangle<int> areaInTarget = {});
} // namespace numpad

/** A slider whose value box opens the number pad, and that a scrolled panel
    leaves alone: a drag on it never scrolls, a tap on its track does not move
    the value (drag the thumb, or type the number). Its title names the number
    on the pad. */
class TouchSlider : public juce::Slider
{
public:
    TouchSlider();
    ~TouchSlider() override;

    /** The usual setup: a horizontal slider with its value box on the right. */
    void setup (const juce::String& title, double lo, double hi, double interval, const juce::String& suffix,
                int decimals, int textBoxWidth = 96);

private:
    struct Opener : juce::MouseListener
    {
        explicit Opener (TouchSlider& s) : slider (s) {}
        void mouseUp (const juce::MouseEvent&) override;
        TouchSlider& slider;
    };
    Opener opener { *this };
};

/** A number in a box: a tap opens the number pad. Empty until it has a value
    when no default is given. */
class NumberField : public juce::Component
{
public:
    NumberField (const juce::String& title, double minimum, double maximum, int decimals,
                 const juce::String& suffix = {});

    void setValue (std::optional<double>, juce::NotificationType = juce::dontSendNotification);
    std::optional<double> getValue() const noexcept { return value; }
    /** Called after the pad changed the value. */
    std::function<void()> onChange;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    double minimum, maximum;
    int decimals;
    juce::String suffix;
    std::optional<double> value;
};

/** Scroll bar width (points). */
constexpr int kScrollBarW = 18;

/** A panel's viewport: scrolls by dragging (outside sliders) and by a wide
    scroll bar on the right, shown while the content is taller than the panel. */
void setupScrolling (juce::Viewport&);
} // namespace scm
