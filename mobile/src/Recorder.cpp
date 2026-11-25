#include "Recorder.h"
#include "LiveAudio.h"
#include "MobileDocument.h"

namespace scm
{
Recorder::Recorder (LiveAudio& a, MobileDocument& d) : audio (a), document (d)
{
    chunk.resize ((size_t) 16384 * 2);
}

Recorder::~Recorder()
{
    stopTimer();
}

void Recorder::setMode (Mode m, double seconds)
{
    recMode = m;
    loopSecs = juce::jlimit (1.0, 3600.0, seconds);
}

double Recorder::maxSeconds() const
{
    return rate > 0.0 && channels > 0 ? (double) capFrames / rate : 0.0;
}

double Recorder::elapsed() const
{
    return rate > 0.0 ? (double) plan.written / rate : 0.0;
}

double Recorder::writeHeadSeconds() const
{
    return rate > 0.0 ? (double) plan.writePos() / rate : 0.0;
}

bool Recorder::start (const ScaleCalibration& cal, juce::String& error)
{
    if (recording) return true;
    if (! audio.isOpen()) { error = "The microphone is not available."; return false; }
    channels = std::max (1, audio.numInputs());
    rate = audio.sampleRate();
    if (! document.startNew (channels, rate)) { error = "Could not start a recording."; return false; }
    if (cal.isCalibrated()) document.setCalibration (cal);

    // Memory cap: a quarter of the device's RAM, at most 1.5 GB of samples.
    const int64_t ramBytes = (int64_t) juce::SystemStats::getMemorySizeInMegabytes() << 20;
    const int64_t budget = std::clamp<int64_t> (ramBytes / 4, (int64_t) 128 << 20, (int64_t) 1536 << 20);
    capFrames = budget / ((int64_t) sizeof (float) * channels);

    plan = {};
    plan.capacity = recMode == Mode::Loop ? std::min<int64_t> ((int64_t) (loopSecs * rate), capFrames) : 0;
    firstFrameWall = {};
    target = MobileDocument::newRecordingFile();
    recording = true;
    audio.setCapturing (true);
    startTimerHz (30);
    return true;
}

void Recorder::pull()
{
    auto* d = document.doc();
    if (d == nullptr) return;
    auto& eng = d->engine;
    const int maxFrames = (int) (chunk.size() / (size_t) channels);
    for (;;)
    {
        int n = audio.readCaptured (chunk.data(), maxFrames);
        if (n <= 0) break;
        if (firstFrameWall.toMilliseconds() == 0)   // the oldest frame in the FIFO arrived n frames ago
            firstFrameWall = juce::Time (juce::Time::currentTimeMillis() - (juce::int64) (1000.0 * n / rate));
        int off = 0;
        while (off < n)
        {
            if (plan.unlimited() && plan.written >= capFrames)
            {
                recording = false;   // keep what we have
                return;
            }
            int64_t m = plan.chunkBeforeWrap (n - off);
            if (plan.unlimited()) m = std::min<int64_t> (m, capFrames - plan.written);
            const float* src = chunk.data() + (size_t) off * channels;
            const bool ok = plan.appending() ? eng.appendSamples (src, m)
                                             : eng.overwriteSamples (src, plan.writePos(), m);
            if (! ok) { recording = false; return; }
            plan.written += m;
            off += (int) m;
        }
    }
}

void Recorder::timerCallback()
{
    if (! recording) return;
    pull();
    if (! recording)   // stopped inside pull(): memory full or the engine refused
    {
        juce::String err;
        stop (err);
        if (onAutoStop)
            onAutoStop ("The recording reached the memory limit ("
                        + juce::String (maxSeconds() / 60.0, 1) + " min) and was saved.");
        return;
    }
    if (onUpdate) onUpdate();
}

juce::File Recorder::stop (juce::String& error, std::function<void()> beforeSave)
{
    stopTimer();
    if (! recording && plan.written == 0)
    {
        audio.setCapturing (false);
        return {};
    }
    pull();
    audio.setCapturing (false);
    recording = false;

    auto* d = document.doc();
    if (d == nullptr) return {};
    auto& eng = d->engine;

    // A wrapped loop: rotate so the file starts with the oldest frame.
    if (const int64_t k = plan.rotationAtStop(); k > 0)
    {
        const int64_t len = eng.numSamples();
        std::vector<std::vector<float>> data ((size_t) eng.numChannels(), std::vector<float> ((size_t) len));
        for (int c = 0; c < eng.numChannels(); ++c)
            eng.readSamples (c, 0, len, data[(size_t) c].data());
        rotateChannels (data, k);
        juce::AudioBuffer<float> buf (eng.numChannels(), (int) len);
        for (int c = 0; c < eng.numChannels(); ++c)
            buf.copyFrom (c, 0, data[(size_t) c].data(), (int) len);
        std::string err;
        eng.replaceWholeFile (buf, eng.sampleRate(), &err);
    }
    eng.rebuildMipmaps();
    d->undo.clear();   // the take is the starting point of the history
    document.spectral().clearCache();

    if (beforeSave) beforeSave();
    juce::File saved;
    if (eng.numSamples() > 0)
    {
        const auto file = target != juce::File() ? target : MobileDocument::newRecordingFile();
        if (document.saveAs (file, error)) saved = file;
    }
    document.changed();
    return saved;
}
} // namespace scm
