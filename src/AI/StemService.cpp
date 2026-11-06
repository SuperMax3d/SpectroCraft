// SpectroCraft - headless stem separation (audio.cpp CLI orchestration).
// Extracted from StemSeparationWindow::runWorker: same steps, same messages,
// no UI. See include/AI/StemService.h.
#include "AI/StemService.h"
#include "PythonScripts.h"   // sc::py::readFloatWav / writeFloatWav
#include "Resampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace sc {

namespace {

// Deletes a temporary working folder on destruction (worker-thread safe);
// a caller-provided folder is kept.
struct WorkDir
{
    juce::File dir;
    bool temporary = false;
    ~WorkDir() { if (temporary && dir.getFullPathName ().isNotEmpty ()) dir.deleteRecursively (); }
};

juce::String logTail (const juce::String& s, int maxChars = 2000)
{
    if (s.length () <= maxChars) return s;
    return "..." + s.substring (s.length () - maxChars, s.length ());
}

// The CLI reports errors on stdout as well as stderr, so fall back to the
// output tail when stderr is empty.
juce::String failureDetail (const sc::acpp::RunResult& rr, const std::string& err)
{
    return rr.stderrText.isNotEmpty () ? logTail (rr.stderrText)
         : rr.stdoutText.isNotEmpty () ? logTail (rr.stdoutText)
                                       : juce::String (err);
}

bool resampleTrack (std::vector<float>& t, double fromRate, double toRate)
{
    juce::AudioBuffer<float> in (1, (int) t.size ());
    std::copy (t.begin (), t.end (), in.getWritePointer (0));
    juce::AudioBuffer<float> out (1, (int) sc::Resampler::estimateOutputFrames (
        (int64_t) t.size (), fromRate, toRate));
    if (!sc::Resampler::resample (in, fromRate, toRate, out))
        return false;
    t.assign (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples ());
    return true;
}

} // namespace

bool StemService::supported()
{
#if JUCE_WINDOWS
    return true;
#else
    return false;
#endif
}

std::vector<sc::acpp::Anchor> StemService::anchorsRelativeToInput (
    const std::vector<sc::acpp::Anchor>& absAnchors, double extStartSecs, double inputDurSecs)
{
    std::vector<sc::acpp::Anchor> out;
    for (const auto& a : absAnchors)
    {
        const double s = std::max (0.0, a.startSecs - extStartSecs);
        const double e = std::min (inputDurSecs, a.stopSecs - extStartSecs);
        if (e > s) out.push_back ({ a.include, s, e });
    }
    return out;
}

juce::String StemService::validate (const Request& req, juce::String* codeOut)
{
    auto bad = [codeOut] (const char* code, const juce::String& msg)
    {
        if (codeOut != nullptr) *codeOut = code;
        return msg;
    };
    if (codeOut != nullptr) codeOut->clear ();

    const int m = (int) req.mode;
    if (m < 1 || m > 3)
        return bad ("invalid_mode", "Unknown stem separation mode " + juce::String (m) + ".");

    if (req.mode == Mode::SamAudio)
    {
        if (req.descriptions.empty ())
            return bad ("no_description", "Enter a description first.");
        for (const auto& d : req.descriptions)
            if (d.text.trim ().isEmpty ())
                return bad ("no_description", "Every description needs some text.");
    }

    if (req.backend != "cpu" && req.backend != "cuda" && req.backend != "vulkan")
        return bad ("invalid_backend", "Unknown backend '" + req.backend + "' (expected cpu, cuda or vulkan).");

    if (!std::isfinite (req.sourceRate) || req.sourceRate <= 0.0)
        return bad ("invalid_rate", "The source sample rate is invalid.");
    if (!std::isfinite (req.extStartSecs) || req.extStartSecs < 0.0)
        return bad ("invalid_range", "The extraction start time is invalid.");

    if (req.input.empty () || req.input[0].empty ())
        return bad ("empty_input", "The selected range is empty.");
    for (const auto& c : req.input)
        if (c.size () != req.input[0].size ())
            return bad ("ragged_input", "The input channels do not have the same length.");

    const juce::File bin = req.binary == juce::File () ? sc::acpp::binaryPath () : req.binary;
    if (!bin.existsAsFile ())
        return bad ("binary_missing", "The audiocpp_cli.exe binary is missing.");

    if (req.mode == Mode::HtDemucsFt)
    {
        if (!req.model.isDirectory ())
            return bad ("model_missing", "The htdemucs_ft model folder is not installed - create it with at least one .gguf file.");
    }
    else if (!req.model.existsAsFile ())
    {
        return bad ("model_missing", req.mode == Mode::HtDemucs6
            ? "The htdemucs_6stems model is not installed - place a .gguf file in the HTDemucs-6stems-GGUF folder."
            : "The SAM-Audio model file is missing.");
    }
    return {};
}

