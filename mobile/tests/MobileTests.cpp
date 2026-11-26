// SpectroCraft Mobile - unit tests for the app's own logic (generator, loop
// recording, calibrator, stored plots, the phone / tablet layout) and for the file round trip through the
// desktop's FileDoc (calibration, stored plots and the video link must come
// back as the desktop reads them).
//
//   build: cmake -S mobile -B <dir> && cmake --build <dir> --target SpectroCraftMobileTests
//   run:   <dir>/SpectroCraftMobileTests_artefacts/<Config>/SpectroCraftMobileTests
#include <JuceHeader.h>
#include "SignalSource.h"
#include "Calibration.h"
#include "StoredPlots.h"
#include "LoopBuffer.h"
#include "FileDoc.h"
#include "FileMeta.h"
#include "LiveAudio.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "ScreenLayout.h"
#include "Telemetry.h"
#include <cmath>
#include <cstring>
#include <thread>

namespace
{
double rmsDb (const std::vector<float>& x, size_t from = 0)
{
    double s = 0.0;
    for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / (double) (x.size() - from));
}

/** Mean power per octave band [f, 2f) from a Welch estimate. */
double octavePowerDb (const std::vector<float>& x, double rate, double f)
{
    const int order = 14, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2);
    std::vector<double> p ((size_t) N / 2 + 1, 0.0);
    int frames = 0;
    for (size_t s = 0; s + (size_t) N <= x.size(); s += (size_t) N / 2, ++frames)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
            buf[(size_t) i] = x[s + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 1; k < N / 2; ++k)
            p[(size_t) k] += (double) buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)]
                           + (double) buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)];
    }
    const double binHz = rate / N;
    double sum = 0.0;
    for (int k = 1; k < N / 2; ++k)
        if (k * binHz >= f && k * binHz < 2.0 * f) sum += p[(size_t) k];
    return 10.0 * std::log10 (sum / std::max (1, frames));
}

std::vector<float> generate (const scm::GeneratorSettings& s, double rate, double seconds)
{
    scm::SignalSource src;
    src.setSettings (s);
    src.prepare (rate);
    std::vector<float> out ((size_t) (rate * seconds));
    src.render (out.data(), (int) out.size());
    return out;
}
} // namespace

class GeneratorTests : public juce::UnitTest
{
public:
    GeneratorTests() : juce::UnitTest ("generator", "SpectroCraftMobile") {}

    void runTest() override
    {
        const double rate = 48000.0;
        beginTest ("tone: RMS level and frequency");
        {
            scm::GeneratorSettings s;
            s.kind = scm::GeneratorSettings::Kind::Tone;
            s.toneHz = 1000.0;
            s.levelDb = -12.0;
            const auto x = generate (s, rate, 2.0);
            expectWithinAbsoluteError (rmsDb (x), -12.0, 0.02);
            int crossings = 0;
            for (size_t i = 1; i < x.size(); ++i)
                if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++crossings;
            expectWithinAbsoluteError ((double) crossings / 2.0, 1000.0, 1.0);
        }

        beginTest ("noise: RMS level and octave slope");
        {
            struct Case { scm::GeneratorSettings::Noise n; double slope; };
            for (auto c : { Case { scm::GeneratorSettings::Noise::White, 0.0 },
                            Case { scm::GeneratorSettings::Noise::Pink, -3.0 },
                            Case { scm::GeneratorSettings::Noise::Brown, -6.0 } })
            {
                scm::GeneratorSettings s;
                s.kind = scm::GeneratorSettings::Kind::Noise;
                s.noise = c.n;
                s.levelDb = -20.0;
                s.lowHz = 20.0;
                s.highHz = 20000.0;
                const auto x = generate (s, rate, 20.0);
                expectWithinAbsoluteError (rmsDb (x, (size_t) rate), -20.0, 0.25);
                const double a = octavePowerDb (x, rate, 250.0);
                const double b = octavePowerDb (x, rate, 2000.0);
                // three octaves apart; per-octave power changes by (slope + 3) dB
                expectWithinAbsoluteError ((b - a) / 3.0, c.slope + 3.0, 0.4,
                                           "octave slope for noise " + juce::String ((int) c.n));
            }
        }

        beginTest ("noise: custom band is respected");
        {
            scm::GeneratorSettings s;
            s.kind = scm::GeneratorSettings::Kind::Noise;
            s.noise = scm::GeneratorSettings::Noise::Custom;
            s.slopeDbPerOctave = 0.0;
            s.lowHz = 1000.0;
            s.highHz = 2000.0;
            s.levelDb = -20.0;
            const auto x = generate (s, rate, 10.0);
            const double inBand = octavePowerDb (x, rate, 1000.0);
            const double below = octavePowerDb (x, rate, 250.0);
            expect (inBand - below > 40.0, "band-limited noise: < -40 dB two octaves below the band");
        }

        beginTest ("sweep: one-shot ends, log sweep reaches the end frequency");
        {
            scm::GeneratorSettings s;
            s.kind = scm::GeneratorSettings::Kind::Sweep;
            s.sweepStartHz = 100.0;
            s.sweepEndHz = 10000.0;
            s.sweepSeconds = 1.0;
            s.sweepRepeat = false;
            scm::SignalSource src;
            src.setSettings (s);
            src.prepare (rate);
            std::vector<float> x ((size_t) (rate * 1.5));
            src.render (x.data(), (int) x.size());
            expect (src.sweepFinished());
            double tail = 0.0;
            for (size_t i = (size_t) (rate * 1.05); i < x.size(); ++i) tail += std::abs (x[i]);
            expectEquals (tail, 0.0);
            // near the end the period is about 1/10 kHz: count crossings over 10 ms
            int crossings = 0;
            const size_t from = (size_t) (rate * 0.985), to = (size_t) (rate * 0.995);
            for (size_t i = from + 1; i < to; ++i)
                if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++crossings;
            expect (crossings > 80 && crossings < 105,
                    "log sweep near 9.5 kHz at the end: " + juce::String (crossings) + " cycles in 10 ms");
        }

        beginTest ("settings: var round trip");
        {
            scm::GeneratorSettings s;
            s.kind = scm::GeneratorSettings::Kind::Noise;
            s.noise = scm::GeneratorSettings::Noise::Custom;
            s.slopeDbPerOctave = -4.5;
            s.levelDb = -18.0;
            const auto t = scm::GeneratorSettings::fromVar (juce::JSON::parse (juce::JSON::toString (s.toVar())));
            expect (t.kind == s.kind && t.noise == s.noise);
            expectEquals (t.slopeDbPerOctave, -4.5);
            expectEquals (t.levelDb, -18.0);
        }
    }
};

