// SpectroCraft Mobile - a slider over named steps, for a finger (the sound
// map's bands, in place of a drop-down list).
//
// A tap or a drag chooses the step under the finger; a finger held still
// (about half a second) makes the drag four times finer (StepDrag.h, the
// spectrogram's precision touch). While the finger is down, a readout above it
// names the step and its neighbours, since the finger hides the thumb; the
// current step's name is drawn above the thumb, some ticks are labelled under
// the track. Its value is the step's index: getValue / onValueChange, the keys
// and screen readers work as on any juce::Slider (getTextFromValue gives the
// step's name).
#pragma once
#include <JuceHeader.h>
#include "StepDrag.h"

namespace scm
{
class StepSlider : public juce::Slider, private juce::Timer
{
public:
    StepSlider();
    ~StepSlider() override;

    /** Height it wants: the name above the thumb, the track, the tick labels. */
    static constexpr int kHeight = 58;

    /** The steps' names and the short label under some ticks (empty: none).
        `groupFrom` > 0 leaves a gap in the track before that step (the last
        steps are a group apart). Keeps the current index when it still exists. */
    void setSteps (const juce::StringArray& names, const juce::StringArray& tickLabels, int groupFrom = -1);
    int getNumSteps() const noexcept { return names.size(); }
    int getStep() const { return juce::roundToInt (getValue()); }
    void setStep (int step, juce::NotificationType);

    /** Where step `i`'s centre is drawn, in this component. */
    float xOfStep (int i) const;

    /** Development hook (screenshots): a finger put down on step `i` and left
        there (fine after precise::kHoldMs), until a real touch or hiding ends it. */
    void touchForTest (int i);

    juce::String getTextFromValue (double) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override {}
    void valueChanged() override;
    void visibilityChanged() override;

private:
    class Readout;
    void timerCallback() override;
    void press (juce::Point<float>, int touchIndex);
    void syncTrack();
    void updateReadout();
    void endTouch();
    float trackY() const;

    juce::StringArray names, ticks;
    int group = -1;
    steps::Drag drag;
    int touch = -1;   // the finger that owns the drag (other fingers are ignored)
    juce::Point<float> finger;
    std::unique_ptr<Readout> readout;
};
} // namespace scm
