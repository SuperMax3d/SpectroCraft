/**
 * SpectroCraft - Main entry point
 * Supports offline modes:
 *   --test-spectral <in.wav> <out.png> [width height t0 t1]
 *   --test-load <in.wav>                       (load timing + mipmap stats)
 *   --test-noise-suppression <in.wav> <out.wav> [modelId]   (AI builds only)
 *       modelId: denseunet (default) | deepfilternet2 | deepfilternet3
 *   --run-action <actionName> <inputFile> <outputFile>
 *       Runs a recorded action (see the Actions panel) on one file headlessly
 *       and saves the result to outputFile.
 *   --batch <actionName> <inFolder> <outFolder> [formats] [rename] [--subfolders]
 *       Batch-runs a recorded action over every matching file in the folder
 *       tree. formats is a ';'-separated list like "*.wav; *.mp3" (default
 *       "*.wav"); rename uses %Filename% for the original name without its
 *       extension (default "%Filename%.wav"). --subfolders scans recursively.
 *   --test-transcribe <in.wav> [modelId] [language] [fast 0|1]   (AI builds only)
 *       modelId: whisper-base (default) | whisper-small | ... ; language: auto|en|...
 *   --test-batch-transcribe <folder> [modelId] [language] [skipExisting 0|1]
 *       [fast 0|1] [--translate] [--channel N] [--no-auto-download]
 *       [--meta-mode auto|embedded|external]
 *       (AI builds only)
 *       Transcribes every .wav in the folder tree headlessly and writes each
 *       file's SCTT chunk back in place. Defaults: whisper-base, auto
 *       language, overwrite existing, balanced speed, channel 1 (a value of
 *       0 or less mixes all channels together). --meta-mode forces where the
 *       file's metadata ends up after the run (auto keeps what it found):
 *       embedded moves everything into RIFF chunks inside the WAV; external
 *       consolidates it into a .meta.json sidecar next to the file.
 *   --list-ai-devices                        (AI builds only)
 *       Prints the OpenVINO devices available on this machine.
 *   --generate-test-files [dir]
 *   --run-unit-tests
 *       With SC_DIAG_LOG_DIR=<folder> the suite writes a diagnostic log there
 *       (ctest sets it per shard; CI uploads it when a shard fails).
 * Normal start options:
 *   --diag-log   diagnostic log for this session (docs/diagnostics.md), as
 *                Help > Diagnostic Log does; SC_DIAG_LOG=1 or
 *                SC_DIAG_LOG_DIR=<folder> do the same. It records the startup
 *                phase timings too ("startup" event).
 */

#include <JuceHeader.h>
#include "Application.h"
#include "CrashTrace.h"
#include "InvalidParamTrace.h"
#include "SingleInstance.h"
#include "StartupTrace.h"
#include "Version.h"   // auto-generated: SC_VERSION_STRING (bumped on every build)
#include "MainWindow.h"
#include "Docking.h"   // sc::dock::DockLookAndFeel (Adobe-style UI font and colours)
#include "RuntimeDlls.h"
#include "SpectralEngine.h"
#include "PngWriter.h"
#include "Settings.h"
#include "control/JsonCli.h"
#include "control/ControlServer.h"
#include "control/ControlState.h"
#include "control/StateJson.h"
#include "Diagnostics.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "NoiseSuppression.h"
    #include "AI/ModelManager.h"
    #include "AI/OvRuntime.h"
    #include "AI/WhisperPipeline.h"
#endif
#include <vector>
#include <cmath>
#include <exception>

// ---- Offline test hooks (implemented in tests/UnitTests.cpp) ----
extern int scRunUnitTests();
extern bool scGenerateTestFiles (const juce::String& dir);

// T0 of the startup timings (include/StartupTrace.h): fires as soon as this
// module's static init runs, i.e. right after the process image and imported
// DLLs have been loaded.
namespace { struct StartTrace { StartTrace() { scStart::begin(); } } startTrace; }

