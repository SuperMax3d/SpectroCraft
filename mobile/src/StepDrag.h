// SpectroCraft Mobile - the finger on a StepSlider, without the component: a
// slider over a few dozen named steps (the sound map's bands), where a step can
// be only a few points wide on a phone.
//
// The value follows the finger: a tap puts it on the step nearest the finger,
// a drag moves it with the finger. A finger held still for precise::kHoldMs
// makes the drag fine: from then on a virtual finger, starting on the current
// step, moves at precise::kGain of the finger's speed (as the spectrogram's
// reticle) and the value is the step nearest it; a step then takes four times
// as much finger travel. The fine drag lasts until the finger is lifted.
#pragma once
#include "PreciseTouch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace scm::steps
{
inline constexpr float kStillPt = 6.0f;   // a finger that moves less than this is still

class Drag
{
public:
    /** The steps' centres on the track, left to right (their spacing may vary). */
    void setPositions (std::vector<float> xs)
    {
        centres = std::move (xs);
        std::sort (centres.begin(), centres.end());
        value = std::clamp (value, 0, std::max (0, (int) centres.size() - 1));
    }

    int count() const noexcept { return (int) centres.size(); }

    /** x of a step's centre (0 when there are no steps). */
    float xOf (int step) const noexcept
    {
        return centres.empty() ? 0.0f : centres[(size_t) std::clamp (step, 0, count() - 1)];
    }

    /** The step whose centre is nearest x. */
    int stepAt (float x) const noexcept
    {
        if (centres.empty()) return 0;
        const auto it = std::lower_bound (centres.begin(), centres.end(), x);
        if (it == centres.begin()) return 0;
        if (it == centres.end()) return count() - 1;
        const int i = (int) (it - centres.begin());
        return x - centres[(size_t) i - 1] <= centres[(size_t) i] - x ? i - 1 : i;
    }

    /** A finger put down: the value goes to the step nearest it. */
    int press (float x, juce::uint32 nowMs) noexcept
    {
        down = true;
        isFine = false;
        stillX = x;
        stillSinceMs = nowMs;
        value = stepAt (x);
        return value;
    }

    /** The finger moved: the step it chooses now. */
    int move (float x, juce::uint32 nowMs) noexcept
    {
        if (! down) return value;
        if (std::abs (x - stillX) >= kStillPt)
        {
            stillX = x;
            stillSinceMs = nowMs;
        }
        if (! isFine)
        {
            value = stepAt (x);
            return value;
        }
        float v = virtualFrom + (x - fingerFrom) * precise::kGain;
        if (! centres.empty() && (v < centres.front() || v > centres.back()))
        {
            // past an end: start again from it, so coming back moves at once
            v = std::clamp (v, centres.front(), centres.back());
            virtualFrom = v;
            fingerFrom = x;
        }
        value = stepAt (v);
        return value;
    }

    /** Polled while the finger is down: true when the drag turns fine now
        (the finger has been still for kHoldMs). */
    bool hold (float x, juce::uint32 nowMs) noexcept
    {
        if (! down || isFine || (juce::int32) (nowMs - stillSinceMs) < precise::kHoldMs) return false;
        isFine = true;
        fingerFrom = x;
        virtualFrom = xOf (value);   // from the step it is on: nothing moves now
        return true;
    }

    void release() noexcept { down = isFine = false; }

    bool fingerDown() const noexcept { return down; }
    bool fine() const noexcept { return isFine; }
    int step() const noexcept { return value; }
    void setStep (int s) noexcept { value = std::clamp (s, 0, std::max (0, count() - 1)); }

private:
    std::vector<float> centres;
    bool down = false, isFine = false;
    int value = 0;
    float stillX = 0.0f;
    juce::uint32 stillSinceMs = 0;
    float fingerFrom = 0.0f, virtualFrom = 0.0f;
};
} // namespace scm::steps
