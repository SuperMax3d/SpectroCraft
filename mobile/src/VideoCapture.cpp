#include "VideoCapture.h"
#include <utility>

namespace scm
{
namespace
{
/** Time for the screen to show the preview gone before the camera stops
    (stopping it can hold the message thread on Android). */
constexpr int kReleaseDelayMs = 150;
} // namespace

VideoCapture::VideoCapture() = default;

VideoCapture::~VideoCapture()
{
    *alive = false;
    close (true);
}

bool VideoCapture::cameraAvailable() const
{
    return ! juce::CameraDevice::getAvailableDevices().isEmpty();
}

void VideoCapture::open (std::function<void (juce::String)> done)
{
    wanted = true;
    if (camera != nullptr) { if (done) done ({}); return; }
    pendingDone = std::move (done);
    if (opening) return;   // the open under way answers pendingDone

    const auto names = juce::CameraDevice::getAvailableDevices();
    if (names.isEmpty())
    {
        wanted = false;
        if (auto d = std::exchange (pendingDone, nullptr)) d ("No camera found.");
        return;
    }
    // A camera still closing goes first: the same camera cannot be opened twice.
    stopTimer();
    closing.reset();

    opening = true;
    // The back camera: on iOS / Android JUCE lists it first.
    juce::CameraDevice::openDeviceAsync (0, [this, token = alive] (juce::CameraDevice* cam, const juce::String& error)
    {
        if (! *token) { delete cam; return; }
        opening = false;
        std::unique_ptr<juce::CameraDevice> device (cam);
        auto answer = std::exchange (pendingDone, nullptr);
        if (! wanted)   // closed while it was opening
        {
            retire (std::move (device));
            return;
        }
        if (device == nullptr)
        {
            wanted = false;
            if (answer) answer (error.isNotEmpty() ? error : juce::String ("The camera could not be opened."));
            return;
        }
        camera = std::move (device);
        viewer.reset (camera->createViewerComponent());
        if (answer) answer ({});
    }, 640, 360, 1920, 1080, true);
}

void VideoCapture::close (bool immediately)
{
    wanted = false;
    pendingDone = nullptr;
    if (recording) stop();
    viewer.reset();   // the preview goes now
    if (camera != nullptr) retire (std::move (camera));
    if (immediately)
    {
        stopTimer();
        closing.reset();
    }
}

void VideoCapture::retire (std::unique_ptr<juce::CameraDevice> device)
{
    if (device == nullptr) return;
    closing = std::move (device);   // replaces (and releases) any older one
    startTimer (kReleaseDelayMs);
}

void VideoCapture::timerCallback()
{
    stopTimer();
    closing.reset();
}

juce::File VideoCapture::movieFileFor (const juce::File& wav)
{
    return wav.withFileExtension (juce::CameraDevice::getFileExtension());
}

bool VideoCapture::start (const juce::File& movie)
{
    if (camera == nullptr || recording) return false;
    movieFile = movie;
    startCall = juce::Time::getCurrentTime();
    firstFrame = {};
    camera->startRecordingToFile (movie, 2);
    recording = true;
    return true;
}

juce::File VideoCapture::stop()
{
    if (camera == nullptr || ! recording) return {};
    camera->stopRecording();
    recording = false;
    firstFrame = camera->getTimeOfFirstRecordedFrame();
    return movieFile;
}

juce::Time VideoCapture::firstFrameTime() const
{
    if (firstFrame.toMilliseconds() > 0) return firstFrame;
    if (camera != nullptr)
    {
        const auto t = camera->getTimeOfFirstRecordedFrame();
        if (t.toMilliseconds() > 0) return t;
    }
    return startCall;
}
} // namespace scm
