/**
 * SpectroCraft - Main entry point
 * Supports offline modes:
 *   --test-spectral <in.wav> <out.png> [width height t0 t1]
 *   --test-load <in.wav>                       (load timing + mipmap stats)
 *   --test-noise-suppression <in.wav> <out.wav> [modelId]   (AI builds only)
 *       modelId: denseunet (default) | deepfilternet2 | deepfilternet3
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
 */

#include <JuceHeader.h>
#include "Application.h"
#include "CrashTrace.h"
#include "SingleInstance.h"
#include "StartupTrace.h"
#include "MainWindow.h"
#include "SpectralEngine.h"
#include "PngWriter.h"
#include "Settings.h"
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

// T0 for --measure_load_time: fires as soon as this module's static init runs,
// i.e. right after the process image and imported DLLs have been loaded.
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

    // This build is Windows-only (little-endian), so the native-order writers
    // are already little-endian.
    auto w16 = [&out] (uint16_t v) { out.writeShort ((short) v); };
    auto w32 = [&out] (uint32_t v) { out.writeInt ((int) v); };

    out.write ("RIFF", 4);
    w32 (36 + dataSize);
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    w32 (16);                    // fmt chunk size
    w16 (1);                     // PCM format tag
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
#endif
    }

    const juce::String getApplicationName() override { return "SpectroCraft"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
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
                    cur += c;
            }
            if (!cur.isEmpty())
                args.add (cur);
        }

        // --measure_load_time: record startup phase timings in a log file.
        // Must be armed before the Application ctor runs, since audio device
        // initialisation is the dominant startup cost.
        for (int i = 0; i < args.size(); ++i)
            if (args[i] == "--measure_load_time")
            {
                const juce::File preferred = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                                 .getParentDirectory()
                                                 .getChildFile ("SpectroCraft_load_time.log");
                const juce::File log = scStart::enable (preferred);
                std::printf ("load-time measurement enabled, log: %s\n",
                             log.getFullPathName().toRawUTF8());
                std::fflush (stdout);
            }

        // Audio files passed on the command line are collected here and
        // opened after the app is up - single-instance handling below may hand
        // them to a running instance instead.
        std::vector<juce::File> cliFiles;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--test-spectral")
            {
                if (i + 2 >= args.size())
                {
                    std::cout << "usage: --test-spectral <in.wav> <out.png> [w h t0 t1]\n";
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
                quit();
                return;
            }

            else if (args[i] == "--test-load")
            {
                if (i + 1 >= args.size())
                {
                    std::cout << "usage: --test-load <in.wav>\n";
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
                    quit();
                    return;
                }

                const bool wrote = writeFloatWav (out, nch, rate, data);
                std::cout << (wrote ? "wrote " : "failed to write ") << out.getFullPathName() << "\n";
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

                Application app;   // no GUI needed for the batch run
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
                    quit();
                    return;
                }

                std::cout << "total: "   << sum.total   << "\n"
                          << "done: "    << sum.done    << "\n"
                          << "skipped: " << sum.skipped << "\n"
                          << "failed: "  << sum.failed  << "\n";
                for (const auto& f : sum.failures)
                    std::cout << "  - " << f << "\n";
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
                const int rc = scRunUnitTests();
                std::cout << (rc == 0 ? "all unit tests passed\n" : "unit tests FAILED\n");
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
            else if (args[i].length() > 0 && !args[i].startsWith ("--"))
            {
                const juce::File f = resolvePath (args[i]);
                if (f.existsAsFile()) cliFiles.push_back (f);
            }
        }

        // Single-instance mode: when "Allow multiple instances" is off, hand
        // the command-line files to a running instance and quit this one.
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

        // Open the command-line files. Routing through MainWindow::openFile
        // gives large files their async load with a progress dialog, exactly
        // like File > Open.
        for (const auto& f : cliFiles)
            mainWindow->openFile (f);
    }

    void shutdown() override
    {
        if (forwarder != nullptr) forwarder->stopServer ();
        mainWindow.reset();
        app.reset();
    }

    void systemRequestedQuit() override
    {
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
                if (dlg.runModalLoop () != 1) return;   // stay open
            }
        }
        quit();
    }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<Application> app;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<sc::singleinstance::Coordinator> forwarder;   // file-forwarding server
};

START_JUCE_APPLICATION (SpectroCraftApplication)
