// SpectroCraft - headless super resolution (audio.cpp CLI orchestration).
// Extracted from SuperResolutionWindow::runWorker: same steps, same messages,
// no UI. See include/AI/SuperResService.h.
#include "AI/SuperResService.h"
#include "PythonScripts.h"   // sc::py::readFloatWav / writeFloatWav
#include "Resampler.h"

#include <chrono>
#include <cmath>

namespace sc {

namespace {

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

} // namespace

bool SuperResService::supported()
{
#if JUCE_WINDOWS
    return true;
#else
    return false;
#endif
}

juce::String SuperResService::validate (const Request& req, juce::String* codeOut)
{
    auto bad = [codeOut] (const char* code, const juce::String& msg)
    {
        if (codeOut != nullptr) *codeOut = code;
        return msg;
    };
    if (codeOut != nullptr) codeOut->clear ();

    const int m = (int) req.mode;
    if (m != 1 && m != 2)
        return bad ("invalid_mode", "Unknown super resolution mode " + juce::String (m) + ".");
    if (req.backend != "cpu" && req.backend != "cuda" && req.backend != "vulkan")
        return bad ("invalid_backend", "Unknown backend '" + req.backend + "' (expected cpu, cuda or vulkan).");
    if (!std::isfinite (req.sourceRate) || req.sourceRate <= 0.0)
        return bad ("invalid_rate", "The source sample rate is invalid.");
    if (req.input.empty () || req.input[0].empty ())
        return bad ("empty_input", "The selected range is empty.");
    for (const auto& c : req.input)
        if (c.size () != req.input[0].size ())
            return bad ("ragged_input", "The input channels do not have the same length.");

    const juce::File bin = req.binary == juce::File () ? sc::acpp::binaryPath () : req.binary;
    if (!bin.existsAsFile ())
        return bad ("binary_missing", "The audiocpp_cli.exe binary is missing.");
    if (!req.model.existsAsFile ())
        return bad ("model_missing", req.mode == Mode::AudioSR ? "The AudioSR model file is missing."
                                                                : "The FlashSR model file is missing.");
    return {};
}

SuperResService::Result SuperResService::run (const Request& req, const Progress& progress,
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

    juce::String code;
    const juce::String invalid = validate (req, &code);
    // The window never checked the binary up front: a missing one surfaced
    // as a failed run with the reason in the detail dialog. Keep that.
    if (code == "binary_missing")
        return fail ("binary_missing", "Super resolution failed.", invalid);
    if (invalid.isNotEmpty ())
        return fail (code.toRawUTF8 (), invalid);
    if (!supported ())
        return fail ("unsupported_platform", "Super resolution failed.",
                     "Super Resolution requires the Windows CLI binary.");

    const juce::File binary = req.binary == juce::File () ? sc::acpp::binaryPath () : req.binary;
    const bool audioSR = req.mode == Mode::AudioSR;

    // 1. Write the input range as a float WAV at the source rate/channel
    //    count - both SR families downmix and resample to their native mono
    //    48 kHz pipeline internally.
    WorkDir work;
    if (req.workDir == juce::File ())
    {
        work.dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("SpectroCraft_superres_"
                           + juce::String (juce::Random::getSystemRandom ().nextInt (999998) + 1));
        work.temporary = true;
    }
    else
        work.dir = req.workDir;
    if (!work.dir.createDirectory ())
        return fail ("io_error", "Could not create the temporary working folder.");

    const juce::File tmpIn = work.dir.getChildFile ("input.wav");
    if (!sc::py::writeFloatWav (tmpIn, req.input, req.sourceRate))
        return fail ("io_error", "Could not write the temporary input file.");

    // 2. Run the task. The tool writes a single enhanced WAV straight to --out.
    const juce::File outWav = work.dir.getChildFile ("enhanced.wav");
    if (outWav.existsAsFile ()) outWav.deleteFile ();   // leftover from an earlier run

    const juce::String label (audioSR ? "Running AudioSR" : "Running FlashSR");
    report (0.0, label + "...");

    sc::acpp::RunResult rr;
    std::string err;
    auto lastUi = std::chrono::steady_clock::now () - std::chrono::milliseconds (500);
    const bool ok = sc::acpp::runSuperResTask (
        binary, (int) req.mode, req.model, req.backend, tmpIn, outWav,
        [&cancel] { return cancel == nullptr || !cancel (); },
        [&] (const juce::String& line) {
            const auto now = std::chrono::steady_clock::now ();
            if (now - lastUi < std::chrono::milliseconds (300)) return;
            lastUi = now;
            report (-1.0, label + "  |  " + line);
        },
        &rr, &err);

    if (!ok)
    {
        if (rr.cancelled)
            return fail ("cancelled", "Run cancelled.");
        const juce::String detail = rr.stderrText.isNotEmpty () ? logTail (rr.stderrText)
                                  : rr.stdoutText.isNotEmpty () ? logTail (rr.stdoutText)
                                                                : juce::String (err);
        return fail ("tool_failed", "Super resolution failed.", detail);
    }

    if (!outWav.existsAsFile ())
        return fail ("no_output", "The tool did not produce " + outWav.getFullPathName () + ".");

    // 3. Read the enhanced result (mono at the model's native rate, 48 kHz for
    //    both families); optionally bring it back to the source rate.
    double tRate = 0.0;
    int nchT = 0;
    std::vector<std::vector<float>> tracks;
    if (!sc::py::readFloatWav (outWav, &tRate, &nchT, tracks) || tracks.empty () || tracks[0].empty ())
        return fail ("io_error", "Could not read the tool's output file.");

    res.toolRate = tRate;
    double trackRate = tRate;
    if (req.resampleToSourceRate && std::abs (trackRate - req.sourceRate) > 1e-6 * req.sourceRate)
    {
        report (-1.0, "Resampling the result to " + juce::String (req.sourceRate, 0) + " Hz...");
        // Like the window always did: only the first (mono) track is resampled.
        auto& t = tracks[0];
        juce::AudioBuffer<float> in (1, (int) t.size ());
        std::copy (t.begin (), t.end (), in.getWritePointer (0));
        juce::AudioBuffer<float> out (1, (int) sc::Resampler::estimateOutputFrames (
            (int64_t) t.size (), trackRate, req.sourceRate));
        if (!sc::Resampler::resample (in, trackRate, req.sourceRate, out))
            return fail ("io_error", "Could not resample the result to "
                                         + juce::String (req.sourceRate, 0) + " Hz.");
        t.assign (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples ());
        tracks.resize (1);   // a multi-channel result would otherwise mix two rates
        trackRate = req.sourceRate;
    }

    report (1.0, {});

    res.outputFile      = work.temporary ? juce::File () : outWav;
    res.merged.rate     = trackRate;
    res.merged.channels = (int) tracks.size ();
    res.merged.data     = std::move (tracks);
    res.ok = true;
    return res;
}

} // namespace sc