class CalibrationTests : public juce::UnitTest
{
public:
    CalibrationTests() : juce::UnitTest ("calibration", "SpectroCraftMobile") {}

    static std::vector<float> tone (double rate, double secs, double hz, double rms, double noiseRms = 1e-4,
                                    double modDepthDb = 0.0)
    {
        std::vector<float> x ((size_t) (rate * secs));
        juce::Random r (7);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / rate;
            const double g = std::pow (10.0, modDepthDb * std::sin (2.0 * juce::MathConstants<double>::pi * 0.5 * t) / 20.0);
            x[i] = (float) (rms * std::sqrt (2.0) * g * std::sin (2.0 * juce::MathConstants<double>::pi * hz * t)
                            + noiseRms * std::sqrt (3.0) * (2.0 * r.nextDouble() - 1.0));
        }
        return x;
    }

    void runTest() override
    {
        const double rate = 48000.0;
        beginTest ("94 dB calibrator: full scale = 1.0024 Pa / rms");
        {
            const double rms = 0.05;
            const auto x = tone (rate, 3.0, 1000.0, rms);
            const auto r = scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate);
            expect (r.ok, r.problem);
            expectWithinAbsoluteError (r.bandRms / rms, 1.0, 0.002);
            expectWithinAbsoluteError (r.fullScalePa, 1.0023722 / rms, 0.03);
            expectWithinAbsoluteError (r.peakHz, 1000.0, 1.0);
            // the resulting calibration reads 94 dB-SPL for this level
            const double readDb = 20.0 * std::log10 (r.bandRms) + scm::cal::fullScaleDbSpl (r.fullScalePa);
            expectWithinAbsoluteError (readDb, 94.0, 0.01);
        }
        beginTest ("calibrator: wrong frequency, clipping, noise and unsteady level are refused");
        {
            auto x = tone (rate, 3.0, 500.0, 0.05);
            expect (! scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate).ok, "500 Hz refused");
            x = tone (rate, 3.0, 1000.0, 0.8);
            expect (! scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate).ok, "clipping refused");
            x = tone (rate, 3.0, 1000.0, 0.01, 0.02);
            expect (! scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate).ok, "noisy refused");
            x = tone (rate, 3.0, 1000.0, 0.05, 1e-4, 1.5);
            expect (! scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate).ok, "unsteady refused");
            x = tone (rate, 0.5, 1000.0, 0.05);
            expect (! scm::cal::analyseCalibrator (x.data(), (int) x.size(), rate).ok, "too short refused");
        }
        beginTest ("dB-SPL <-> Pa");
        {
            expectWithinAbsoluteError (scm::cal::fullScaleDbSpl (1.0), 93.9794, 1e-3);
            expectWithinAbsoluteError (scm::cal::fullScalePaFromDbSpl (120.0), 20.0, 1e-9);
        }
        beginTest ("per-input store");
        {
            juce::PropertySet props;
            scm::cal::Store store (&props);
            scm::cal::InputCalibration c;
            c.input = "USB Mic";
            c.fullScalePa = 12.5;
            c.method = "typed";
            store.set (c);
            expectEquals (store.get ("USB Mic").fullScalePa, 12.5);
            expect (! store.get ("Built-in Microphone").valid());
            const auto s = store.get ("USB Mic").toScale();
            expect (s.unit == ScaleCalibration::Unit::Pascal && s.fullScaleValue == 12.5);
        }
    }
};

