#include "LiveAudio.h"
#include <cmath>

namespace scm
{
LiveAudio::LiveAudio()
{
    fifoData.assign ((size_t) kFifoFrames * kMaxChannels, 0.0f);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        peak[c].store (-200.0f);
        rms[c].store (-200.0f);
    }
    scratchOut.resize (8192);
    shot.assign ((size_t) (10.0 * 96000.0), 0.0f);   // the calibrator's capture: up to 10 s at 96 kHz
    deviceManager.addChangeListener (this);
}

LiveAudio::~LiveAudio()
{
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void LiveAudio::open (int inputChannels, std::function<void (juce::String)> done)
{
    auto start = [this, inputChannels, done] (bool granted)
    {
        if (! granted)
        {
            if (done) done ("Microphone access is off. Allow it in the system settings to record.");
            return;
        }
        const auto err = deviceManager.initialise (juce::jlimit (1, kMaxChannels, inputChannels), 2,
                                                   nullptr, true);
        if (err.isNotEmpty())
        {
            if (done) done (err);
            return;
        }
        deviceManager.addAudioCallback (this);
        const auto err2 = applySetup ({}, inputChannels);
        if (done) done (err2);
    };

    if (juce::RuntimePermissions::isRequired (juce::RuntimePermissions::recordAudio)
        && ! juce::RuntimePermissions::isGranted (juce::RuntimePermissions::recordAudio))
        juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio, start);
    else
        start (true);
}

bool LiveAudio::isOpen() const
{
    return simulated || deviceManager.getCurrentAudioDevice() != nullptr;
}

void LiveAudio::simulateDevice (double sampleRate, int inputChannels)
{
    simulated = true;
    rate.store (sampleRate);
    inputs.store (juce::jlimit (1, kMaxChannels, inputChannels));
    gen.prepare (sampleRate);
}

juce::String LiveAudio::inputName() const
{
    auto setup = deviceManager.getAudioDeviceSetup();
    if (setup.inputDeviceName.isNotEmpty()) return setup.inputDeviceName;
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getName();
    return "Microphone";
}

juce::String LiveAudio::applySetup (const juce::String& inputName, int inputChannels)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    if (inputName.isNotEmpty()) setup.inputDeviceName = inputName;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (0, juce::jlimit (1, kMaxChannels, inputChannels), true);
    setup.useDefaultOutputChannels = true;
    const auto err = deviceManager.setAudioDeviceSetup (setup, true);
    if (auto* d = deviceManager.getCurrentAudioDevice())
        d->setAudioPreprocessingEnabled (false);   // measurement mode
    return err;
}

juce::String LiveAudio::selectInputDevice (const juce::String& name, int inputChannels)
{
    return applySetup (name, inputChannels);
}

void LiveAudio::setPlaybackSource (juce::AudioSource* s)
{
    juce::AudioSource* old = nullptr;
    {
        const juce::ScopedLock sl (sourceLock);
        old = sourcePrepared ? source : nullptr;
        source = s;
        sourcePrepared = false;
        if (source != nullptr && isOpen())
        {
            source->prepareToPlay (blockSize, rate.load());
            sourcePrepared = true;
        }
    }
    if (old != nullptr && old != s) old->releaseResources();
}

void LiveAudio::setCapturing (bool on)
{
    if (on) fifo.reset();
    dropped.store (0);
    capturing.store (on);
}

int LiveAudio::readCaptured (float* dest, int maxFrames)
{
    const int ch = inputs.load();
    const auto scope = fifo.read (std::min (maxFrames, fifo.getNumReady()));
    int written = 0;
    const auto copy = [&] (int start, int n)
    {
        std::memcpy (dest + (size_t) written * ch, fifoData.data() + (size_t) start * ch,
                     sizeof (float) * (size_t) n * ch);
        written += n;
    };
    if (scope.blockSize1 > 0) copy (scope.startIndex1, scope.blockSize1);
    if (scope.blockSize2 > 0) copy (scope.startIndex2, scope.blockSize2);
    return written;
}

void LiveAudio::startShot (double seconds)
{
    // The buffer is allocated once (constructor) and never reallocated, so an
    // audio callback still writing the previous shot cannot touch freed memory.
    shotWanted.store (0);
    shotDone.store (false);
    shotFilled.store (0);
    shotWanted.store ((int) juce::jlimit (1.0, (double) shot.size(), seconds * rate.load()));
}

