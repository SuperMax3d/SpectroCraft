// SpectroCraft Mobile - the number being typed on the number pad
// (TouchControls.h), without the pad: digits, one decimal point, a sign.
#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <optional>
#include <utility>

namespace scm::numpad
{
/** `value` with `decimals` places (none when 0), as the pad shows it. */
inline juce::String format (double value, int decimals)
{
    if (decimals <= 0) return juce::String (juce::roundToInt (value));
    return juce::String (value, decimals);
}

class Entry
{
public:
    /** Starts from the value shown. The first key replaces it, as a selected
        text would be. When the range has no positive values (levels in dBFS),
        typed digits come out negative. */
    Entry (juce::String start, bool negativeOnly) : shown (std::move (start)), negative (negativeOnly) {}

    void digit (int d)
    {
        jassert (d >= 0 && d <= 9);
        const juce::String ch (d);
        if (std::exchange (fresh, false))
            shown = negative ? "-" + ch : ch;
        else if (shown == "0" || shown == "-0")
            shown = shown.dropLastCharacters (1) + ch;
        else if (shown.length() < kMaxLength)
            shown << ch;
    }

    void point()
    {
        if (std::exchange (fresh, false))
            shown = negative ? "-0." : "0.";
        else if (! shown.containsChar ('.') && shown.length() < kMaxLength)
            shown << (shown.isEmpty() || shown == "-" ? "0." : ".");
    }

    void toggleSign()
    {
        fresh = false;   // the value stays: only its sign changes
        shown = shown.startsWithChar ('-') ? shown.substring (1) : "-" + shown;
    }

    void backspace()
    {
        if (std::exchange (fresh, false))
            shown = {};
        else
            shown = shown.dropLastCharacters (1);
    }

    const juce::String& text() const noexcept { return shown; }
    /** Still the starting value, which the next key replaces. */
    bool isFresh() const noexcept { return fresh; }

    /** The number typed; nothing while it is not one yet ("", "-", "."). */
    std::optional<double> value() const
    {
        const auto t = shown.trim();
        if (! t.containsAnyOf ("0123456789") || ! t.containsOnly ("0123456789.-")) return {};
        const double v = t.getDoubleValue();
        if (! std::isfinite (v)) return {};
        return v;
    }

    static constexpr int kMaxLength = 12;

private:
    juce::String shown;
    bool negative = false;
    bool fresh = true;
};
} // namespace scm::numpad