class StoredPlotsTests : public juce::UnitTest
{
public:
    StoredPlotsTests() : juce::UnitTest ("stored plots", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("reduce keeps the peak, round trip, 8 slots");
        {
            scm::StoredPlot p;
            p.mode = scm::AnalysisMode::FftLog;
            p.unit = "dB-SPL";
            for (int k = 0; k <= 32768; ++k)
            {
                p.freqs.push_back (k * 48000.0 / 65536.0);
                p.db.push_back (k == 1365 ? 80.0 : 20.0);   // a peak at ~1000 Hz
            }
            scm::StoredPlots plots;
            expect (plots.add (p));
            const auto& q = plots.plots()[0];
            expect ((int) q.freqs.size() <= scm::StoredPlots::kMaxPoints);
            expectEquals (*std::max_element (q.db.begin(), q.db.end()), 80.0);

            scm::StoredPlots back;
            back.fromVar (juce::JSON::parse (juce::JSON::toString (plots.toVar())));
            expectEquals ((int) back.plots().size(), 1);
            expect (back.plots()[0].unit == "dB-SPL" && back.plots()[0].freqs.size() == q.freqs.size());

            for (int i = 1; i < scm::StoredPlots::kMaxPlots; ++i) expect (plots.add (p));
            expect (! plots.add (p), "a ninth plot is refused");
        }
    }
};

class LoopTests : public juce::UnitTest
{
public:
    LoopTests() : juce::UnitTest ("loop recording", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("the rotated window holds the last frames, oldest first");
        for (int total : { 5, 10, 17, 30, 31 })
        {
            scm::LoopPlan plan;
            plan.capacity = 10;
            std::vector<std::vector<float>> doc (1);
            int next = 0;
            while (next < total)
            {
                int64_t n = std::min<int64_t> (3, total - next);
                n = plan.chunkBeforeWrap (n);
                for (int64_t i = 0; i < n; ++i)
                {
                    if (plan.appending()) doc[0].push_back ((float) next);
                    else doc[0][(size_t) plan.writePos()] = (float) next;
                    ++next;
                    ++plan.written;
                }
            }
            scm::rotateChannels (doc, plan.rotationAtStop());
            const int kept = std::min (total, 10);
            expectEquals ((int) doc[0].size(), kept);
            for (int i = 0; i < kept; ++i)
                expectEquals ((int) doc[0][(size_t) i], total - kept + i);
        }
    }
};

class FileRoundTripTests : public juce::UnitTest
{
public:
    FileRoundTripTests() : juce::UnitTest ("file round trip", "SpectroCraftMobile") {}

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("scm-tests-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
        dir.createDirectory();
        const auto wav = dir.getChildFile ("Recording 1.wav");
        const auto movie = dir.getChildFile ("Recording 1.mp4");
        movie.replaceWithText ("not a real movie");

        beginTest ("calibration, stored plots and the video link survive a save and a desktop load");
        {
            FileDoc d;
            std::vector<std::vector<float>> data (1, std::vector<float> (48000, 0.0f));
            for (size_t i = 0; i < data[0].size(); ++i)
                data[0][i] = 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / 48000.0);
            expect (d.engine.initNew (1, 48000.0, &data));
            d.scale.unit = ScaleCalibration::Unit::Pascal;
            d.scale.fullScaleValue = 20.0;
            scm::StoredPlots plots;
            scm::StoredPlot p;
            p.unit = "dB-SPL";
            p.freqs = { 100.0, 1000.0 };
            p.db = { 40.0, 94.0 };
            plots.add (p);
            d.storedPlots = plots.toVar();
            sc::VideoTrackData t;
            t.file = juce::File ("/var/mobile/Containers/Data/Application/X/Documents/Recording 1.mp4");
            t.relPath = "Recording 1.mp4";
            t.offsetSecs = 0.25;
            d.videoTracks = { t };
            expect (d.engine.saveFileAs (wav), "save");

            FileDoc e;
            expect (e.load (wav), "load");
            expect (e.scale.unit == ScaleCalibration::Unit::Pascal);
            expectWithinAbsoluteError (e.scale.fullScaleValue, 20.0, 1e-9);
            scm::StoredPlots back;
            back.fromVar (e.storedPlots);
            expectEquals ((int) back.plots().size(), 1);
            expectEquals ((int) e.videoTracks.size(), 1);
            if (! e.videoTracks.empty())
            {
                expect (e.videoTracks[0].file == movie,
                        "video resolved next to the WAV: " + e.videoTracks[0].file.getFullPathName());
                expectWithinAbsoluteError (e.videoTracks[0].offsetSecs, 0.25, 1e-9);
            }

            // A desktop re-save keeps the plots and writes the relative path.
            expect (e.engine.saveFile (wav), "re-save");
            FileDoc f;
            expect (f.load (wav), "reload");
            scm::StoredPlots again;
            again.fromVar (f.storedPlots);
            expectEquals ((int) again.plots().size(), 1);
            expect (! f.videoTracks.empty() && f.videoTracks[0].relPath == "Recording 1.mp4");
        }

        beginTest ("a bare relative \"file\" is resolved against the audio folder");
        {
            sc::MetaBundle b;
            expect (sc::parseBundle (R"({"videoTracks":[{"file":"Recording 1.mp4","offset":1.0}]})", b));
            expectEquals ((int) b.videoTracks.size(), 1);
            sc::resolveVideoTracks (b.videoTracks, dir);
            expect (! b.videoTracks.empty() && b.videoTracks[0].file == movie);
        }

        dir.deleteRecursively();
    }
};


