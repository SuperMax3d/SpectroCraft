// SpectroCraft Mobile - filming next to the sound recording (issue #73).
//
// JUCE's CameraDevice, built with JUCE_CAMERA_RECORD_AUDIO=0 (a SpectroCraft
// patch in the vendored JUCE): the movie carries the picture only, and the
// app's own WAV is its sound. The movie is written next to the WAV with the
// same name; the WAV's metadata lists it as a video track ("videoTracks", with
// a path relative to the WAV), with the offset of its first frame, so the
// desktop opens the picture with the recording (and VideoPane plays it here).
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <vector>

namespace scm
{
class VideoCapture : private juce::Timer
{
public:
    VideoCapture();
    ~VideoCapture() override;

    bool cameraAvailable() const;
    /** Opens the back camera (asks for the permission). Every caller's `done`
        gets "" or an error, once. A close() before the camera is up cancels
        it: the callers are then not answered. */
    void open (std::function<void (juce::String)> done);
    /** Releases the camera. The preview goes at once; the camera device itself
        is released a moment later (`immediately` = now, e.g. going to the
        background), so the screen updates before the camera stops. */
    void close (bool immediately = false);
    bool isOpen() const noexcept { return camera != nullptr; }

    /** The live preview (owned here); nullptr while closed. */
    juce::Component* preview() noexcept { return viewer.get(); }

    /** The movie file for a recording: same name, the platform's extension. */
    static juce::File movieFileFor (const juce::File& wav);

    /** Takes a still (Android: the preview must be on screen). `done` gets the
        picture, or an invalid picture and a reason, exactly once: also when the
        camera never answers (JUCE does not report every failure), is closed or
        is filming. One request at a time; a request that times out is retried
        once (Android drops a still asked before its session is ready). */
    using StillCallback = std::function<void (const juce::Image&, const juce::String& error)>;
    void takeStill (StillCallback done, int timeoutMs = 6000);
    bool isTakingStill() const noexcept { return stillPending; }

    bool start (const juce::File& movie);
    /** Stops; returns the movie file (or {}). The camera finishes writing it
        a little later. */
    juce::File stop();
    bool isRecording() const noexcept { return recording; }
    /** Wall-clock time of the first frame (after stop), or of the start call. */
    juce::Time firstFrameTime() const;

private:
    void timerCallback() override;
    /** The camera is released `delayMs` later (see close). */
    void retire (std::unique_ptr<juce::CameraDevice>, int delayMs = 150);
    void answerWaiters (const juce::String& error);
    void requestStill (int timeoutMs, bool retry);
    void finishStill (int serial, const juce::Image& image, const juce::String& error);

    std::unique_ptr<juce::CameraDevice> camera, closing;
    std::unique_ptr<juce::Component> viewer;
    std::vector<std::function<void (juce::String)>> openWaiters;   // every caller of open() while it is pending
    bool opening = false, wanted = false;
    bool stillPending = false;
    int stillSerial = 0;
    StillCallback stillDone;
    juce::File movieFile;
    bool recording = false;
    juce::Time startCall, firstFrame;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_WEAK_REFERENCEABLE (VideoCapture)
};
} // namespace scm