// juce::File(String) asserts on relative paths, so resolve them against the
// current working directory first (CLI args are often relative).
static juce::File resolvePath (const juce::String& p)
{
    if (p.startsWithChar ('\\') || p.containsChar (':'))
        return juce::File (p);
    return juce::File::getCurrentWorkingDirectory().getChildFile (p);
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
// Minimal 32-bit float RIFF writer for the --test-noise-suppression hook.
static bool writeFloatWav(const juce::File& file, int nchans, double rate,
                          const std::vector<std::vector<float>>& data)
{
    if (nchans <= 0 || data.empty() || data[0].empty()) return false;

    const uint32_t frames = (uint32_t) data[0].size();
    const uint32_t blockAlign = (uint32_t) nchans * 4u;
    const uint32_t dataSize = frames * blockAlign;

    file.deleteFile();   // FileOutputStream opens in append mode
    juce::FileOutputStream out (file);
    if (out.failedToOpen()) return false;

    // AI builds target little-endian CPUs only (x86-64, arm64), so the
    // native-order writers are already little-endian.
    auto w16 = [&out] (uint16_t v) { out.writeShort ((short) v); };
    auto w32 = [&out] (uint32_t v) { out.writeInt ((int) v); };

    out.write ("RIFF", 4);
    w32 (36 + dataSize);
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    w32 (16);                    // fmt chunk size
    w16 (3);                     // WAVE_FORMAT_IEEE_FLOAT (the samples are float32)
    w16 ((uint16_t) nchans);
    w32 ((uint32_t) rate);
    w32 ((uint32_t) (rate * blockAlign));   // byte rate
    w16 ((uint16_t) blockAlign);
    w16 (32);                    // bits per sample
    out.write ("data", 4);
    w32 (dataSize);

    for (uint32_t f = 0; f < frames; ++f)
        for (int c = 0; c < nchans; ++c)
        {
            int32_t bits;
            std::memcpy (&bits, &data[c][(size_t) f], sizeof (bits));
            out.writeInt ((int) bits);
        }
    return true;
}
#endif

class SpectroCraftApplication : public juce::JUCEApplication
{
public:
    SpectroCraftApplication()
    {
#if defined (_WIN32)
        scCrash::install(); // trace real crashes; pass through driver FATAL_APP_EXIT events
        // ... and append a crash report to the diagnostic log folder when the
        // log is on (folded into the next session's session.unclean event).
        scCrash::g_onCrash = &sc::diag::writeWindowsCrash;
#if JUCE_DEBUG
        scInvalidParam::install();  // symbolize CRT/_STL_VERIFY failures (debug only)
#endif
#endif
    }

    const juce::String getApplicationName() override { return "SpectroCraft"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Adobe-style look for the whole app: Source Sans 3 UI font, neutral
        // dark greys, dark popup menus (include/Docking.h). Installed before
        // any window exists; the unit tests run with it as well.
        lookAndFeel = std::make_unique<sc::dock::DockLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

#if defined (_WIN32)
        // Bundled runtime DLLs live in the SpectroCraft subfolder next to this
        // executable; add it to the process DLL search path before anything
        // loads or delay-load-resolves one of them.
        sc::setupRuntimeDllSearchPath();
#endif
        try
        {
            initialiseInternal (commandLine);
        }
        catch (const std::exception& ex)
        {
            std::fprintf (stderr, "[init] C++ exception: %s\n", ex.what());
            throw;
        }
    }

    void initialiseInternal (const juce::String& commandLine)
    {
        // Whitespace tokenisation of the command line, with double-quote
        // support so arguments containing spaces can be passed as one token.
        juce::StringArray args;
        {
            juce::String cur;
            bool inQuotes = false;
            for (int i = 0; i < commandLine.length(); ++i)
            {
                const char32_t c = commandLine[(size_t) i];
                if (c == '"')
                {
                    inQuotes = !inQuotes;
                    continue;   // drop the quote character itself
                }
                if (!inQuotes && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
                {
                    if (!cur.isEmpty()) { args.add (cur); cur.clear(); }
                }
                else
                    cur += juce::String::charToString ((juce::juce_wchar) c);
            }
            if (!cur.isEmpty())
                args.add (cur);
        }

        // Audio files passed on the command line are collected here and
        // opened after the app is up - single-instance handling below may hand
        // them to a running instance instead.
        std::vector<juce::File> cliFiles;
        bool controlFlag = false;
        bool diagFlag = false;
        bool finishUpdateFlag = false;   // started by spectrocraft-updater (docs/updates.md)

        // Headless JSON mode owns the whole command line (its options may
        // come in any order): see include/control/JsonCli.h.
        if (args.contains ("--json") || args.contains ("--list-commands"))
        {
            const int rc = sc::control::runJsonCli (args);
            setApplicationReturnValue (rc);
            quit();
            return;
        }

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--test-spectral")
            {
                if (i + 2 >= args.size())
                {
                    std::cout << "usage: --test-spectral <in.wav> <out.png> [w h t0 t1]\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File in  = resolvePath (args[++i]);
                const juce::File out = resolvePath (args[++i]);
                int w = 1024, h = 512;
                double t0 = -1, t1 = -1;
                if (i + 4 < args.size())
                {
                    w = args[++i].getIntValue();
                    h = args[++i].getIntValue();
                    t0 = args[++i].getFloatValue();
                    t1 = args[++i].getFloatValue();
                }

                AudioEngine engine;
                if (!engine.loadFile (in))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                SpectralEngine spectral;
                Settings s;
                spectral.setColourMap (ColourMap::Classic);

                std::vector<int> chans;
                for (int c = 0; c < engine.numChannels(); ++c)
                    chans.push_back (c);

                double tt0 = t0 < 0 ? 0.0 : t0;
                double tt1 = (t1 <= tt0) ? engine.duration() : t1;
                std::vector<int> allChans;
                for (int c = 0; c < engine.numChannels(); ++c)
                    allChans.push_back (c);
                std::vector<uint8_t> rgba;
                spectral.renderToPixels (engine, s, w, h, tt0, tt1, allChans, rgba);
                const bool ok = PngWriter::writePng (out, w, h, rgba);
                std::cout << (ok ? "wrote " : "failed to write ") << out.getFullPathName() << "\n";
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
                return;
            }

            else if (args[i] == "--run-action" || args[i] == "--batch")
            {
                const bool isBatch = (args[i] == "--batch");

                // --run-action <actionName> <inputFile> <outputFile>
                // --batch    <actionName> <inputFolder> <outputFolder>
                //             [formatsCsv] [renamePattern] [--subfolders]
                if (i + 3 >= args.size())
                {
                    std::cout << (isBatch
                        ? "usage: --batch <actionName> <inFolder> <outFolder> [formats] [rename] [--subfolders]\n"
                        : "usage: --run-action <actionName> <inputFile> <outputFile>\n");
                    setApplicationReturnValue (2);
                    quit();
                    return;
                }

                const juce::String name = args[++i];
                const juce::File a  = resolvePath (args[++i]);
                const juce::File b  = resolvePath (args[++i]);

                // Load the recorded actions from the user's recent store.
                juce::PropertiesFile::Options opts;
                opts.applicationName = "SpectroCraft";
                opts.filenameSuffix = ".recent";
                const juce::PropertiesFile store (opts.getDefaultFile(), opts);
                sc::ActionStore actions;
                if (!actions.loadFromJson (store.getValue ("actions_json")))
                {
                    std::cout << "could not load the recorded actions\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                const sc::Action* action = nullptr;
                for (int k = 0; k < actions.size (); ++k)
                    if (actions.get (k)->name == name) { action = actions.get (k); break; }
                if (action == nullptr || action->steps.empty())
                {
                    std::cout << "no recorded action named \"" << name.toRawUTF8()
                              << "\" with steps\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                // Isolated store: the headless application must never touch
                // (or save into) the user's real settings file.
                const juce::File tempStore = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                 .getChildFile (isBatch ? "SpectroCraft_batch.recent"
                                                                         : "SpectroCraft_run_action.recent");

                if (!isBatch)
                {
                    Application app (tempStore);
                    if (!app.openFile (a))
                    {
                        std::cout << "failed to open " << a.getFullPathName() << "\n";
                        setApplicationReturnValue (1);
                        setApplicationReturnValue (1);
                        quit();
                        return;
                    }
                    app.selectAll();   // selection-based steps act on the whole file

                    for (const auto& step : action->steps)
                    {
                        std::string err;
                        if (!sc::applyActionStep (app, step, &err))
                        {
                            std::cout << "step \"" << step.type.toRawUTF8()
                                      << "\" failed: " << err << "\n";
                            setApplicationReturnValue (1);
                            quit();
                            return;
                        }
                    }

                    const bool ok = app.audioEngine().saveFileAs (b);
                    std::cout << (ok ? "wrote " : "failed to write ") << b.getFullPathName() << "\n";
                    setApplicationReturnValue (ok ? 0 : 1);
                    quit();
                    return;
                }

                // Batch: optional [formats] [rename] tokens, then flags.
                juce::String formatsText = "*.wav", rename = "%Filename%.wav";
                bool subfolders = false;
                int optSeen = 0;
                while (i + 1 < args.size())
                {
                    const auto t = args[i + 1];
                    if (t == "--subfolders") { subfolders = true; ++i; }
                    else if (optSeen == 0)   { formatsText = t; ++i; ++optSeen; }
                    else if (optSeen == 1)   { rename = t;      ++i; ++optSeen; }
                    else break;
                }

                const auto formats = sc::parseFormatList (formatsText);
                std::string summary;
                const bool batchOk = sc::runActionBatch (*action, a, b, formats, rename, subfolders,
                                                         tempStore, nullptr, nullptr, nullptr, &summary);
                std::cout << summary << "\n";
                setApplicationReturnValue (batchOk ? 0 : 1);
                quit();
                return;
            }

            else if (args[i] == "--test-load")
            {
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: --test-load <in.wav>\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File in = resolvePath (args[++i]);

                AudioEngine engine;
                int64_t progressCalls = 0;
                const auto t0 = juce::Time::getMillisecondCounter();
                if (!engine.loadFile (in, [&progressCalls] (float) { ++progressCalls; }))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;   // ~AudioEngine runs here
                }
                const int64_t ms = juce::Time::getMillisecondCounter() - t0;

                std::cout << "loaded " << (int) engine.duration() << " s, "
                          << engine.numChannels() << " ch @ " << (int) engine.sampleRate()
                          << " Hz in " << ms << " ms\n";
                std::cout << "mipmaps: " << engine.mipmaps().size() << " levels, base factor "
                          << engine.mipmapBaseFactor() << ", progress callbacks: "
                          << progressCalls << "\n";
                quit();
                return;
            }
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            else if (args[i] == "--test-noise-suppression")
            {
                if (i + 2 >= args.size())
                {
                    std::cout << "usage: --test-noise-suppression <in.wav> <out.wav> [modelId]\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File in  = resolvePath (args[++i]);
                const juce::File out = resolvePath (args[++i]);
                std::string modelId = "denseunet";
                if (i + 1 < args.size())
                    modelId = args[++i].toStdString ();

                AudioEngine engine;
                if (!engine.loadFile (in))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                const int nch = engine.numChannels ();
                const double rate = engine.sampleRate ();
                const int64_t frames = engine.numSamples ();
                std::vector<std::vector<float>> data (nch);
                for (int c = 0; c < nch; ++c)
                {
                    data[c].resize ((size_t) frames);
                    if (!engine.readSamples (c, 0, frames, data[c].data()))
                    {
                        std::cout << "failed to read audio\n";
                        setApplicationReturnValue (1);
                        quit();
                        return;
                    }
                }

                int lastPct = -1;
                std::string err;
                const bool isDf = modelId == "deepfilternet2" || modelId == "deepfilternet3";
                const auto progress = [&lastPct] (float f) {
                    const int pct = (int) (f * 100.0);
                    if (pct >= lastPct + 10 || pct == 100)
                    {
                        lastPct = std::max (lastPct, pct);
                        std::cout << "  " << lastPct << "%\n";
                        std::flush (std::cout);
                    }
                    return true;
                };
                const bool ok = isDf
                    ? sc::runDeepFilterPipeline (modelId, "CPU", rate, nch, data,
                                                 0.f, false, progress, &err)
                    : sc::runDenseUnetPipeline ("denseunet", "CPU", rate, nch, data,
                                                progress, &err);

                if (!ok)
                {
                    std::cout << "noise suppression failed: " << err << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                const bool wrote = writeFloatWav (out, nch, rate, data);
                std::cout << (wrote ? "wrote " : "failed to write ") << out.getFullPathName() << "\n";
                setApplicationReturnValue (wrote ? 0 : 1);
                quit();
                return;
            }
            else if (args[i] == "--test-transcribe")
            {
                // Transcribes the whole file with Whisper and prints the
                // segments. Usage: --test-transcribe <in.wav> [modelId] [language] [fast 0|1]
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: --test-transcribe <in.wav> [modelId] [language] [fast 0|1]\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File in = resolvePath (args[++i]);
                std::string modelId  = "whisper-base";
                std::string language = "auto";
                bool fast = false;
                if (i + 1 < args.size()) modelId  = args[++i].toStdString ();
                if (i + 1 < args.size()) language = args[++i].toStdString ();
                if (i + 1 < args.size() && args[i + 1] == "1") { fast = true; ++i; }

                AudioEngine engine;
                if (!engine.loadFile (in))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                auto& mm = sc::ai::ModelManager::instance ();
                const sc::ai::ModelSpec* spec = mm.find (modelId);
                if (spec == nullptr)
                {
                    std::cout << "unknown transcription model: " << modelId << "\n";
                    quit();
                    return;
                }

                int lastPct = -1;
                const auto progress = [&lastPct] (float f) {
                    const int pct = (int) (f * 100.0);
                    if (pct >= lastPct + 10 || pct == 100)
                    {
                        lastPct = std::max (lastPct, pct);
                        std::cout << "  " << lastPct << "%\n";
                        std::flush (std::cout);
                    }
                    return true;
                };

                std::string err;
                if (!mm.ensureInstalled (*spec,
                        [&progress] (float f, const std::string&) { return progress (0.15f * f); }, &err))
                {
                    std::cout << "model install failed: " << err << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                const juce::File dir = mm.dirFor (modelId);
                const std::string baseName = modelId.substr (std::string ("whisper-").size ());
                const juce::File ggmlModel = dir.getChildFile ("ggml-" + baseName + ".bin");

                sc::ai::WhisperTranscriber::Options opts;
                opts.language = language;
                opts.beamSize = fast ? 1 : 5;   // greedy vs beam search

                std::vector<ScttSegment> segs;
                const bool ok = sc::ai::runWhisperPipeline (engine, { 0 }, ggmlModel,
                                                           opts, segs, progress, &err);
                if (!ok)
                {
                    std::cout << "transcription failed: " << err << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                std::cout << "segments: " << segs.size() << "\n";
                for (const auto& s : segs)
                    std::cout << "[" << s.startSec << " - " << s.endSec << "] " << s.text << "\n";
                quit();
                return;
            }
            else if (args[i] == "--test-batch-transcribe")
            {
                // Batch-transcribes every .wav in a folder tree headlessly and
                // writes the SCTT chunk back into each file. Usage:
                //   --test-batch-transcribe <folder> [modelId] [language]
                //       [skipExisting 0|1] [fast 0|1] [--translate]
                //       [--channel N] [--no-auto-download] [--meta-mode M]
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: --test-batch-transcribe <folder> [modelId] [language] [skipExisting 0|1] [fast 0|1] [--translate] [--channel N] [--no-auto-download] [--meta-mode auto|embedded|external]\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File folder = resolvePath (args[++i]);
                std::string modelId  = "whisper-base";
                std::string language = "auto";
                bool skipExisting = false, fast = false, translate = false;
                int channel = 1;   // <= 0 mixes all channels together
                bool autoDownload = true;
                sc::MetaSaveTarget metaTarget = sc::MetaSaveTarget::Auto;

                // Positional options (in this order) are consumed until the
                // first "--flag" appears.
                int pos = 0;
                while (i + 1 < args.size() && !args[i + 1].startsWithChar ('-'))
                {
                    const juce::String a = args[++i];
                    if      (pos == 0) modelId      = a.toStdString ();
                    else if (pos == 1) language     = a.toStdString ();
                    else if (pos == 2) skipExisting = (a == "1");
                    else               fast         = (a == "1");
                    ++pos;
                }

                // Named flags, in any order.
                while (i + 1 < args.size())
                {
                    const juce::String a = args[++i];
                    if (a == "--translate")
                        translate = true;
                    else if (a == "--no-auto-download")
                        autoDownload = false;
                    else if (a == "--meta-mode" && i + 1 < args.size())
                    {
                        const juce::String v = args[++i].toLowerCase ();
                        if      (v == "auto")     metaTarget = sc::MetaSaveTarget::Auto;
                        else if (v == "embedded") metaTarget = sc::MetaSaveTarget::Embedded;
                        else if (v == "external") metaTarget = sc::MetaSaveTarget::External;
                        else
                        {
                            std::cout << "invalid --meta-mode value: "
                                      << v.toStdString () << "\n";
                            quit();
                            return;
                        }
                    }
                    else if (a == "--channel" && i + 1 < args.size())
                    {
                        const juce::String v = args[++i];
                        if (!Application::parseChannelText (v, channel))
                        {
                            std::cout << "invalid --channel value: "
                                      << v.toStdString () << "\n";
                            quit();
                            return;
                        }
                    }
                    else
                    {
                        std::cout << "unknown option: " << a.toStdString () << "\n";
                        quit();
                        return;
                    }
                }

                // No GUI needed for the batch run; an isolated settings store so a
                // headless run never rewrites the user's SpectroCraft.recent.
                const juce::File batchStore = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                  .getChildFile ("SpectroCraft_test_batch_transcribe.recent");
                Application app (batchStore);
                BatchTranscribeSummary sum;
                std::string err;
                const bool ok = app.batchTranscribeFolder (
                    folder, modelId, language, translate,
                    fast, skipExisting, channel,
                    [] (float f) {
                        static int lastPct = -1;
                        const int pct = (int) (f * 100.0);
                        if (pct >= lastPct + 10 || pct == 100)
                        {
                            lastPct = std::max (lastPct, pct);
                            std::cout << "  " << lastPct << "%\n";
                            std::flush (std::cout);
                        }
                        return true;
                    },
                    [] (const juce::String& name) {
                        std::cout << "  transcribing " << name.toStdString () << "\n";
                        std::flush (std::cout);
                    },
                    nullptr, &sum, &err, autoDownload, metaTarget);
                if (!ok)
                {
                    std::cout << "batch transcription failed: " << err << "\n";
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }

                std::cout << "total: "   << sum.total   << "\n"
                          << "done: "    << sum.done    << "\n"
                          << "skipped: " << sum.skipped << "\n"
                          << "failed: "  << sum.failed  << "\n";
                for (const auto& f : sum.failures)
                    std::cout << "  - " << f << "\n";
                setApplicationReturnValue (sum.failed > 0 ? 1 : 0);
                quit();
                return;
            }
            else if (args[i] == "--test-ns-failure-alert")
            {
                // Shows the exact failure dialog used by the GUI and verifies
                // that its text can be selected + copied (the old message box
                // had no selectable text). Usage:
                //   --test-ns-failure-alert <in.wav> [modelId] ["fake error"]
                // With a third argument the pipeline is skipped and that
                // string is used as the error, so both branches (missing
                // runtime vs. model problem) can be tested deterministically.
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: --test-ns-failure-alert <in.wav> [modelId] [\"fake error\"]\n";
                    setApplicationReturnValue (2);   // usage error
                    quit();
                    return;
                }
                const juce::File in = resolvePath (args[++i]);
                std::string modelId = "deepfilternet2";
                if (i + 1 < args.size())
                    modelId = args[++i].toStdString ();

                std::string err;
                bool haveErr = false;
                if (i + 1 < args.size())
                {
                    err = args[++i].toStdString ();
                    haveErr = true;
                }
                else
                {
                    AudioEngine engine;
                    if (!engine.loadFile (in))
                    {
                        std::cout << "failed to open " << in.getFullPathName() << "\n";
                        setApplicationReturnValue (1);
                        quit();
                        return;
                    }
                    const int nch = engine.numChannels ();
                    const double rate = engine.sampleRate ();
                    const int64_t frames = engine.numSamples ();
                    std::vector<std::vector<float>> data (nch);
                    for (int c = 0; c < nch; ++c)
                    {
                        data[c].resize ((size_t) frames);
                        if (!engine.readSamples (c, 0, frames, data[c].data()))
                        {
                            std::cout << "failed to read audio\n";
                            setApplicationReturnValue (1);
                            quit();
                            return;
                        }
                    }
                    const bool isDf = modelId == "deepfilternet2" || modelId == "deepfilternet3";
                    const auto progress = [] (float) { return true; };
                    const bool ok = isDf
                        ? sc::runDeepFilterPipeline (modelId, "CPU", rate, nch, data,
                                                     0.f, false, progress, &err)
                        : sc::runDenseUnetPipeline ("denseunet", "CPU", rate, nch, data,
                                                    progress, &err);
                    if (ok)
                    {
                        std::cout << "pipeline succeeded - no failure to show.\n"
                                  << "Pass a third argument to simulate an error instead.\n";
                        quit();
                        return;
                    }
                    haveErr = true;
                }

                const juce::String details = sc::buildNsFailureDetails (err, modelId);
                std::cout << "--- alert details ---\n" << details.toRawUTF8 ()
                          << "\n---------------------\n";

                // The same dialog the GUI shows.
                auto* w = new juce::AlertWindow ("Noise Suppression Failed",
                                                 "The selection could not be processed.\n"
                                                 "Details below - select any text to copy it.",
                                                 juce::AlertWindow::WarningIcon);
                auto* ed = new juce::TextEditor ();
                ed->setMultiLine (true);
                ed->setReadOnly (true);
                ed->setSize (560, 230);
                ed->setText (details, false);
                w->addCustomComponent (ed);
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));

                // Programmatic copy test: select all + copy, then read the
                // system clipboard back and compare.
                ed->selectAll ();
                ed->copyToClipboard ();
                // (This JUCE build exposes the clipboard as static
                //  SystemClipboard methods, not a Clipboard singleton.)
                const juce::String clip = juce::SystemClipboard::getTextFromClipboard ();
                std::cout << (clip == details ? "clipboard round-trip: OK\n"
                                              : "clipboard round-trip: MISMATCH\n");

                // Keep the dialog open briefly (so it can be screenshotted),
                // then close itself.
                struct AutoCloser : juce::Timer {
                    juce::AlertWindow* w;
                    explicit AutoCloser (juce::AlertWindow* a) : w (a) {}
                    void timerCallback() override { if (w != nullptr) w->exitModalState (1); }
                };
                w->enterModalState (false);
                ed->grabKeyboardFocus();
                AutoCloser closer (w);
                closer.startTimer (3000);
                w->runModalLoop ();
                delete w;
                delete ed;   // AlertWindow does not delete its custom components

                std::cout << "alert dialog closed\n";
                quit();
                return;
            }
#endif
            else if (args[i] == "--list-ai-devices")
            {
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
                std::string err;
                const bool ok = sc::ai::OvRuntime::instance ().init (&err);
                if (!ok)
                    std::cout << "OpenVINO runtime not available: " << err << "\n";
                else
                {
                    const auto devs = sc::ai::OvRuntime::instance ().devices ();
                    std::cout << "OpenVINO devices:";
                    if (devs.empty ())
                        std::cout << " (none)";
                    for (size_t j = 0; j < devs.size (); ++j)
                        std::cout << (j == 0 ? " " : ", ") << devs[j];
                    std::cout << "\n"
                              << "(OpenVINO only uses Intel CPU/GPU/NPU hardware - NVIDIA GPUs are not supported.)\n";
                }
#else
                std::cout << "AI features are disabled in this build.\n";
#endif
                quit();
                return;
            }
            else if (args[i] == "--run-unit-tests")
            {
                // Same isolation as SpectroCraftTests: never the user's real settings.
                Application::useIsolatedSettingsStore (true);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI && !defined(_WIN32)
                // ... nor the user's real model / OpenVINO caches (macOS resolves
                // ~/Library without $HOME). ctest passes its own folders; a run
                // by hand uses the same temp folders as SpectroCraftTests,
                // kept between runs so the weights are not downloaded again.
                {
                    const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory);
                    ::setenv ("SC_MODELS_DIR",
                              tmp.getChildFile ("SpectroCraftTests-models").getFullPathName ().toRawUTF8 (), 0);
                    ::setenv ("SC_OPENVINO_CACHE_DIR",
                              tmp.getChildFile ("SpectroCraftTests-openvino-cache").getFullPathName ().toRawUTF8 (), 0);
                }
#endif
                // ctest gives each shard a log folder (tests/CMakeLists.txt):
                // CI uploads it with the other test logs when a shard fails.
                // No freeze watchdog: the tests block the message thread on purpose.
                const juce::String diagDir = juce::SystemStats::getEnvironmentVariable ("SC_DIAG_LOG_DIR", {}).trim ();
                if (diagDir.isNotEmpty ())
                {
                    sc::diag::Config dc;
                    dc.folder = juce::File::getCurrentWorkingDirectory ().getChildFile (diagDir);
                    dc.reason = "tests";
                    dc.watchdog = false;
                    dc.appVersion = SC_VERSION_STRING;
                    sc::diag::start (dc);
                }
                const int rc = scRunUnitTests();
                if (sc::diag::isRunning ())
                    sc::diag::stop ();
                std::cout << (rc == 0 ? "all unit tests passed\n" : "unit tests FAILED\n");
                setApplicationReturnValue (rc == 0 ? 0 : 1);   // usable from scripts / CI
                quit();
                return;
            }
            else if (args[i] == "--generate-test-files")
            {
                const juce::File dir = resolvePath ((i + 1 < args.size()) ? args[i + 1] : ".");
                const bool ok = scGenerateTestFiles (dir.getFullPathName());
                std::cout << (ok ? "generated test files in " : "failed generating in ")
                          << dir.getFullPathName() << "\n";
                quit();
                return;
            }
            else if (args[i] == "--json" || args[i] == "--list-commands")
            {
                // Headless JSON mode (AI control): one request per line in,
                // one response per line out. See include/control/JsonCli.h.
                const int rc = sc::control::runJsonCli (args);
                setApplicationReturnValue (rc);
                quit();
                return;
            }
            else if (args[i] == "--diag-log")
            {
                diagFlag = true;   // diagnostic log for this session (not saved)
            }
            else if (args[i] == "--finish-update")
            {
                // The updater swapped the files and started this new version;
                // reportFinishedUpdate() says what it did (update-result.json).
                finishUpdateFlag = true;
            }
#if defined(SC_ENABLE_CONTROL) && SC_ENABLE_CONTROL
            else if (args[i] == "--control")
            {
                // AI control: start the loopback control server whatever the
                // setting, and never hand off to a running instance.
                controlFlag = true;
            }
#endif
            else if (args[i].startsWith ("--"))
            {
                // Unknown flags used to be ignored silently and the UI started.
                std::cerr << "unknown flag " << args[i].toRawUTF8()
                          << " (see README: command-line options)\n";
                setApplicationReturnValue (2);
                quit();
                return;
            }
            else if (args[i].length() > 0)
            {
                const juce::File f = resolvePath (args[i]);
                if (f.existsAsFile()) cliFiles.push_back (f);
            }
        }

        // Single-instance mode: when "Allow multiple instances" is off, hand
        // the command-line files to a running instance and quit this one.
        // --control skips it: the instance launched by the AI adapter must be
        // a controllable one (it writes its own discovery file).
        if (!controlFlag)
        {
            juce::StringArray names;
            for (const auto& f : cliFiles) names.add (f.getFullPathName());
            if (sc::singleinstance::handleStartup (names))
            {
                quit ();
                return;
            }
        }

        app = std::make_unique<Application>();
        mainWindow = std::make_unique<MainWindow> (*app, getApplicationName());

        // Every instance hosts the file-forwarding server so a later
        // single-instance launch can hand its command-line files over to it.
        forwarder = std::make_unique<sc::singleinstance::Coordinator> (
            [mw = mainWindow.get ()] (const juce::StringArray& paths) {
                for (auto& p : paths)
                    if (juce::File (p).existsAsFile ()) mw->openFile (juce::File (p));
            });
        forwarder->startServer ();

        // AI control server (opt-in: Window > Allow AI control, or --control).
#if defined(SC_ENABLE_CONTROL) && SC_ENABLE_CONTROL
        mainWindow->onControlToggled = [this] (bool on) { setControlServer (on); };
        if (controlFlag || app->settings.controlEnabled)
            setControlServer (true);
#endif

        // Diagnostic log (opt-in: Help > Diagnostic Log, --diag-log,
        // SC_DIAG_LOG=1 or SC_DIAG_LOG_DIR).
        mainWindow->onDiagToggled = [this] (bool on) { setDiagnostics (on, "setting"); };
        {
            const juce::String envOn = juce::SystemStats::getEnvironmentVariable ("SC_DIAG_LOG", {}).trim ();
            const juce::String envDir = juce::SystemStats::getEnvironmentVariable ("SC_DIAG_LOG_DIR", {}).trim ();
            const bool env = (envOn.isNotEmpty () && envOn != "0") || envDir.isNotEmpty ();
            if (diagFlag || env || app->settings.diagLogEnabled)
                setDiagnostics (true, diagFlag ? "flag" : env ? "env" : "setting");
        }
        // The "startup" event (phase timings) is posted once the window has
        // been shown (MainWindow, scStart::report).
        scStart::state.files = (int) cliFiles.size ();

        // Open the command-line files. Routing through MainWindow::openFile
        // gives large files their async load with a progress dialog, exactly
        // like File > Open.
        for (const auto& f : cliFiles)
            mainWindow->openFile (f);

        // Automatic updates (docs/updates.md, Windows only). Both happen after
        // the window is up and the files have been handed to the loader, so an
        // update dialog never competes with the startup or a large open.
#if JUCE_WINDOWS
        if (finishUpdateFlag)
        {
            juce::Component::SafePointer<MainWindow> mw (mainWindow.get());
            juce::Timer::callAfterDelay (700, [mw] { if (mw != nullptr) mw->reportFinishedUpdate(); });
        }
        else
        {
            // The optional once-a-day check (Help > Check for Updates at
            // Startup): silent unless something newer is found.
            juce::Component::SafePointer<MainWindow> mw (mainWindow.get());
            juce::Timer::callAfterDelay (4000,
                [mw] { if (mw != nullptr) mw->maybeCheckForUpdatesAtStartup(); });
        }
#endif
    }

    void shutdown() override
    {
        // The dispatch loop has ended: no heartbeat any more, so the freeze
        // watchdog would take the teardown for a hang.
        sc::diag::stopFreezeWatchdog ();
        // Control server first: in-flight requests get shutting_down and its
        // threads are joined while the window and documents still exist.
        if (controlServer != nullptr) controlServer->stop ();
        controlServer.reset ();
        if (forwarder != nullptr) forwarder->stopServer ();
        // The diagnostic log keeps recording the teardown but stops asking
        // the window and the documents (about to go) for samples.
        sc::diag::setSampler (nullptr);
        sc::diag::setHeaderProvider (nullptr);
        mainWindow.reset();
        app.reset();
        sc::diag::stop ();   // session.end: a clean exit (no session.unclean next time)
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
    }

    void systemRequestedQuit() override
    {
        // The macOS application-menu Quit and WM_QUERYENDSESSION land here
        // even while a modal progress dialog is open. A background job's
        // worker still uses the documents and this window (it deletes its
        // progress dialog when done): quitting now would destroy them under
        // it. MainWindow's own Quit paths already refuse.
        if (mainWindow != nullptr && mainWindow->isBackgroundJobRunning ())
            return;

        // Ask before losing unsaved work: audio edits OR metadata that has
        // not been written to disk yet (markers, selections, calibrations,
        // transcription).
        if (app != nullptr)
        {
            const auto unsaved = app->unsavedDocNames ();
            if (!unsaved.empty ())
            {
                juce::String list;
                for (const auto& n : unsaved)
                {
                    if (!list.isEmpty()) list += "\n";
                    list += n;
                }

                juce::AlertWindow dlg ("Quit SpectroCraft",
                    "You have unsaved files:\n\n" + list +
                    "\n\nDo you want to quit?", juce::AlertWindow::WarningIcon,
                    mainWindow.get ());
                dlg.addButton ("Yes, Quit", 1);
                dlg.addButton ("Cancel", 0);

                // Activate and raise the dialog before entering the modal loop.
                // If our process does not hold the foreground when the dialog is
                // created (another app or instance was active), SetFocus inside
                // enterModalState() silently fails and the window appears without
                // activation; the user's first click on "Yes, Quit" then only
                // activates it and can be swallowed by Windows, so they have to
                // click twice. We still hold input rights (the user just clicked X
                // or File > Quit), so BringWindowToTop via toFront() activates it.
                dlg.toFront (true);

                if (dlg.runModalLoop () != 1) return;   // stay open
            }
        }
        quit();
    }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<sc::dock::DockLookAndFeel> lookAndFeel;   // outlives every window
    std::unique_ptr<Application> app;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<sc::singleinstance::Coordinator> forwarder;   // file-forwarding server
    std::unique_ptr<sc::control::ControlServer> controlServer;    // AI control (loopback)

    // ---- Diagnostic log (src/Diagnostics.cpp) ------------------------------
    juce::String lastAudioDev;
    double lastAudioRate = 0.0;
    int lastAudioBlock = 0, lastXruns = -1;

    void setDiagnostics (bool on, const juce::String& reason)
    {
        if (!on)
        {
            sc::diag::stop ();
            return;
        }
        sc::diag::Config c;
        c.reason = reason;
        c.appVersion = SC_VERSION_STRING;
        const juce::String dirEnv = juce::SystemStats::getEnvironmentVariable ("SC_DIAG_LOG_DIR", {}).trim ();
        if (dirEnv.isNotEmpty ())
            c.folder = juce::File::getCurrentWorkingDirectory ().getChildFile (dirEnv);
        sc::diag::setHeaderProvider ([this] (juce::DynamicObject& h) { describeForDiagnostics (h); });
        sc::diag::setSampler ([this] (bool) { sampleAudioForDiagnostics (); });
        lastAudioDev = {};
        lastXruns = -1;
        if (!sc::diag::start (c))
            std::fprintf (stderr, "[diag] could not write the diagnostic log in %s\n",
                          sc::diag::folder ().getFullPathName ().toRawUTF8 ());
    }

    // Extra session.start fields: the state a reader needs to understand the
    // events (no user name, no machine name).
    void describeForDiagnostics (juce::DynamicObject& h)
    {
        if (app != nullptr)
        {
            h.setProperty ("docs", app->numDocs ());
            h.setProperty ("fft", app->settings.fftSize);
            h.setProperty ("aiControl", app->settings.controlEnabled);
            h.setProperty ("multiInstance", app->settings.allowMultipleInstances);
            h.setProperty ("audioOutput", Application::audioOutputDisabled () ? "disabled" : "default device");
        }
        if (mainWindow != nullptr)
        {
            const auto& ws = mainWindow->workspaces ();
            const auto* e = ws.find (ws.activeId ());
            h.setProperty ("ws", e != nullptr ? e->name : ws.activeId ());
            h.setProperty ("floating", mainWindow->dockManager ().numFloatingWindows ());
        }
       #if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        h.setProperty ("aiBuild", true);
       #else
        h.setProperty ("aiBuild", false);
       #endif
    }

    // Once a second (message thread): audio device changes, dropouts (xruns)
    // and the callback load while playing. Never touches the audio thread.
    void sampleAudioForDiagnostics ()
    {
        if (app == nullptr || !app->audioDeviceReady () || app->audioDevice == nullptr)
            return;
        auto* dev = app->audioDevice->getCurrentAudioDevice ();
        if (dev == nullptr)
            return;
        const juce::String name = dev->getName ();
        const double rate = dev->getCurrentSampleRate ();
        const int block = dev->getCurrentBufferSizeSamples ();
        const int xruns = dev->getXRunCount ();   // -1: not reported by this driver
        if (name != lastAudioDev || juce::roundToInt (rate) != juce::roundToInt (lastAudioRate) || block != lastAudioBlock)
        {
            lastAudioDev = name;
            lastAudioRate = rate;
            lastAudioBlock = block;
            lastXruns = xruns;
            sc::diag::event ("audio.device", sc::diag::Level::info,
                             { { "dev", name }, { "type", dev->getTypeName () }, { "sr", rate }, { "block", block },
                               { "out", dev->getActiveOutputChannels ().countNumberOfSetBits () },
                               { "latencyMs", juce::roundToInt (1000.0 * dev->getOutputLatencyInSamples () / juce::jmax (1.0, rate)) },
                               { "xruns", xruns >= 0 ? juce::var ("reported") : juce::var ("not reported") } });
            return;
        }
        const double load = app->audioDevice->getCpuUsage ();
        const bool playing = app->audioEngine ().isPlaying ();
        if (xruns >= 0 && lastXruns >= 0 && xruns > lastXruns)
            sc::diag::event ("audio.xrun", sc::diag::Level::warn,
                             { { "n", xruns - lastXruns }, { "total", xruns }, { "block", block }, { "sr", rate },
                               { "load", std::round (load * 1000.0) / 1000.0 }, { "playing", playing } });
        lastXruns = xruns;
        if (playing)
            sc::diag::recordAudioLoad (load);
    }

    void setControlServer (bool on)
    {
        if (!on)
        {
            if (controlServer != nullptr) controlServer->stop ();
            controlServer.reset ();
            if (mainWindow != nullptr) { mainWindow->setControlClients (0); mainWindow->controlServerRunning = false; }
            return;
        }
        if (controlServer != nullptr || app == nullptr) return;
        controlServer = std::make_unique<sc::control::ControlServer> (*app);
        controlServer->onClientsChanged = [this] (int n) {
            if (mainWindow != nullptr) mainWindow->setControlClients (n);
        };
        // Persistent journal (JSON Lines, metadata only) while the server runs.
        auto& journal = app->control().journal;
        if (journal.file() == juce::File())
        {
            sc::control::Journal::pruneOldSessions (sc::control::Journal::journalFolder());
            journal.setFile (sc::control::Journal::sessionFile (sc::control::processId()));
        }
        juce::String err;
        if (controlServer->start (&err))
        {
            if (mainWindow != nullptr) mainWindow->controlServerRunning = true;
            std::fprintf (stderr, "[control] listening on 127.0.0.1:%d (%s)\n", controlServer->port (),
                          controlServer->discoveryFile ().getFullPathName ().toRawUTF8 ());
        }
        else
        {
            std::fprintf (stderr, "[control] could not start: %s\n", err.toRawUTF8 ());
            controlServer.reset ();
        }
    }
};

START_JUCE_APPLICATION (SpectroCraftApplication)