class RecordingPathTests : public juce::UnitTest
{
public:
    RecordingPathTests() : juce::UnitTest ("recording path", "SpectroCraftMobile") {}

    /** Feeds `seconds` of input through the device callback (blocks of 512),
        the value of each frame given by `signal (frameIndex)`, polling the
        recorder as the app's timer would. Returns the generator's output. */
    static std::vector<float> feed (scm::LiveAudio& audio, scm::Recorder& rec, double seconds,
                                    int64_t& frame, const std::function<float (int64_t)>& signal)
    {
        const int block = 512;
        std::vector<float> in ((size_t) block), outL ((size_t) block), outR ((size_t) block), played;
        const int blocks = (int) std::ceil (seconds * 48000.0 / block);
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i) in[(size_t) i] = signal (frame + i);
            frame += block;
            const float* ins[] = { in.data() };
            float* outs[] = { outL.data(), outR.data() };
            audio.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, block, {});
            played.insert (played.end(), outL.begin(), outL.end());
            if (b % 8 == 7) rec.poll();
        }
        rec.poll();
        return played;
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("scm-rec-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
        dir.createDirectory();
        setenv ("SC_MOBILE_RECORDINGS", dir.getFullPathName().toRawUTF8(), 1);

        scm::LiveAudio audio;
        audio.simulateDevice (48000.0, 1);
        scm::MobileDocument doc;
        scm::Recorder rec (audio, doc);

        beginTest ("unlimited: every frame reaches the saved WAV, with the calibration");
        {
            ScaleCalibration cal;
            cal.unit = ScaleCalibration::Unit::Pascal;
            cal.fullScaleValue = 31.7;
            juce::String err;
            rec.setMode (scm::Recorder::Mode::Unlimited, 0.0);
            expect (rec.start (cal, err), err);
            int64_t frame = 0;
            feed (audio, rec, 2.0, frame, [] (int64_t i) { return 0.25f * (float) std::sin (0.1309 * (double) i); });
            const auto saved = rec.stop (err);
            expect (saved.existsAsFile(), "saved: " + err);
            FileDoc back;
            expect (back.load (saved));
            expectEquals ((int64_t) back.engine.numSamples(), frame);
            expect (back.scale.unit == ScaleCalibration::Unit::Pascal);
            expectWithinAbsoluteError (back.scale.fullScaleValue, 31.7, 1e-9);
            float x = 0.0f;
            back.engine.readSamples (0, 1000, 1, &x);
            expectWithinAbsoluteError (x, 0.25f * (float) std::sin (0.1309 * 1000.0), 1e-6f);
            expect (rec.fileStartTime() == rec.firstFrameTime(), "unlimited: the file starts with the take");
        }

        beginTest ("loop: the file keeps the last N seconds, oldest first");
        {
            juce::String err;
            rec.setMode (scm::Recorder::Mode::Loop, 1.0);
            expect (rec.start ({}, err), err);
            int64_t frame = 0;
            // each frame carries its own index (scaled), so the order can be checked
            feed (audio, rec, 2.5, frame, [] (int64_t i) { return (float) i / 1.0e6f; });
            expect (rec.wrapped());
            const auto saved = rec.stop (err);
            FileDoc back;
            expect (back.load (saved));
            expectEquals ((int64_t) back.engine.numSamples(), (int64_t) 48000);
            std::vector<float> v (48000);
            back.engine.readSamples (0, 0, 48000, v.data());
            const int64_t first = frame - 48000;
            bool ordered = true;
            for (int64_t i = 0; i < 48000; ++i)
                if (std::abs (v[(size_t) i] - (float) (first + i) / 1.0e6f) > 1e-6f) { ordered = false; break; }
            expect (ordered, "frames " + juce::String (first) + ".." + juce::String (frame - 1) + " in order");
            // a video filmed with the take lines up with the file's first frame, not the take's
            expectWithinAbsoluteError ((double) (rec.fileStartTime().toMilliseconds() - rec.firstFrameTime().toMilliseconds()),
                                       (double) first / 48.0, 1.0);
        }

        beginTest ("the generator plays while the input records (full duplex)");
        {
            scm::GeneratorSettings g;
            g.kind = scm::GeneratorSettings::Kind::Tone;
            g.toneHz = 1000.0;
            g.levelDb = -20.0;
            audio.generator().setSettings (g);
            audio.generator().setEnabled (true);
            juce::String err;
            rec.setMode (scm::Recorder::Mode::Unlimited, 0.0);
            expect (rec.start ({}, err), err);
            int64_t frame = 0;
            const auto played = feed (audio, rec, 1.0, frame, [] (int64_t) { return 0.01f; });
            const auto saved = rec.stop (err);
            audio.generator().setEnabled (false);
            expectWithinAbsoluteError (rmsDb (played, 4800), -20.0, 0.05);
            FileDoc back;
            expect (back.load (saved));
            expectEquals ((int64_t) back.engine.numSamples(), frame);
        }

        beginTest ("calibrator capture through the device callback");
        {
            audio.startShot (2.0);
            int64_t frame = 0;
            const double rms = 0.02;
            feed (audio, rec, 2.1, frame, [rms] (int64_t i)
                  { return (float) (rms * std::sqrt (2.0) * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / 48000.0)); });
            expect (audio.shotReady());
            const auto shot = audio.takeShot();
            expectEquals ((int) shot.size(), 96000);
            const auto r = scm::cal::analyseCalibrator (shot.data(), (int) shot.size(), 48000.0);
            expect (r.ok, r.problem);
            expectWithinAbsoluteError (scm::cal::fullScaleDbSpl (r.fullScalePa), 94.0 - 20.0 * std::log10 (rms), 0.02);
        }

        unsetenv ("SC_MOBILE_RECORDINGS");
        dir.deleteRecursively();
    }
};