double LiveAudio::shotProgress() const noexcept
{
    const int w = shotWanted.load();
    return w > 0 ? (double) shotFilled.load() / w : 0.0;
}

std::vector<float> LiveAudio::takeShot()
{
    const int n = shotDone.load() ? shotFilled.load() : 0;
    std::vector<float> out (shot.begin(), shot.begin() + n);
    shotWanted.store (0);
    shotDone.store (false);
    return out;
}

float LiveAudio::inputPeakDb (int ch) const noexcept
{
    return juce::isPositiveAndBelow (ch, kMaxChannels) ? peak[ch].load() : -200.0f;
}

float LiveAudio::inputRmsDb (int ch) const noexcept
{
    return juce::isPositiveAndBelow (ch, kMaxChannels) ? rms[ch].load() : -200.0f;
}

void LiveAudio::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    rate.store (d->getCurrentSampleRate());
    blockSize = d->getCurrentBufferSizeSamples();
    inputs.store (juce::jlimit (1, kMaxChannels, d->getActiveInputChannels().countNumberOfSetBits()));
    gen.prepare (d->getCurrentSampleRate());
    scratchOut.assign ((size_t) std::max (8192, blockSize * 2), 0.0f);
    const juce::ScopedLock sl (sourceLock);
    if (source != nullptr)
    {
        source->prepareToPlay (blockSize, rate.load());
        sourcePrepared = true;
    }
}

void LiveAudio::audioDeviceStopped()
{
    const juce::ScopedLock sl (sourceLock);
    if (source != nullptr && sourcePrepared) source->releaseResources();
    sourcePrepared = false;
}

void LiveAudio::audioDeviceError (const juce::String&) {}

void LiveAudio::audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                                  float* const* out, int numOut, int n,
                                                  const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);

    // ---- input: meters, FIFO, one-shot capture
    const int ch = std::min (numIn, kMaxChannels);
    for (int c = 0; c < ch; ++c)
    {
        if (in[c] == nullptr) continue;
        float pk = 0.0f;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            pk = std::max (pk, std::abs (in[c][i]));
            sum += (double) in[c][i] * in[c][i];
        }
        const float pkDb = pk > 0.0f ? 20.0f * std::log10 (pk) : -200.0f;
        const float rmsDb = sum > 0.0 ? 10.0f * (float) std::log10 (sum / n) : -200.0f;
        // Peak: instant attack, ~20 dB/s release (the UI polls at 30 Hz).
        const float decay = 20.0f * (float) n / (float) rate.load();
        peak[c].store (std::max (pkDb, peak[c].load() - decay));
        const float prev = rms[c].load();
        rms[c].store (prev < -150.0f ? rmsDb : prev + 0.3f * (rmsDb - prev));
    }

    if (capturing.load() && ch > 0)
    {
        const int want = inputs.load();
        const auto scope = fifo.write (std::min (n, fifo.getFreeSpace()));
        int done = 0;
        const auto put = [&] (int start, int count)
        {
            for (int i = 0; i < count; ++i)
                for (int c = 0; c < want; ++c)
                {
                    const float* src = in[std::min (c, ch - 1)];
                    fifoData[(size_t) (start + i) * want + c] = src != nullptr ? src[done + i] : 0.0f;
                }
            done += count;
        };
        if (scope.blockSize1 > 0) put (scope.startIndex1, scope.blockSize1);
        if (scope.blockSize2 > 0) put (scope.startIndex2, scope.blockSize2);
        if (done < n) dropped.fetch_add (n - done);
    }

    const int wanted = shotWanted.load();
    if (wanted > 0 && ! shotDone.load() && ch > 0 && in[0] != nullptr)
    {
        const int filled = shotFilled.load();
        const int m = std::min (n, wanted - filled);
        std::memcpy (shot.data() + filled, in[0], sizeof (float) * (size_t) m);
        shotFilled.store (filled + m);
        if (filled + m >= wanted) shotDone.store (true);
    }

    // ---- output: file playback, then the generator on top
    {
        const juce::ScopedTryLock sl (sourceLock);
        if (sl.isLocked() && source != nullptr && sourcePrepared && numOut > 0)
        {
            juce::AudioBuffer<float> buf (out, numOut, n);
            juce::AudioSourceChannelInfo info (&buf, 0, n);
            source->getNextAudioBlock (info);
        }
    }
    gen.addTo (out, numOut, n);
}

void LiveAudio::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        d->setAudioPreprocessingEnabled (false);
    if (onDeviceChanged) onDeviceChanged();
}
} // namespace scm
