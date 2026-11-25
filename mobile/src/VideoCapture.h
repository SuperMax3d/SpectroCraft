// SpectroCraft Mobile - filming next to the sound recording (issue #73).
//
// JUCE's CameraDevice, built with JUCE_CAMERA_RECORD_AUDIO=0 (a SpectroCraft
// patch in the vendored JUCE): the movie carries the picture only, and the
// app's own WAV is its sound. The movie is written next to the WAV with the
// same name; the WAV's metadata lists it as a video track ("videoTracks", with
// a path relative to the WAV), with the offset of its first frame, so the
// desktop opens the picture with the recording.
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>

namespace scm
{
class VideoCapture
{
public:
    VideoCapture();
    ~VideoCapture();

    bool cameraAvailable() const;
    /** Opens the back camera (asks for the permission); `done` gets "" or an error. */
    void open (std::function<void (juce::String)> done);
    void close();
    bool isOpen() const noexcept { return camera != nullptr; }

    /** The live preview (owned here); nullptr while closed. */
    juce::Component* preview() noexcept { return viewer.get(); }

    /** The movie file for a recording: same name, the platform's extension. */
    static juce::File movieFileFor (const juce::File& wav);

    bool start (const juce::File& movie);
    /** Stops; returns the movie file (or {}). */
    juce::File stop();
    bool isRecording() const noexcept { return recording; }
    /** Wall-clock time of the first frame (after stop), or of the start call. */
    juce::Time firstFrameTime() const;

private:
    std::unique_ptr<juce::CameraDevice> camera;
    std::unique_ptr<juce::Component> viewer;
    juce::File movieFile;
    bool recording = false;
    juce::Time startCall;
};
} // namespace scm
