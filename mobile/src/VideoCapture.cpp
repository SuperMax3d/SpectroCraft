#include "VideoCapture.h"
#include <utility>

namespace scm
{
namespace
{
/** Time for the screen to show the preview gone before the camera stops
    (stopping it can hold the message thread on Android). */
constexpr int kReleaseDelayMs = 150;
/** How long a camera closed while it was taking a still is kept. */
constexpr int kStillGraceMs = 3000;
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
    // Every caller is answered: the Map tab and the Video button can ask at
    // the same time, and JUCE drops a second open of the same camera.
    openWaiters.push_back (std::move (done));
    if (opening) return;   // the open under way answers them all

    const auto names = juce::CameraDevice::getAvailableDevices();
    if (names.isEmpty())
    {
        wanted = false;
        answerWaiters ("No camera found.");
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
        if (! wanted)   // closed while it was opening: the callers were dropped
        {
            retire (std::move (device));
            return;
        }
        if (device == nullptr)
        {
            wanted = false;
            answerWaiters (error.isNotEmpty() ? error : juce::String ("The camera could not be opened."));
            return;
        }
        camera = std::move (device);
        viewer.reset (camera->createViewerComponent());
        answerWaiters ({});
    }, 640, 360, 1920, 1080, true);
}

void VideoCapture::close (bool immediately)
{
    wanted = false;
    openWaiters.clear();   // a pending open is dropped when it lands
    if (recording) stop();
    viewer.reset();   // the preview goes now
    if (stillPending)
    {
        finishStill (stillSerial, {}, "The camera was closed.");
        // JUCE delivers an in-flight still through the camera object (iOS: a
        // callback holding a plain pointer): keep it a while, even when the
        // camera should go at once.
        if (camera != nullptr) retire (std::move (camera), kStillGraceMs);
        return;
    }
    if (camera != nullptr) retire (std::move (camera));
    if (immediately)
    {
        stopTimer();
        closing.reset();
    }
}

void VideoCapture::answerWaiters (const juce::String& error)
{
    auto waiters = std::exchange (openWaiters, {});
    for (auto& w : waiters)
        if (w) w (error);
}

void VideoCapture::retire (std::unique_ptr<juce::CameraDevice> device, int delayMs)
{
    if (device == nullptr) return;
    closing = std::move (device);   // replaces (and releases) any older one
    startTimer (delayMs);
}

void VideoCapture::timerCallback()
{
    stopTimer();
    closing.reset();
}

void VideoCapture::takeStill (StillCallback done, int timeoutMs)
{
    if (! done) return;
    if (camera == nullptr) { done ({}, "The camera is not open."); return; }
    if (recording) { done ({}, "The camera is filming."); return; }
    if (stillPending) { done ({}, "A picture is already being taken."); return; }
    stillPending = true;
    stillDone = std::move (done);
    requestStill (timeoutMs, true);
}

void VideoCapture::requestStill (int timeoutMs, bool retry)
{
    const int serial = ++stillSerial;
    juce::WeakReference<VideoCapture> weak (this);
    camera->takeStillPicture ([weak, serial] (const juce::Image& image)
    {
        if (auto* self = weak.get())
            self->finishStill (serial, image, image.isValid() ? juce::String() : juce::String ("The picture is empty."));
    });
    juce::Timer::callAfterDelay (timeoutMs, [weak, serial, timeoutMs, retry]
    {
        auto* self = weak.get();
        if (self == nullptr || ! self->stillPending || serial != self->stillSerial) return;
        if (retry && self->camera != nullptr)
            self->requestStill (timeoutMs, false);
        else
            self->finishStill (serial, {}, "The camera did not take the picture.");
    });
}

void VideoCapture::finishStill (int serial, const juce::Image& image, const juce::String& error)
{
    // a late picture of the first try still counts during the retry
    if (! stillPending || serial > stillSerial || serial < stillSerial - 1) return;
    stillPending = false;
    auto cb = std::move (stillDone);
    stillDone = nullptr;
    if (cb) cb (image, image.isValid() ? juce::String() : error);
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