class TelemetryTests : public juce::UnitTest
{
public:
    TelemetryTests() : juce::UnitTest ("telemetry", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("PostHog batch: api key, event, anonymous id, no person profile");
        {
            auto* props = new juce::DynamicObject();
            props->setProperty ("mode", "loop");
            props->setProperty ("loop_s", 30);
            const auto ev = scm::telemetry::makeEvent ("recording_started", juce::var (props), "id-123",
                                                       juce::Time (2026, 10, 24, 18, 30, 0, 0, false));
            const auto body = juce::JSON::parse (scm::telemetry::buildBatchJson ("phc_test", { ev }));
            expectEquals (body.getProperty ("api_key", {}).toString(), juce::String ("phc_test"));
            const auto* batch = body.getProperty ("batch", {}).getArray();
            expect (batch != nullptr && batch->size() == 1);
            if (batch != nullptr && batch->size() == 1)
            {
                const auto e = (*batch)[0];
                expectEquals (e.getProperty ("event", {}).toString(), juce::String ("recording_started"));
                expectEquals (juce::Time::fromISO8601 (e.getProperty ("timestamp", {}).toString()).toMilliseconds(),
                              juce::Time (2026, 10, 24, 18, 30, 0, 0, false).toMilliseconds());
                const auto p = e.getProperty ("properties", {});
                expectEquals (p.getProperty ("distinct_id", {}).toString(), juce::String ("id-123"));
                expect (! (bool) p.getProperty ("$process_person_profile", true));
                expectEquals (p.getProperty ("mode", {}).toString(), juce::String ("loop"));
                expectEquals ((int) p.getProperty ("loop_s", 0), 30);
                expect (p.getProperty ("$os", {}).toString().isNotEmpty());
                expect (p.getProperty ("$app_version", {}).toString().isNotEmpty());
            }
        }

        beginTest ("the batch reaches an HTTP server as a JSON POST to /batch/");
        {
            // A one-shot local server standing in for PostHog.
            juce::StreamingSocket server;
            int port = 0;
            for (int p = 47311; p < 47330 && port == 0; ++p)
                if (server.createListener (p, "127.0.0.1")) port = p;
            expect (port != 0, "local listener");
            juce::String request;
            std::thread serve ([&server, &request]
            {
                std::unique_ptr<juce::StreamingSocket> c (server.waitForNextConnection());
                if (c == nullptr) return;
                char buf[8192];
                for (int tries = 0; tries < 50; ++tries)
                {
                    if (c->waitUntilReady (true, 200) <= 0) continue;
                    const int n = c->read (buf, (int) sizeof (buf), false);
                    if (n <= 0) break;
                    request += juce::String::fromUTF8 (buf, n);
                    const int headerEnd = request.indexOf ("\r\n\r\n");
                    if (headerEnd > 0)
                    {
                        const int len = request.fromFirstOccurrenceOf ("Content-Length:", false, true)
                                               .upToFirstOccurrenceOf ("\r\n", false, false).trim().getIntValue();
                        if (request.getNumBytesAsUTF8() >= (size_t) (headerEnd + 4 + len)) break;
                    }
                }
                const char* reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\nConnection: close\r\n\r\n{\"status\":1}";
                c->write (reply, (int) std::strlen (reply));
            });
            const auto ev = scm::telemetry::makeEvent ("app_opened", {}, "id-9", juce::Time::getCurrentTime());
            const bool ok = port != 0 && scm::telemetry::postBatch ("http://127.0.0.1:" + juce::String (port), "phc_local", { ev });
            serve.join();
            server.close();
            expect (ok, "2xx answer");
            expect (request.startsWith ("POST /batch/"), request.substring (0, 40));
            expect (request.containsIgnoreCase ("Content-Type: application/json"));
            const auto body = juce::JSON::parse (request.fromFirstOccurrenceOf ("\r\n\r\n", false, false));
            expectEquals (body.getProperty ("api_key", {}).toString(), juce::String ("phc_local"));
        }

        beginTest ("a build without keys sends nothing and cannot be switched on");
        {
            expect (! scm::telemetry::available());
            juce::PropertiesFile::Options o;
            o.applicationName = "scm-telemetry-test";
            o.filenameSuffix = ".settings";
            o.folderName = "scm-telemetry-test";
            o.osxLibrarySubFolder = "Application Support";
            juce::PropertiesFile props (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                            .getChildFile ("scm-telemetry-test.settings"), o);
            scm::telemetry::initialise (props);
            expect (! scm::telemetry::enabled());
            scm::telemetry::setEnabled (true);
            expect (! scm::telemetry::enabled());
            scm::telemetry::capture ("x", { { "a", 1 } });   // no sender: a no-op
            scm::telemetry::reportError ("y");
            expect (props.getValue ("telemetryId").length() >= 32, "an install id is still kept");
            scm::telemetry::shutdown();
            props.getFile().deleteFile();
        }
    }
};


class PlaybackEndTests : public juce::UnitTest
{
public:
    PlaybackEndTests() : juce::UnitTest ("playback end", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("playback stops at the end of the file (with the screen's 30 Hz playhead sync)");
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("scm-play-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
        dir.createDirectory();
        const auto wav = dir.getChildFile ("tone.wav");
        {
            FileDoc d;
            std::vector<std::vector<float>> data (1, std::vector<float> (48000, 0.5f));
            d.engine.initNew (1, 48000.0, &data);
            d.engine.saveFileAs (wav);
        }
        scm::LiveAudio audio;
        audio.simulateDevice (48000.0, 1);
        scm::MobileDocument doc;
        juce::String err;
        expect (doc.open (wav, err), err);
        auto& eng = doc.doc()->engine;
        audio.setPlaybackSource (&eng);
        eng.play();
        const int block = 512;
        std::vector<float> in ((size_t) block, 0.0f), outL ((size_t) block), outR ((size_t) block);
        int nonSilentAfterEnd = 0, framesAfterEnd = 0;
        int64_t frame = 0;
        for (int b = 0; b < 300; ++b)   // ~3.2 s for a 1 s file
        {
            const float* ins[] = { in.data() };
            float* outs[] = { outL.data(), outR.data() };
            audio.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, block, {});
            frame += block;
            if (frame > 48000 + 4 * block)
            {
                framesAfterEnd += block;
                for (float v : outL) if (std::abs (v) > 1e-6f) { ++nonSilentAfterEnd; break; }
            }
            if (b % 3 == 0)   // what MainScreen::timerCallback does
                doc.followPlayback (eng.position());
        }
        expect (! eng.isPlaying(), "stopped after the end");
        expectWithinAbsoluteError (doc.playhead(), 1.0, 0.02, "the playhead stays at the end");
        expectEquals (nonSilentAfterEnd, 0, "no sound after the end (no loop)");
        audio.setPlaybackSource (nullptr);
        dir.deleteRecursively();
    }
};


class CropTests : public juce::UnitTest
{
public:
    CropTests() : juce::UnitTest ("crop", "SpectroCraftMobile") {}

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("scm-crop-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
        dir.createDirectory();
        setenv ("SC_MOBILE_RECORDINGS", dir.getFullPathName().toRawUTF8(), 1);
        const auto wav = dir.getChildFile ("Take.wav");
        {
            FileDoc d;
            std::vector<std::vector<float>> data (1, std::vector<float> (48000 * 4));
            for (size_t i = 0; i < data[0].size(); ++i) data[0][i] = (float) i / 1.0e6f;   // each sample = its index
            d.engine.initNew (1, 48000.0, &data);
            d.scale.unit = ScaleCalibration::Unit::Pascal;
            d.scale.fullScaleValue = 12.0;
            d.engine.saveFileAs (wav);
        }
        scm::MobileDocument doc;
        juce::String err;
        expect (doc.open (wav, err), err);

        beginTest ("crop to a new recording keeps the selection and the calibration");
        {
            doc.setSelection ({ 1.0, 2.5 });
            const auto f = doc.cropToNewFile (err);
            expect (f.existsAsFile(), err);
            expectEquals (f.getFileName(), juce::String ("Take crop.wav"));
            FileDoc c;
            expect (c.load (f));
            expectEquals ((int64_t) c.engine.numSamples(), (int64_t) 72000);
            float first = 0.0f;
            c.engine.readSamples (0, 0, 1, &first);
            expectWithinAbsoluteError (first, 48000.0f / 1.0e6f, 1e-6f);
            expect (c.scale.unit == ScaleCalibration::Unit::Pascal && c.scale.fullScaleValue == 12.0);
            expectWithinAbsoluteError (doc.duration(), 4.0, 1e-6, "the open file is unchanged");
        }

        beginTest ("crop in place keeps the selection and can be undone");
        {
            doc.setSelection ({ 1.0, 2.5 });
            expect (doc.cropToSelection (err), err);
            expectWithinAbsoluteError (doc.duration(), 1.5, 1e-6);
            float first = 0.0f;
            doc.doc()->engine.readSamples (0, 0, 1, &first);
            expectWithinAbsoluteError (first, 48000.0f / 1.0e6f, 1e-6f);
            expect (doc.undo());
            expectWithinAbsoluteError (doc.duration(), 4.0, 1e-6, "undo restores the whole take");
        }

        unsetenv ("SC_MOBILE_RECORDINGS");
        dir.deleteRecursively();
    }
};

class ScreenLayoutTests : public juce::UnitTest
{
public:
    ScreenLayoutTests() : juce::UnitTest ("screen layout", "SpectroCraftMobile") {}

