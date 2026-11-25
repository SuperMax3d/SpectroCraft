#include "VideoCapture.h"

namespace scm
{
VideoCapture::VideoCapture() = default;

VideoCapture::~VideoCapture()
{
    close();
}

bool VideoCapture::cameraAvailable() const
{
    return ! juce::CameraDevice::getAvailableDevices().isEmpty();
}

void VideoCapture::open (std::function<void (juce::String)> done)
{
    if (camera != nullptr) { if (done) done ({}); return; }
    const auto names = juce::CameraDevice::getAvailableDevices();
    if (names.isEmpty()) { if (done) done ("No camera found."); return; }
    // The back camera: on iOS / Android JUCE lists it first.
    juce::CameraDevice::openDeviceAsync (0, [this, done] (juce::CameraDevice* cam, const juce::String& error)
    {
        if (cam == nullptr)
        {
            if (done) done (error.isNotEmpty() ? error : juce::String ("The camera could not be opened."));
            return;
        }
        camera.reset (cam);
        viewer.reset (camera->createViewerComponent());
        if (done) done ({});
    }, 640, 360, 1920, 1080, true);
}

void VideoCapture::close()
{
    if (recording) stop();
    viewer.reset();
    camera.reset();
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
    camera->startRecordingToFile (movie, 2);
    recording = true;
    return true;
}

juce::File VideoCapture::stop()
{
    if (camera == nullptr || ! recording) return {};
    camera->stopRecording();
    recording = false;
    return movieFile;
}

juce::Time VideoCapture::firstFrameTime() const
{
    if (camera != nullptr)
    {
        const auto t = camera->getTimeOfFirstRecordedFrame();
        if (t.toMilliseconds() > 0) return t;
    }
    return startCall;
}
} // namespace scm