StemService::Result StemService::run (const Request& req, const Progress& progress,
                                      const Cancel& cancel)
{
    Result res;
    auto fail = [&res] (const char* code, const juce::String& msg, const juce::String& detail = {})
    {
        res.ok = false;
        res.cancelled = juce::String (code) == "cancelled";
        res.errorCode = code;
        res.error = msg;
        res.detail = detail;
        return res;
    };
    auto report = [&progress] (double fraction, const juce::String& status)
    {
        if (progress) progress (fraction, status);
    };
    auto cancelled = [&cancel] { return cancel != nullptr && cancel (); };

    juce::String code;
    const juce::String invalid = validate (req, &code);
    if (invalid.isNotEmpty ())
        return fail (code.toRawUTF8 (), invalid);
    if (!supported ())
        return fail ("unsupported_platform", "Stem separation failed.",
                     "Stem Separation requires the Windows CLI binary.");

    const juce::File binary = req.binary == juce::File () ? sc::acpp::binaryPath () : req.binary;
    const int mode = (int) req.mode;

    // 1. Write the input range into the working folder as a float WAV.
    WorkDir work;
    if (req.workDir == juce::File ())
    {
        work.dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("SpectroCraft_stem_"
                           + juce::String (juce::Random::getSystemRandom ().nextInt (999998) + 1));
        work.temporary = true;
    }
    else
        work.dir = req.workDir;
    if (!work.dir.createDirectory ())
        return fail ("io_error", "Could not create the temporary working folder.");
    const juce::File tmpIn = work.dir.getChildFile ("input.wav");

    const int64_t nFrames = (int64_t) req.input[0].size ();

    // The htdemucs families require a 44.1 kHz input: resample the
    // extraction when the source rate differs (into a copy; the request's
    // input is only read).
    double writeRate = req.sourceRate;
    std::vector<std::vector<float>> resampled;
    if (mode >= 2 && std::abs (writeRate - 44100.0) > 1e-6 * 44100.0)
    {
        report (-1.0, "Resampling the input to 44.1 kHz...");
        resampled = req.input;
        for (auto& c : resampled)
            if (!resampleTrack (c, writeRate, 44100.0))
                return fail ("io_error", "Could not resample the input to 44.1 kHz.");
        writeRate = 44100.0;
    }
    const std::vector<std::vector<float>>& inData = resampled.empty () ? req.input : resampled;

    if (!sc::py::writeFloatWav (tmpIn, inData, writeRate))
        return fail ("io_error", "Could not write the temporary input file.");

    // Subsequent descriptions must work on a single mono track rather than
    // the whole multichannel extraction. Write a downmixed copy once when
    // the source has more than one channel (mono sources need none). Not
    // needed for the htdemucs modes: they run a single plain separation.
    const juce::File tmpInMono = work.dir.getChildFile ("input_mono.wav");
    bool haveMonoCopy = false;
    if (mode == 1 && inData.size () > 1)
    {
        std::vector<std::vector<float>> mono;
        mono.push_back (sc::acpp::downmixToMono (inData));
        if (!sc::py::writeFloatWav (tmpInMono, mono, req.sourceRate))
            return fail ("io_error", "Could not write the temporary mono input file.");
        haveMonoCopy = true;
    }
    resampled.clear ();
    resampled.shrink_to_fit ();

    const double inputDur = (double) nFrames / req.sourceRate;

    // Log lines are frequent: forward at most one every 300 ms, with a fresh
    // throttle per CLI call (like the window had), so the first line of each
    // run always shows.
    auto logForwarder = [&report] (const juce::String& prefix)
    {
        auto lastUi = std::make_shared<std::chrono::steady_clock::time_point> (
            std::chrono::steady_clock::now () - std::chrono::milliseconds (500));
        return [lastUi, &report, prefix] (const juce::String& line)
        {
            const auto now = std::chrono::steady_clock::now ();
            if (now - *lastUi < std::chrono::milliseconds (300)) return;
            *lastUi = now;
            report (-1.0, prefix + "  |  " + line);
        };
    };
    auto keepGoing = [&cancelled] { return !cancelled (); };

    // 2. Run the separation through the CLI.
    std::vector<std::vector<float>> tracks;      // one per output track
    std::vector<double> trackRates;
    auto addStem = [&] (const juce::String& name, const juce::File& f, double rate,
                        std::vector<std::vector<float>>& data)
    {
        Stem s;
        s.name = name;
        s.file = work.temporary ? juce::File () : f;
        s.rate = rate;
        s.channels = (int) data.size ();
        s.frames = data.empty () ? 0 : (int64_t) data[0].size ();
        res.stems.push_back (s);
        for (auto& c : data) { tracks.push_back (std::move (c)); trackRates.push_back (rate); }
    };

    if (mode >= 2)
    {
        // Plain separation task: a single CLI call with no description or
        // anchors. The tool writes one named .wav file per stem into outDir.
        const juce::String family = (mode == 2 ? "htdemucs_6stems" : "htdemucs");
        report (0.0, "Running " + family + "...");

        const juce::File outDir = work.dir.getChildFile ("sep");
        sc::acpp::RunResult rr;
        std::string err;
        const bool ok = sc::acpp::runSepTask (binary, family, req.model, req.backend, tmpIn, outDir,
                                              keepGoing, logForwarder ("Running " + family), &rr, &err);
        if (!ok)
        {
            if (rr.cancelled)
                return fail ("cancelled", "Run cancelled.");
            return fail ("tool_failed", "Stem separation failed.", failureDetail (rr, err));
        }

        // Collect every stem wav in the output folder, sorted by name so
        // the track order is stable across runs.
        std::vector<juce::File> stems;
        for (auto entry : juce::RangedDirectoryIterator (outDir, false))
            if (entry.getFile ().hasFileExtension ("wav"))
                stems.push_back (entry.getFile ());
        std::sort (stems.begin (), stems.end (),
                   [] (const juce::File& a, const juce::File& b) {
                       return a.getFileName ().toLowerCase () < b.getFileName ().toLowerCase (); });
        if (stems.empty ())
            return fail ("no_output", "The tool produced no stem files in " + outDir.getFullPathName ());

        for (const auto& f : stems)
        {
            double tRate = 0.0;
            int nchT = 0;
            std::vector<std::vector<float>> tData;
            if (!sc::py::readFloatWav (f, &tRate, &nchT, tData))
                return fail ("io_error", "Could not read the tool's output file " + f.getFileName () + ".");
            addStem (f.getFileNameWithoutExtension (), f, tRate, tData);
        }
        report (1.0, {});
    }
    else
    {
        const int N = (int) req.descriptions.size ();
        juce::File prevResidual;

        for (int i = 0; i < N; ++i)
        {
            if (cancelled ())
                return fail ("cancelled", "Run cancelled.");

            const auto& d = req.descriptions[(size_t) i];
            const juce::String what = "Description " + juce::String (i + 1) + "/" + juce::String (N)
                                     + ": " + d.text;
            report ((double) i / (double) N, what);

            // Chained mode: every description after the first works on the
            // previous residual. From-original mode: subsequent descriptions
            // work on a single mono track of the original extraction - never
            // the whole multichannel file (the first keeps the full input).
            juce::File inWav;
            if (!req.extractFromOriginal && i > 0)   inWav = prevResidual;
            else if (i > 0 && haveMonoCopy)          inWav = tmpInMono;
            else                                     inWav = tmpIn;
            const juce::File outDir = work.dir.getChildFile ("d" + juce::String (i + 1));

            const auto relAnchors = anchorsRelativeToInput (d.anchors, req.extStartSecs, inputDur);

            sc::acpp::RunResult rr;
            std::string err;
            const bool ok = sc::acpp::runStemSeparation (binary, req.model, req.backend, inWav, d.text,
                                                         relAnchors, outDir, keepGoing,
                                                         logForwarder (what), &rr, &err);
            if (!ok)
            {
                if (rr.cancelled)
                    return fail ("cancelled", "Run cancelled.");
                return fail ("tool_failed", "Stem separation failed for '" + d.text + "'.",
                             failureDetail (rr, err));
            }

            const juce::File targetF   = outDir.getChildFile ("target.wav");
            const juce::File residualF = outDir.getChildFile ("residual.wav");
            if (!targetF.existsAsFile () || !residualF.existsAsFile ())
                return fail ("no_output", "The tool did not produce target.wav / residual.wav in "
                                              + outDir.getFullPathName ());

            double tRate = 0.0, rRate = 0.0;
            int nchT = 0, nchR = 0;
            std::vector<std::vector<float>> tData, rData;
            if (!sc::py::readFloatWav (targetF, &tRate, &nchT, tData)
                || !sc::py::readFloatWav (residualF, &rRate, &nchR, rData))
                return fail ("io_error", "Could not read the tool's output files.");

            // Every description contributes its target; only the LAST one
            // also keeps its residual, so a run with N descriptions yields
            // N targets plus a single final "everything else" track instead of
            // an intermediate residual after every step.
            addStem ("target " + juce::String (i + 1), targetF, tRate, tData);
            if (i + 1 == N)
                addStem ("residual", residualF, rRate, rData);
            prevResidual = residualF;   // still needed as the next step's input
            report ((double) (i + 1) / (double) N, {});
        }
    }

    // 3. Normalise every track to the source rate and pad to equal length.
    int64_t maxLen = 0;
    for (const auto& t : tracks) maxLen = std::max (maxLen, (int64_t) t.size ());
    if (maxLen <= 0)
        return fail ("no_output", "The tool produced empty output.");

    for (size_t i = 0; i < tracks.size (); ++i)
    {
        auto& t = tracks[i];
        if (std::abs (trackRates[i] - req.sourceRate) > 1e-6 * req.sourceRate && !t.empty ())
            if (!resampleTrack (t, trackRates[i], req.sourceRate))
                return fail ("io_error", "Could not resample the tool's output to "
                                             + juce::String (req.sourceRate, 0) + " Hz.");
    }
    // Resampling can change lengths: pad after every track is at the source rate.
    maxLen = 0;
    for (const auto& t : tracks) maxLen = std::max (maxLen, (int64_t) t.size ());
    for (auto& t : tracks)
        t.resize ((size_t) maxLen, 0.0f);

    res.merged.rate     = req.sourceRate;
    res.merged.channels = (int) tracks.size ();
    res.merged.data     = std::move (tracks);
    res.ok = true;
    return res;
}

} // namespace sc