    using Form = scm::screen::Form;
    using Rect = juce::Rectangle<int>;

    /** Every area inside the safe area, none overlapping another, the
        minimum sizes kept, and the areas cover the safe area. */
    void checkFrame (Rect safe, Form form, const scm::screen::TabletState& st, const juce::String& what)
    {
        namespace s = scm::screen;
        const auto f = s::tabletFrame (safe, form, st);
        const std::vector<std::pair<const char*, Rect>> top {
            { "top bar", f.topBar }, { "views", f.views }, { "transport", f.transport },
            { "panels", f.panels }, { "panels splitter", f.panelsSplitter } };
        int area = 0;
        for (size_t i = 0; i < top.size(); ++i)
        {
            expect (safe.contains (top[i].second) || top[i].second.isEmpty(), what + ": " + top[i].first + " inside");
            area += top[i].second.getWidth() * top[i].second.getHeight();
            for (size_t j = i + 1; j < top.size(); ++j)
                expect (! top[i].second.intersects (top[j].second),
                        what + ": " + top[i].first + " / " + top[j].first + " overlap");
        }
        expectEquals (area, safe.getWidth() * safe.getHeight(), what + ": the areas cover the screen");
        expect (f.views.getWidth() >= s::kMinViewsWidth && f.views.getHeight() >= s::kMinViewsHeight / 2,
                what + ": room for the views " + f.views.toString());
        if (! st.panelsVisible)
        {
            expect (f.panels.isEmpty() && f.panelsSplitter.isEmpty() && f.groupSplitter.isEmpty(), what + ": no panels");
            return;
        }
        for (int g = 0; g < 2; ++g)
        {
            expect (f.panels.contains (f.tabs[g]) && f.panels.contains (f.panel[g]), what + ": group inside the panels");
            expectEquals (f.tabs[g].getHeight(), scm::style::kTouch, what + ": tab strip height");
            expect (f.panel[g].getHeight() >= s::kMinPanelHeight * 3 / 4, what + ": panel height " + f.panel[g].toString());
            expect (f.panel[g].getWidth() >= s::kMinPanelWidth * 3 / 4, what + ": panel width " + f.panel[g].toString());
            expect (! f.panel[g].intersects (f.groupSplitter) && ! f.tabs[g].intersects (f.groupSplitter),
                    what + ": group splitter apart");
        }
        expect (! f.panel[0].intersects (f.panel[1]) && ! f.tabs[0].intersects (f.panel[1]), what + ": groups apart");
    }

