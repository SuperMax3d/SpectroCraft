// SpectroCraft Mobile - the audio device: microphone in, generator and file
// playback out, all in one full-duplex callback.
//
// The microphone is opened in measurement mode (no automatic gain, no noise
// suppression: AudioIODevice::setAudioPreprocessingEnabled (false); on iOS this
// is AVAudioSessionModeMeasurement) so calibrated levels mean something.
//
// Captured frames go through a lock-free FIFO to the message thread, which
// appends them to the document (Recorder). A separate one-shot capture feeds
// the calibrator. Levels are published for the meters.
#pragma once
#include <JuceHeader.h>
#include "SignalSource.h"
#include <atomic>
#include <functional>
#include <vector>

namespace scm
{
class LiveAudio : public juce::AudioIODeviceCallback,
                  private juce::ChangeListener
{
public:
    LiveAudio();
    ~LiveAudio() override;

    juce::AudioDeviceManager& devices() noexcept { return deviceManager; }

    /** Asks for the microphone permission, then opens the device with
        `inputChannels` inputs (1 or 2) and two outputs. `done` receives an
        error ("" on success) on the message thread. */
    void open (int inputChannels, std::function<void (juce::String)> done);
    /** Reopens with another input (a JUCE device name; "" = default). */
    juce::String selectInputDevice (const juce::String& name, int inputChannels);
    bool isOpen() const;
    /** Tests: behave as if a device were open at this rate with these inputs,
        and feed audioDeviceIOCallbackWithContext() by hand. */
    void simulateDevice (double sampleRate, int inputChannels);

    double sampleRate() const noexcept { return rate.load(); }
    int numInputs() const noexcept { return inputs.load(); }
    juce::String inputName() const;

    // ---- output
    SignalSource& generator() noexcept { return gen; }
    /** Message thread: the source to play (the document's AudioEngine), or nullptr. */
    void setPlaybackSource (juce::AudioSource* s);

    // ---- recording FIFO
    void setCapturing (bool on);
    bool isCapturing() const noexcept { return capturing.load(); }
    /** Message thread: moves up to maxFrames captured frames (interleaved,
        numInputs() channels) into dest. Returns the frame count. */
    int readCaptured (float* dest, int maxFrames);
    /** Frames lost because the FIFO was full (the message thread stalled). */
    int64_t droppedFrames() const noexcept { return dropped.load(); }

    // ---- one-shot capture (calibrator): the first input channel
    void startShot (double seconds);
    bool shotReady() const noexcept { return shotDone.load(); }
    bool shotRunning() const noexcept { return shotWanted.load() > 0 && ! shotDone.load(); }
    double shotProgress() const noexcept;
    std::vector<float> takeShot();

    // ---- meters (dBFS, decaying peak hold; -200 when silent)
    float inputPeakDb (int ch) const noexcept;
    float inputRmsDb (int ch) const noexcept;

    std::function<void()> onDeviceChanged;   // message thread

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::String applySetup (const juce::String& inputName, int inputChannels);

    juce::AudioDeviceManager deviceManager;
    SignalSource gen;

    juce::CriticalSection sourceLock;
    juce::AudioSource* source = nullptr;
    bool sourcePrepared = false;
    int blockSize = 512;

    bool simulated = false;
    std::atomic<double> rate { 48000.0 };
    std::atomic<int> inputs { 1 };

    static constexpr int kFifoFrames = 1 << 19;   // ~11 s at 48 kHz
    static constexpr int kMaxChannels = 2;
    juce::AbstractFifo fifo { kFifoFrames };
    std::vector<float> fifoData;
    std::atomic<bool> capturing { false };
    std::atomic<int64_t> dropped { 0 };

    std::vector<float> shot;
    std::atomic<int> shotWanted { 0 };
    std::atomic<int> shotFilled { 0 };
    std::atomic<bool> shotDone { false };

    std::atomic<float> peak[kMaxChannels];
    std::atomic<float> rms[kMaxChannels];
    std::vector<float> scratchOut;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveAudio)
};
} // namespace scm
