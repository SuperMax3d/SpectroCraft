// SpectroCraft Mobile - recording into the document.
//
// Unlimited: frames are appended while memory allows (the cap is shown in the
// UI; the take stops and is kept when it is reached). Loop: the last N seconds
// are kept (LoopPlan) and rotated at stop so the file starts with the oldest
// frame. The calibration of the current input is attached to the new document,
// and the take is saved to the recordings folder when it stops.
#pragma once
#include <JuceHeader.h>
#include "LoopBuffer.h"
#include "ScaleCalibration.h"

namespace scm
{
class LiveAudio;
class MobileDocument;

class Recorder : private juce::Timer
{
public:
    Recorder (LiveAudio&, MobileDocument&);
    ~Recorder() override;

    enum class Mode { Unlimited, Loop };
    void setMode (Mode m, double loopSeconds);
    Mode mode() const noexcept { return recMode; }
    double loopSeconds() const noexcept { return loopSecs; }

    /** Starts a new take in a new document. */
    bool start (const ScaleCalibration& cal, juce::String& error);
    /** Stops, rotates a wrapped loop, calls `beforeSave` (to attach metadata
        such as the video link) and saves to targetFile(); returns the saved
        file (or {}). */
    juce::File stop (juce::String& error, std::function<void()> beforeSave = {});
    /** The WAV the take will be saved to (chosen at start). */
    juce::File targetFile() const { return target; }
    bool isRecording() const noexcept { return recording; }

    /** Seconds recorded so far, and the longest take memory allows. */
    double elapsed() const;
    double maxSeconds() const;
    /** Document time of the newest frame (the loop's write head while wrapped). */
    double writeHeadSeconds() const;
    bool wrapped() const noexcept { return plan.full(); }

    /** Wall-clock time of the first recorded frame: lines a video up with
        the audio (CameraDevice reports its first frame the same way). */
    juce::Time firstFrameTime() const noexcept { return firstFrameWall; }

    /** Moves the captured frames into the document now (the timer does it at
        30 Hz; tests call it directly). */
    void poll() { timerCallback(); }

    std::function<void()> onUpdate;                        // ~30 Hz while recording
    std::function<void (const juce::String&)> onAutoStop;  // memory full / device lost

private:
    void timerCallback() override;
    void pull();

    LiveAudio& audio;
    MobileDocument& document;
    Mode recMode = Mode::Unlimited;
    double loopSecs = 30.0;
    bool recording = false;
    LoopPlan plan;
    int channels = 1;
    double rate = 48000.0;
    int64_t capFrames = 0;
    juce::Time firstFrameWall;
    juce::File target;
    std::vector<float> chunk;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
} // namespace scm