    void runTest() override
    {
        namespace s = scm::screen;

        beginTest ("the form follows the smaller side: phones in both orientations, tablets from 600 points");
        {
            expect (s::formFor ({ 0, 0, 390, 844 }) == Form::Phone);
            expect (s::formFor ({ 0, 0, 932, 430 }) == Form::Phone, "a large phone in landscape");
            expect (s::formFor ({ 0, 0, 599, 960 }) == Form::Phone);
            expect (s::formFor ({ 0, 0, 600, 960 }) == Form::TabletPortrait);
            expect (s::formFor ({ 0, 0, 744, 1133 }) == Form::TabletPortrait, "iPad mini");
            expect (s::formFor ({ 0, 0, 1180, 820 }) == Form::TabletLandscape, "iPad");
            expect (s::formFor ({ 0, 0, 1376, 1032 }) == Form::TabletLandscape, "iPad Pro 13");
            expect (s::formFor ({}) == Form::Phone, "before the first layout");
        }

        beginTest ("tablet frames: inside the safe area, no overlap, minimum sizes, at any splitter position");
        {
            const std::vector<Rect> screens {
                { 0, 24, 1180, 776 },    // iPad landscape, status bar and home indicator out
                { 0, 24, 820, 1136 },    // iPad portrait
                { 0, 24, 744, 1089 },    // iPad mini portrait
                { 0, 24, 1133, 700 },    // iPad mini landscape
                { 0, 24, 1376, 988 },    // iPad Pro 13 landscape
                { 0, 0, 960, 528 },      // small Android tablet landscape, bars out
                { 0, 0, 600, 912 } };    // small Android tablet portrait
            for (const auto& sa : screens)
                for (double fraction : { 0.0, 0.34, 0.5, 1.0 })
                    for (bool visible : { true, false })
                    {
                        s::TabletState st;
                        st.landscapeColumn = st.landscapeSplit = st.portraitPanels = st.portraitSplit = fraction;
                        st.panelsVisible = visible;
                        const auto form = sa.getWidth() > sa.getHeight() ? Form::TabletLandscape : Form::TabletPortrait;
                        checkFrame (sa, form, st, sa.toString() + " fraction " + juce::String (fraction)
                                                    + (visible ? "" : " hidden"));
                    }
        }

        beginTest ("landscape: the column on the left, the groups stacked; portrait: the groups side by side below");
        {
            s::TabletState st;
            const auto l = s::tabletFrame ({ 0, 0, 1180, 800 }, Form::TabletLandscape, st);
            expect (l.panels.getX() == 0 && l.views.getX() > l.panels.getRight(), "column left of the views");
            expect (l.tabs[1].getY() > l.panel[0].getBottom(), "group 1 under group 0");
            expect (l.transport.getWidth() == 1180 && l.transportSingleRow, "transport along the bottom, one row");
            const auto p = s::tabletFrame ({ 0, 0, 820, 1160 }, Form::TabletPortrait, st);
            expect (p.panels.getY() > p.views.getBottom() && p.transport.getY() >= p.views.getBottom()
                        && p.transport.getBottom() <= p.panels.getY(), "views, transport, then the panels");
            expect (p.tabs[1].getX() > p.panel[0].getRight(), "group 1 right of group 0");
            expect (! p.transportSingleRow, "820 points: the phone's two transport rows");
        }

        beginTest ("a splitter drag moves the bar to the finger, within the limits");
        {
            const Rect sa { 0, 0, 1180, 800 };
            s::TabletState st;
            s::dragPanelsSplitter (st, sa, Form::TabletLandscape, 500);
            auto f = s::tabletFrame (sa, Form::TabletLandscape, st);
            expectEquals (f.panelsSplitter.getCentreX(), 500);
            s::dragPanelsSplitter (st, sa, Form::TabletLandscape, 5000);
            f = s::tabletFrame (sa, Form::TabletLandscape, st);
            expectEquals (f.views.getWidth(), s::kMinViewsWidth, "the views keep their minimum");
            s::dragPanelsSplitter (st, sa, Form::TabletLandscape, -100);
            f = s::tabletFrame (sa, Form::TabletLandscape, st);
            expectEquals (f.panels.getWidth(), s::kMinPanelWidth, "the column keeps its minimum");

            s::dragGroupSplitter (st, sa, Form::TabletLandscape, 300);
            f = s::tabletFrame (sa, Form::TabletLandscape, st);
            expectEquals (f.groupSplitter.getCentreY(), 300);

            const Rect sp { 0, 0, 820, 1160 };
            s::dragPanelsSplitter (st, sp, Form::TabletPortrait, 700);
            f = s::tabletFrame (sp, Form::TabletPortrait, st);
            expectEquals (f.panelsSplitter.getCentreY(), 700);
            s::dragGroupSplitter (st, sp, Form::TabletPortrait, 420);
            f = s::tabletFrame (sp, Form::TabletPortrait, st);
            expectEquals (f.groupSplitter.getCentreX(), 420);
        }

        beginTest ("tabs: two groups as on the desktop; a tap shows the tab in its group and brings the panels back");
        {
            for (int t = 0; t < s::NumTabs; ++t)
            {
                const auto& g = s::tabsOfGroup (s::groupOf (t));
                expect (std::find (g.begin(), g.end(), t) != g.end());
            }
            expectEquals ((int) (s::tabsOfGroup (0).size() + s::tabsOfGroup (1).size()), (int) s::NumTabs);
            s::TabletState st;
            st.panelsVisible = false;
            s::tapTab (st, s::Generator);
            expect (st.panelsVisible && st.tab[1] == s::Generator && st.tab[0] == s::Analysis);
            s::tapTab (st, s::Files);
            expect (st.tab[0] == s::Files && st.tab[1] == s::Generator, "the other group keeps its panel");
        }

        beginTest ("hardware keyboard: the desktop's shortcuts");
        {
            using K = juce::KeyPress;
            using C = s::Command;
            const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
            expect (s::commandForKey (K (K::spaceKey)) == C::PlayPause);
            expect (s::commandForKey (K ('.', cmd, 0)) == C::Stop);
            expect (s::commandForKey (K (K::F10Key, shift, 0)) == C::Record);
            expect (s::commandForKey (K ('z', cmd, 0)) == C::Undo);
            expect (s::commandForKey (K ('Z', cmd, 0)) == C::Undo, "letters in either case");
            expect (s::commandForKey (K ('y', cmd, 0)) == C::Redo);
            expect (s::commandForKey (K ('z', cmd | shift, 'Z')) == C::Redo);
            expect (s::commandForKey (K ('s', cmd, 0)) == C::Save);
            expect (s::commandForKey (K ('a', cmd, 0)) == C::SelectAll);
            expect (s::commandForKey (K (K::escapeKey)) == C::ClearSelection);
            expect (s::commandForKey (K (K::backspaceKey)) == C::Delete);
            expect (s::commandForKey (K (K::deleteKey)) == C::Delete);
            expect (s::commandForKey (K (K::homeKey)) == C::GoToStart);
            expect (s::commandForKey (K (K::endKey)) == C::GoToEnd);
            expect (s::commandForKey (K ('=', cmd, 0)) == C::ZoomIn);
            expect (s::commandForKey (K ('-', cmd, 0)) == C::ZoomOut);
            expect (s::commandForKey (K ('0', cmd, 0)) == C::ZoomAll);
            expect (s::commandForKey (K (K::returnKey, cmd | shift, 0)) == C::TogglePanels);
            expect (s::commandForKey (K ('z', 0, 'z')) == C::None, "a plain letter does nothing");
            expect (s::commandForKey (K (K::spaceKey, shift, 0)) == C::None);
            expect (s::commandForKey (K ('s', cmd | juce::ModifierKeys::altModifier, 0)) == C::None);
        }
    }
};

static GeneratorTests generatorTests;
static CalibrationTests calibrationTests;
static StoredPlotsTests storedPlotsTests;
static LoopTests loopTests;
static FileRoundTripTests fileRoundTripTests;
static RecordingPathTests recordingPathTests;
static TelemetryTests telemetryTests;
static PlaybackEndTests playbackEndTests;
static CropTests cropTests;
static ScreenLayoutTests screenLayoutTests;

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("SpectroCraftMobile");
    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }
    std::printf ("%d checks passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
